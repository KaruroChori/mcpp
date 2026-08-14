// Async-path object-handle demo: proves the threaded `mcpp-server` worker
// (Server::on_async_call) also honors ToolResult errors from non-static tools —
// i.e. a call with a revoked handle returns a real JSON-RPC error through the
// async worker, not just through the inline protocol path.
//
// Unlike object_demo.cpp this links mcpp-server (simdjson + threads + a real
// Stream): an async tool needs the Server's worker pool, so it cannot run in
// the parser-free core demo. A `mcp::Server` is driven over a socketpair and
// the client side asserts the envelopes it gets back.
//
// Build/run via the mcpp example target `mcp-example-async` (xmake with
// `--with_demo=y`).

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include <sys/socket.h>
#include <unistd.h>

#include <mcpp-server/mcpp-server.hpp>

namespace async_demo {

namespace {

// Set by the slow tool once a worker is actually in its loop, so the test can
// send the cancellation only once the tool is provably in-flight.
std::atomic<bool> started{false};

struct cancelled_result { std::string status; std::int64_t progress; std::int64_t total; };
struct complete_result { std::string status; std::int64_t seconds; std::int64_t total; };

} // namespace

// A stateful, async tool group. `.async = true` makes Server::on_async_call
// run the tool on a worker thread; the injected `handle` still selects which
// registered instance it mutates.
struct [[= mcp::tool_group{} ]] counter {
    [[= mcp::tool{ .description = mcp::str("Async tick: returns the counter's stored value."), .async = true } ]]
    auto tick() {
        return (struct{ std::int64_t value; }){ state_ };
    }

    [[= mcp::tool{ .description = mcp::str("Stores a value on the counter."), .async = true } ]]
    auto set(std::int64_t v) {
        state_ = v;
        return (struct{ bool ok; }){ true };
    }

    // Static async tool that honours cancellation via ctx.cancelled; the test
    // below cancels it mid-flight and asserts the cancelled variant comes back.
    [[= mcp::tool{ .description = mcp::str("Long-running async tool that honours cancellation via ctx.cancelled."), .async = true } ]]
    static std::variant<cancelled_result, complete_result> slow(std::int64_t iterations, mcp::ActionContext& ctx) {
        for (int64_t i = 0; i < iterations; ++i) {
            if (ctx.cancelled && ctx.cancelled->load()) {
                return cancelled_result{ "cancelled", i, iterations };
            }
            started.store(true);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return complete_result{ "complete", iterations, iterations };
    }

    std::int64_t state_ = 0;
};

// D1: float-literal round-trip through the simdjson backend (the mini parser
// rejects float literals by design, so this lives here instead of object_demo).
struct [[= mcp::tool_group{} ]] argtypes {
    [[= mcp::tool{ .description = mcp::str("Multiplies a float parameter by two.") } ]]
    static auto scale(float factor) { return (struct{ double result; }){ double(factor) * 2.0 }; }

    // D4: vector parameter round-trip through simdjson (real JSON array).
    [[= mcp::tool{ .description = mcp::str("Sums a list of integers.") } ]]
    static auto sum_list(std::vector<std::int64_t> values) {
        std::int64_t acc = 0;
        for (auto x : values) acc += x;
        return (struct{ std::int64_t total; }){ acc };
    }
};

namespace {

int failures = 0;

void check(bool cond, const char* what) {
    if (!cond) {
        std::printf("async_demo: FAILED %s\n", what);
        ++failures;
    } else {
        std::printf("async_demo: ok      %s\n", what);
    }
}

// Simple client-side fd helpers (raw newline-delimited frames over the socket).
std::string read_line(int fd) {
    std::string pending;
    while (true) {
        auto nl = pending.find('\n');
        if (nl != std::string::npos) {
            std::string line = pending.substr(0, nl);
            pending.erase(0, nl + 1);
            return line;
        }
        char buf[4096];
        const ssize_t n = ::recv(fd, buf, sizeof buf, 0);
        if (n <= 0) return pending;   // EOF / error: flush what we have
        pending.append(buf, static_cast<size_t>(n));
    }
}

void write_line(int fd, const std::string& line) {
    std::string with_nl = line + "\n";
    size_t off = 0;
    while (off < with_nl.size()) {
        const ssize_t n = ::send(fd, with_nl.data() + off, with_nl.size() - off, MSG_NOSIGNAL);
        if (n < 0) return;
        off += static_cast<size_t>(n);
    }
}

// 2026-07-28: every request carries `_meta.io.modelcontextprotocol/
// protocolVersion` + `clientCapabilities` in its params.
std::string with_meta(std::string_view line) {
    constexpr std::string_view marker = R"("params":{)";
    const std::size_t pos = line.find(marker);
    if (pos == std::string_view::npos) return std::string(line);
    constexpr std::string_view meta =
        R"("_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientCapabilities":{}})";
    std::string out;
    out.reserve(line.size() + meta.size() + 1);
    out.append(line.substr(0, pos + marker.size()));
    out.append(meta);
    const std::size_t after = pos + marker.size();
    if (after < line.size() && line[after] != '}') out += ",";
    out.append(line.substr(after));
    return out;
}

} // namespace

int run() {
    static constexpr auto actions =
        mcp::register_tools<^^async_demo::counter, mcp::simdjson_parser>();
    static_assert(actions.size() == 3);

    // Live instance + one that is revoked before we talk to the server.
    counter live;
    mcp::object_handle<counter> hlive(live);
    const mcp::object_token live_token = hlive.token();

    mcp::object_token revoked_token = 0;
    {
        counter doomed;
        mcp::object_handle<counter> hdoomed(doomed);
        revoked_token = hdoomed.token();
    }   // hdoomed destroyed -> revoked

    // Socketpair: one end becomes the server's stream, the other the client.
    int fds[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        std::printf("async_demo: FAILED socketpair\n");
        return 1;
    }

    mcp::UnixSocketStream stream(fds[0]);
    static constexpr auto counters = mcp::register_tools<^^async_demo::counter, mcp::simdjson_parser>();
    static constexpr auto scalers  = mcp::register_tools<^^async_demo::argtypes, mcp::simdjson_parser>();
    static constexpr mcp::Action all[] = { counters[0], counters[1], counters[2], scalers[0], scalers[1] };
    mcp::Registry reg{ std::span<const mcp::Action>(all), {}, {}, {} };
    // Small worker pool (2) so the queueing path is exercised by the
    // concurrent-calls test below.
    mcp::Server server(stream, reg, "async-demo", "0.1.0", nullptr, nullptr, 2);

    std::thread server_thread([&server] { server.run(); });

    // Discovery (replaces the legacy initialize handshake).
    write_line(fds[1], with_meta("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"server/discover\",\"params\":{}}"));
    const std::string disc_resp = read_line(fds[1]);
    check(disc_resp.find("\"supportedVersions\":[\"2026-07-28\"]") != std::string::npos,
          "server/discover reports the supported version");

    // Async tool on a live handle -> result envelope (async worker path).
    write_line(fds[1], with_meta("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":{\"name\":\"set\",\"arguments\":{\"handle\":" +
                       std::to_string(live_token) + ",\"v\":42}}}"));
    const std::string ok_resp = read_line(fds[1]);
    check(ok_resp.find("\"result\"") != std::string::npos &&
              ok_resp.find("\"error\"") == std::string::npos,
          "async tool with live handle -> result envelope");
    check(ok_resp.find("\\\"ok\\\":true") != std::string::npos, "async set applied on live object");

    // Async tool on a revoked handle -> JSON-RPC error (async worker path).
    write_line(fds[1], with_meta("{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":{\"name\":\"tick\",\"arguments\":{\"handle\":" +
                       std::to_string(revoked_token) + "}}}"));
    const std::string err_resp = read_line(fds[1]);
    check(err_resp.find("\"error\"") != std::string::npos &&
              err_resp.find("-32602") != std::string::npos,
          "async tool with revoked handle -> JSON-RPC error (-32602)");

    // Cancellation: a `notifications/cancelled` for an in-flight async tool
    // sets its cancel flag; the tool observes it and returns the cancelled
    // variant. We wait until the worker is provably in its loop (`started`)
    // before sending the notification, so the test is not racy.
    started.store(false);
    write_line(fds[1], with_meta("{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\",\"params\":{\"name\":\"slow\",\"arguments\":{\"iterations\":1000}}}"));
    while (!started.load()) std::this_thread::yield();
    write_line(fds[1], "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/cancelled\",\"params\":{\"requestId\":4}}");
    const std::string cancel_resp = read_line(fds[1]);
    check(cancel_resp.find("\"result\"") != std::string::npos &&
              cancel_resp.find("cancelled") != std::string::npos,
          "cancelled notification stops the in-flight async tool");

    // Float literal round-trip through simdjson (2.5 * 2 -> 5.0 -> "5").
    write_line(fds[1], with_meta("{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"tools/call\",\"params\":{\"name\":\"scale\",\"arguments\":{\"factor\":2.5}}}"));
    const std::string scale_resp = read_line(fds[1]);
    check(scale_resp.find("\"result\"") != std::string::npos &&
              scale_resp.find("\\\"result\\\":5") != std::string::npos,
          "float literal round-trips (scale)");

    // Pool: more concurrent async calls than workers (3 > 2) — all complete;
    // the extra call is queued until a worker frees up.
    write_line(fds[1], with_meta("{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"tools/call\",\"params\":{\"name\":\"slow\",\"arguments\":{\"iterations\":5}}}"));
    write_line(fds[1], with_meta("{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/call\",\"params\":{\"name\":\"slow\",\"arguments\":{\"iterations\":5}}}"));
    write_line(fds[1], with_meta("{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"tools/call\",\"params\":{\"name\":\"slow\",\"arguments\":{\"iterations\":5}}}"));
    int completes = 0;
    for (int i = 0; i < 3; ++i) {
        const std::string r = read_line(fds[1]);
        if (r.find("complete") != std::string::npos) ++completes;
    }
    check(completes == 3, "pool serves more concurrent async calls than workers");

    // Vector arg round-trip through simdjson (real JSON array).
    write_line(fds[1], with_meta("{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"tools/call\",\"params\":{\"name\":\"sum_list\",\"arguments\":{\"values\":[1,2,3]}}}"));
    const std::string vec_resp = read_line(fds[1]);
    check(vec_resp.find("\"result\"") != std::string::npos &&
              vec_resp.find("\\\"total\\\":6") != std::string::npos,
          "vector arg round-trips through simdjson");

    // Close the client side; the server reads EOF and exits.
    ::shutdown(fds[1], SHUT_RDWR);
    ::close(fds[1]);
    server_thread.join();

    if (failures == 0) std::printf("async_demo: ALL PASS\n");
    return failures == 0 ? 0 : 1;
}

} // namespace async_demo

int main() {
    return async_demo::run();
}
