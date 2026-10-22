// Object-handle demo: proves that non-static member-function tools operate on a
// caller-selected, registered object instance rather than a fabricated static
// instance. Also proves that a revoked handle makes later calls fail with a
// JSON-RPC error.
//
// Two layers are exercised:
//   1. Directly through the reflected `Action`s (raw tools/call lines) — fast,
//      parser-free checks of the invoker + object registry.
//   2. Through `mcp::Protocol` dispatch (same lines, real response envelope) —
//      proves the inline server path turns a `ToolResult` error into a proper
//      JSON-RPC `error` response and a success into a `result` envelope.
//
// Build/run via the mcpp example target `mcp-example-object` (xmake with
// `--with_demo=y`). No stdin needed.

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include <mcpp/mcpp.hpp>
#include "mini-json.hpp"

namespace object_demo {

// A stateful tool group: non-static member functions carry the implicit `this`,
// which reflection exposes as an injected `handle` first argument.
struct [[= mcp::tool_group{} ]] counter {
    [[= mcp::tool{ .description = mcp::str("Stores a value on the counter.") } ]]
    auto set(std::int64_t v) {
        state_ = v;
        return (struct{ bool ok; }){ true };
    }

    [[= mcp::tool{ .description = mcp::str("Returns the counter's stored value.") } ]]
    auto get() {
        return (struct{ std::int64_t value; }){ state_ };
    }

    std::int64_t state_ = 0;
};

// A static tool on a second group, to confirm static tools are untouched by the
// object-handle machinery (no injected handle).
struct [[= mcp::tool_group{} ]] statics {
    [[= mcp::tool{ .description = mcp::str("A static helper, no object.") } ]]
    static auto ping() {
        return (struct{ std::string pong; }){ "pong" };
    }
};

// A non-static prompt: member prompt functions are object-bound too, so their
// declared arguments include the injected `handle`.
struct [[= mcp::prompt_group{} ]] prompts {
    [[= mcp::tool{ .description = mcp::str("A member prompt bound to an object.") } ]]
    auto describe() {
        const std::string text = "Object-bound prompt.";
        return "[" + mcp::json_object({
            {"role", mcp::json_string("user")},
            {"content", mcp::json_object({
                {"type", mcp::json_string("text")},
                {"text", mcp::json_string(text)},
            })},
        }) + "]";
    }
};

// Tagged-group discovery: register_tagged_tools must pick up non-static groups.
namespace tagged_ns {
struct [[= mcp::tool_group{} ]] ticker {
    [[= mcp::tool{ .description = mcp::str("Ticks the counter.") } ]]
    auto tick() {
        return (struct{ std::int64_t n; }){ ++count_ };
    }

    std::int64_t count_ = 0;
};
} // namespace tagged_ns

// Param type coverage (D1): float, unsigned/signed integers, and enums (which
// marshal as their enumerator name on the wire).
enum class color { red, green, blue };

struct [[= mcp::tool_group{} ]] types {
    [[= mcp::tool{ .description = mcp::str("Sums two float params, returning a double.") } ]]
    static auto add_float(float a, float b) { return (struct{ double sum; }){ double(a) + double(b) }; }

    [[= mcp::tool{ .description = mcp::str("Returns an unsigned 64-bit value unchanged.") } ]]
    static auto identity_u64(std::uint64_t v) { return (struct{ std::uint64_t value; }){ v }; }

    [[= mcp::tool{ .description = mcp::str("Returns a signed 16-bit value widened.") } ]]
    static auto widen(std::int16_t v) { return (struct{ std::int64_t value; }){ v }; }

    [[= mcp::tool{ .description = mcp::str("Returns the colour as its enumerator name.") } ]]
    static color pick_color(color c) { return c; }
};

// Const-reference parameters: the invoker decays the reflected type before
// dispatching, so annotated functions keep their natural `const T&` signature.
struct [[= mcp::tool_group{} ]] refs {
    [[= mcp::tool{ .description = mcp::str("Echoes a const-reference string.") } ]]
    static std::string echo_ref(const std::string& text) { return text; }

    [[= mcp::tool{ .description = mcp::str("Negates a bool.") } ]]
    static bool negate(bool value) { return !value; }
};

// D4: std::optional, std::vector, and literal-struct parameters.
struct point { std::int64_t x; std::int64_t y; };

struct [[= mcp::tool_group{} ]] complex {
    [[= mcp::tool{ .description = mcp::str("Multiplies by an optional factor (default 1).") } ]]
    static auto scale(std::int64_t v, std::optional<std::int64_t> factor) {
        return (struct{ std::int64_t result; }){ v * factor.value_or(1) };
    }

    [[= mcp::tool{ .description = mcp::str("Sums a list of integers.") } ]]
    static auto sum(std::vector<std::int64_t> values) {
        std::int64_t acc = 0;
        for (auto x : values) acc += x;
        return (struct{ std::int64_t total; }){ acc };
    }

    [[= mcp::tool{ .description = mcp::str("Adds the coordinates of a point.") } ]]
    static auto offset(point p) {
        return (struct{ std::int64_t sum; }){ p.x + p.y };
    }
};

namespace {

std::atomic<int> failures{0};

void check(bool cond, const char* what) {
    if (!cond) {
        std::printf("object_demo: FAILED %s\n", what);
        failures.fetch_add(1);
    } else {
        std::printf("object_demo: ok      %s\n", what);
    }
}

// ---- direct Action-level helpers (parser-free, no protocol) -----------------

std::string call(const mcp::Action& tool, std::string_view name, const std::string& args_json) {
    const std::string line =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{\"name\":" +
        mcp::json_string(name) + ",\"arguments\":" + args_json + "}}";
    mcp::ActionContext ctx;
    return tool.call(line, ctx).text;
}

mcp::ToolResult call_raw(const mcp::Action& tool, std::string_view name, const std::string& args_json) {
    const std::string line =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{\"name\":" +
        mcp::json_string(name) + ",\"arguments\":" + args_json + "}}";
    mcp::ActionContext ctx;
    return tool.call(line, ctx);
}

// ---- Protocol-level helpers (dispatch through mcp::Protocol + mini parser) ---

struct captured {
    std::vector<std::string> lines;
};

void sink_write(void* ctx, std::string_view line) {
    static_cast<captured*>(ctx)->lines.push_back(std::string(line));
}

// 2026-07-28: every request carries `_meta.io.modelcontextprotocol/
// protocolVersion` + `clientCapabilities` in its params. Inject them into a
// raw request line so the harness tests the modern protocol.
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

struct protocol_harness {
    mcp::Protocol proto;
    captured out;
    mcp::Sink sink;

    protocol_harness(const mcp::Registry& reg, std::string_view name)
        : proto(reg, name, "0.1.0") {
        sink.write = &sink_write;
        sink.ctx = &out;
    }

    void dispatch(const std::string& line) {
        const std::string req = with_meta(line);
        mini::Document doc;
        if (!mini::parse(req, doc)) return;
        proto.dispatch(req, mini::root(doc), sink);
    }

    // Dispatch verbatim (no _meta injection) — for validation-failure tests.
    void dispatch_raw(const std::string& line) {
        mini::Document doc;
        if (!mini::parse(line, doc)) return;
        proto.dispatch(line, mini::root(doc), sink);
    }
};

void test_protocol_roundtrip() {
    counter a, b;
    mcp::object_handle<counter> ha(a);
    mcp::object_handle<counter> hb(b);

    mcp::Registry reg{
        mcp::register_tools<^^object_demo::counter, mini::parser>(),
        {},
        {},
        {},
    };
    protocol_harness h(reg, "object-demo");

    // server/discover: discloses the supported version + served capabilities
    // (this registry serves tools only), with the envelope fields.
    h.dispatch("{\"jsonrpc\":\"2.0\",\"id\":0,\"method\":\"server/discover\",\"params\":{}}");
    check(!h.out.lines.empty() &&
          h.out.lines.back().find("\"supportedVersions\":[\"2026-07-28\"]") != std::string::npos &&
          h.out.lines.back().find("\"resultType\":\"complete\"") != std::string::npos &&
          h.out.lines.back().find("\"ttlMs\":") != std::string::npos &&
          h.out.lines.back().find("\"tools\":{}") != std::string::npos &&
          h.out.lines.back().find("\"resources\"") == std::string::npos &&
          h.out.lines.back().find("\"prompts\"") == std::string::npos,
          "server/discover discloses supported version + served capabilities");

    // Per-request _meta validation: missing fields -> -32602, unsupported
    // version -> -32022 UnsupportedProtocolVersionError with data. In dual-era
    // builds a missing-_meta request is a legacy request (served), so the
    // -32602 check only holds for the strict modern-only build.
#ifndef MCPP_LEGACY_2025
    h.out.lines.clear();
    h.dispatch_raw("{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"tools/list\",\"params\":{}}");
    check(!h.out.lines.empty() && h.out.lines.back().find("-32602") != std::string::npos,
          "missing _meta protocolVersion -> -32602");
#endif
    h.out.lines.clear();
    h.dispatch_raw("{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/list\",\"params\":{\"_meta\":{\"io.modelcontextprotocol/protocolVersion\":\"2025-11-25\",\"io.modelcontextprotocol/clientCapabilities\":{}}}}");
    check(!h.out.lines.empty() &&
          h.out.lines.back().find("-32022") != std::string::npos &&
          h.out.lines.back().find("\"supported\":[\"2026-07-28\"]") != std::string::npos &&
          h.out.lines.back().find("\"requested\":\"2025-11-25\"") != std::string::npos,
          "unsupported version -> -32022 with data.supported/requested");

    // tools/list: non-static tools advertise the injected handle.
    h.dispatch("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\",\"params\":{}}");
    check(!h.out.lines.empty(), "tools/list responds");
    check(h.out.lines.back().find("\"handle\"") != std::string::npos,
          "tools/list schema exposes 'handle'");

    // Valid handle -> result envelope (no error).
    h.out.lines.clear();
    h.dispatch("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":{\"name\":\"set\",\"arguments\":{\"handle\":" +
               std::to_string(ha.token()) + ",\"v\":42}}}");
    check(!h.out.lines.empty() && h.out.lines.back().find("\"result\"") != std::string::npos &&
              h.out.lines.back().find("\"error\"") == std::string::npos,
          "valid handle -> result envelope");

    // Unknown handle -> JSON-RPC error.
    h.out.lines.clear();
    h.dispatch("{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":{\"name\":\"get\",\"arguments\":{\"handle\":123456789}}}");
    check(!h.out.lines.empty() && h.out.lines.back().find("\"error\"") != std::string::npos &&
              h.out.lines.back().find("-32602") != std::string::npos,
          "unknown handle -> JSON-RPC error (-32602)");

    // Missing handle -> JSON-RPC error (reads as token 0, never registered).
    h.out.lines.clear();
    h.dispatch("{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\",\"params\":{\"name\":\"get\",\"arguments\":{}}}");
    check(!h.out.lines.empty() && h.out.lines.back().find("\"error\"") != std::string::npos,
          "missing handle -> JSON-RPC error");

    // Revoked handle -> JSON-RPC error.
    mcp::object_token revoked = 0;
    {
        counter c;
        mcp::object_handle<counter> hc(c);
        revoked = hc.token();
    }   // hc destroyed here -> token revoked
    h.out.lines.clear();
    h.dispatch("{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"tools/call\",\"params\":{\"name\":\"get\",\"arguments\":{\"handle\":" +
               std::to_string(revoked) + "}}}");
     check(!h.out.lines.empty() && h.out.lines.back().find("\"error\"") != std::string::npos,
          "revoked handle -> JSON-RPC error");
}

// Dual-era: a legacy client (no modern `_meta`) gets the 2025-11-25
// initialize handshake and 2025-style results. Only compiled when the legacy
// path is enabled (MCPP_LEGACY_2025, the default).
#ifdef MCPP_LEGACY_2025
void test_legacy_2025() {
    static constexpr auto tools = mcp::register_tools<^^object_demo::counter, mini::parser>();
    counter a;
    mcp::object_handle<counter> ha(a);
    mcp::Registry reg{ tools, {}, {}, {} };
    protocol_harness h(reg, "legacy-2025");

    // initialize handshake (2025-11-25).
    h.dispatch_raw("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2025-11-25\",\"capabilities\":{},\"clientInfo\":{\"name\":\"x\",\"version\":\"1\"}}}");
    check(!h.out.lines.empty() &&
          h.out.lines.back().find("\"protocolVersion\":\"2025-11-25\"") != std::string::npos &&
          h.out.lines.back().find("\"capabilities\"") != std::string::npos,
          "legacy initialize handshake");

    // tools/list without _meta -> 2025-style result (no resultType).
    h.out.lines.clear();
    h.dispatch_raw("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\",\"params\":{}}");
    check(!h.out.lines.empty() &&
          h.out.lines.back().find("\"tools\"") != std::string::npos &&
          h.out.lines.back().find("\"resultType\"") == std::string::npos,
          "legacy tools/list has no resultType");

    // tools/call without _meta -> 2025-style result envelope (no resultType).
    h.out.lines.clear();
    h.dispatch_raw("{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":{\"name\":\"set\",\"arguments\":{\"handle\":" +
                   std::to_string(ha.token()) + ",\"v\":9}}}");
    check(!h.out.lines.empty() &&
          h.out.lines.back().find("\"result\"") != std::string::npos &&
          h.out.lines.back().find("\"resultType\"") == std::string::npos,
          "legacy tools/call has no resultType");
}
#endif

void test_registry_semantics() {
    counter x;
    mcp::object_handle<counter> h1(x);
    mcp::object_handle<counter> h2(x);   // second handle to the same object
    check(h1.token() != h2.token(), "two handles to one object have distinct tokens");
    check(mcp::object_registry<counter>::resolve(h1.token()) == &x, "h1 resolves to the object");
    check(mcp::object_registry<counter>::resolve(h2.token()) == &x, "h2 resolves to the object");

    // revoke -> null
    mcp::object_registry<counter>::revoke(h2.token());
    check(mcp::object_registry<counter>::resolve(h2.token()) == nullptr,
          "revoked handle resolves to null");

    // re-issue after revoke
    const mcp::object_token fresh = mcp::object_registry<counter>::issue(x);
    check(fresh != h1.token(), "re-issued token differs from the live one");
    check(mcp::object_registry<counter>::resolve(fresh) == &x, "re-issued token resolves");
    mcp::object_registry<counter>::revoke(fresh);

    // move semantics: moved-to keeps the token and revokes on destruction;
    // moved-from holds nothing and revokes nothing.
    mcp::object_token moved_t = 0;
    {
        counter y;
        mcp::object_handle<counter> src(y);
        moved_t = src.token();
        mcp::object_handle<counter> dst(std::move(src));
        check(dst.token() == moved_t, "move keeps the token");
        check(src.get() == nullptr, "moved-from handle holds nothing");
        check(mcp::object_registry<counter>::resolve(moved_t) == &y,
              "moved handle still resolves while alive");
    }   // dst destroyed here -> token revoked
    check(mcp::object_registry<counter>::resolve(moved_t) == nullptr,
          "moved handle revoked on destruction");

    // Token 0 is the "missing handle" convention (read_arg yields 0 for an
    // absent handle), so issue() must never mint it — otherwise a request with
    // no handle argument would silently resolve to that object.
    bool any_zero = false;
    for (int i = 0; i < 1000; ++i) {
        counter c;
        mcp::object_handle<counter> h(c);
        if (h.token() == 0) any_zero = true;
    }
    check(!any_zero, "issued tokens are never 0");
}

void test_unix_socket_path() {
    // Over-long paths must be rejected, not truncated into sun_path.
    const std::string too_long(300, 'a');
    check(mcp::listen_unix_socket(too_long) < 0,
          "over-long unix socket path is rejected");

    // A valid path still binds and listens.
    const std::string sock_path = "/tmp/mcpp-object-demo.sock";
    ::unlink(sock_path.c_str());
    const int fd = mcp::listen_unix_socket(sock_path);
    check(fd >= 0, "valid unix socket path binds");
    if (fd >= 0) ::close(fd);
    ::unlink(sock_path.c_str());
}

void test_concurrency() {
    constexpr int N = 8;
    std::vector<counter> objs(N);
    std::atomic<int> mismatches{0};
    std::vector<std::thread> threads;
    threads.reserve(N);
    for (int i = 0; i < N; ++i) {
        threads.emplace_back([&, i] {
            for (int k = 0; k < 500; ++k) {
                const mcp::object_token t = mcp::object_registry<counter>::issue(objs[i]);
                if (mcp::object_registry<counter>::resolve(t) != &objs[i]) mismatches.fetch_add(1);
                mcp::object_registry<counter>::revoke(t);
            }
        });
    }
    for (auto& t : threads) t.join();
    check(mismatches.load() == 0, "concurrent issue/resolve/revoke stays consistent");
}

// tools/list field regression: title and annotations must coexist on one tool;
// hint values follow the MCP schema defaults (destructive/openWorld = true).
void test_tools_list_fields() {
    static constexpr mcp::Action all[] = {
        { .name = "plain",  .description = "d", .input_schema = "{}" },
        { .name = "titled", .title = "A Title", .description = "d", .input_schema = "{}" },
        { .name = "hinty",  .description = "d", .input_schema = "{}", .is_read_only = true },
        { .name = "both",   .title = "A Title", .description = "d", .input_schema = "{}",
          .is_open_world = false },
    };
    mcp::Registry reg{ std::span<const mcp::Action>(all), {}, {}, {} };
    protocol_harness h(reg, "tools-list-fields");
    h.dispatch("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\",\"params\":{}}");
    check(!h.out.lines.empty(), "tools/list responds (field test)");
    const std::string& out = h.out.lines.back();

    // Slice out each tool entry (between its own "name" and the next tool's).
    auto entry = [&](const char* name) {
        const std::string marker = std::string("\"name\":\"") + name + "\"";
        const std::size_t start = out.find(marker);
        if (start == std::string::npos) return std::string_view();
        const std::size_t next = out.find("\"name\":\"", start + marker.size());
        const std::size_t end = next == std::string::npos ? out.size() : next;
        return std::string_view(out).substr(start, end - start);
    };

    const std::string_view plain = entry("plain");
    check(plain.find("\"title\"") == std::string::npos &&
          plain.find("\"annotations\"") == std::string::npos,
          "plain tool: no title, no annotations");
    const std::string_view titled = entry("titled");
    check(titled.find("\"title\":\"A Title\"") != std::string::npos &&
          titled.find("\"annotations\"") == std::string::npos,
          "title-only tool keeps title, no annotations");
    const std::string_view hinty = entry("hinty");
    check(hinty.find("\"title\"") == std::string::npos &&
          hinty.find("\"readOnlyHint\":true") != std::string::npos &&
          hinty.find("\"destructiveHint\":true") != std::string::npos &&
          hinty.find("\"openWorldHint\":true") != std::string::npos,
          "hint-only tool: annotations with schema-default hint values");
    const std::string_view both = entry("both");
    check(both.find("\"title\":\"A Title\"") != std::string::npos &&
          both.find("\"openWorldHint\":false") != std::string::npos &&
          both.find("\"readOnlyHint\":false") != std::string::npos,
          "title + hints tool keeps BOTH title and annotations (explicit values)");
}

// D1: read_arg covers all integrals, float, and enums (as enumerator names);
// schemas must match (number/integer/string+enum).
void test_arg_types() {
    static constexpr auto tools = mcp::register_tools<^^object_demo::types, mini::parser>();
    static_assert(tools.size() == 4);
    mcp::Registry reg{ tools, {}, {}, {} };
    protocol_harness h(reg, "arg-types");

    h.dispatch("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\",\"params\":{}}");
    const std::string& list = h.out.lines.back();
    check(list.find("\"name\":\"add_float\"") != std::string::npos &&
          list.find("\"a\":{\"type\":\"number\"}") != std::string::npos &&
          list.find("\"b\":{\"type\":\"number\"}") != std::string::npos,
          "float params schema is number");
    check(list.find("\"name\":\"identity_u64\"") != std::string::npos &&
          list.find("\"v\":{\"type\":\"integer\"}") != std::string::npos,
          "unsigned 64-bit param schema is integer");
    check(list.find("\"name\":\"widen\"") != std::string::npos &&
          list.find("\"v\":{\"type\":\"integer\"}") != std::string::npos,
          "signed 16-bit param schema is integer");
    check(list.find("\"name\":\"pick_color\"") != std::string::npos &&
          list.find("\"c\":{\"type\":\"string\",\"enum\":[\"red\",\"green\",\"blue\"]}") != std::string::npos,
          "enum param schema is a string enum list");

    // Round-trips (Action-level `call`: result text is the raw serialized
    // value; the mini parser reads integers and get_double covers float).
    auto by_name = [&](const char* n) -> const mcp::Action& {
        for (const auto& t : tools) if (std::string(t.name) == n) return t;
        __builtin_unreachable();
    };
    check(call(by_name("add_float"), "add_float", "{\"a\":3,\"b\":4}").find("\"sum\":7") != std::string::npos,
          "float read round-trips");
    check(call(by_name("identity_u64"), "identity_u64", "{\"v\":9007199254740993}").find("\"value\":9007199254740993") != std::string::npos,
          "uint64 read round-trips");
    check(call(by_name("widen"), "widen", "{\"v\":-123}").find("\"value\":-123") != std::string::npos,
          "int16 read round-trips");
    check(call(by_name("pick_color"), "pick_color", "{\"c\":\"green\"}").find("\"green\"") != std::string::npos,
          "enum read round-trips by name");
    check(call(by_name("pick_color"), "pick_color", "{\"c\":\"purple\"}").find("\"red\"") != std::string::npos,
          "unknown enum name falls back to enumerator 0");
}

// Const-reference parameters and bool schema/round-trip coverage.
void test_const_ref_and_bool() {
    static constexpr auto tools = mcp::register_tools<^^object_demo::refs, mini::parser>();
    static_assert(tools.size() == 2);
    mcp::Registry reg{ tools, {}, {}, {} };
    protocol_harness h(reg, "refs");

    h.dispatch("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\",\"params\":{}}");
    const std::string& list = h.out.lines.back();
    check(list.find("\"name\":\"echo_ref\"") != std::string::npos &&
              list.find("\"text\":{\"type\":\"string\"}") != std::string::npos,
          "const-reference string schema is string");
    check(list.find("\"name\":\"negate\"") != std::string::npos &&
              list.find("\"value\":{\"type\":\"boolean\"}") != std::string::npos,
          "bool schema is boolean");

    auto by_name = [&](const char* n) -> const mcp::Action& {
        for (const auto& t : tools)
            if (std::string(t.name) == n) return t;
        __builtin_unreachable();
    };
    check(call(by_name("echo_ref"), "echo_ref", "{\"text\":\"hi\"}").find("\"hi\"") !=
              std::string::npos,
          "const-reference string round-trips");
    check(call(by_name("negate"), "negate", "{\"value\":true}").find("false") !=
              std::string::npos,
          "bool round-trips");
}

// D4: std::optional / std::vector / literal-struct parameters — schemas and
// round-trips (through the mini parser, which now parses arrays).
void test_arg_types_d4() {
    static constexpr auto tools = mcp::register_tools<^^object_demo::complex, mini::parser>();
    static_assert(tools.size() == 3);
    mcp::Registry reg{ tools, {}, {}, {} };
    protocol_harness h(reg, "arg-types-d4");

    h.dispatch("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\",\"params\":{}}");
    const std::string& list = h.out.lines.back();
    check(list.find("\"name\":\"scale\"") != std::string::npos &&
          list.find("\"factor\":{\"type\":\"integer\"}") != std::string::npos &&
          list.find("\"required\":[\"v\"]") != std::string::npos,
          "optional param schema unwraps and is not required");
    check(list.find("\"name\":\"sum\"") != std::string::npos &&
          list.find("\"values\":{\"type\":\"array\",\"items\":{\"type\":\"integer\"}}") != std::string::npos,
          "vector param schema is an array");
    check(list.find("\"name\":\"offset\"") != std::string::npos &&
          list.find("\"p\":{\"type\":\"object\",\"properties\":{\"x\":{\"type\":\"integer\"},\"y\":{\"type\":\"integer\"}},\"required\":[\"x\",\"y\"]}") != std::string::npos,
          "struct param schema is an object");

    auto by_name = [&](const char* n) -> const mcp::Action& {
        for (const auto& t : tools) if (std::string(t.name) == n) return t;
        __builtin_unreachable();
    };
    check(call(by_name("scale"), "scale", "{\"v\":5}").find("\"result\":5") != std::string::npos,
          "optional absent -> default");
    check(call(by_name("scale"), "scale", "{\"v\":5,\"factor\":3}").find("\"result\":15") != std::string::npos,
          "optional present -> value");
    check(call(by_name("sum"), "sum", "{\"values\":[1,2,3]}").find("\"total\":6") != std::string::npos,
          "vector arg round-trips");
    check(call(by_name("offset"), "offset", "{\"p\":{\"x\":2,\"y\":3}}").find("\"sum\":5") != std::string::npos,
          "struct arg round-trips");
}

} // namespace

int run() {
    static constexpr auto actions = mcp::register_tools<^^object_demo::counter, mini::parser>();
    static constexpr auto statics = mcp::register_tools<^^object_demo::statics, mini::parser>();
    static constexpr auto prompts_arr = mcp::register_prompts<^^object_demo::prompts, mini::parser>();
    static constexpr auto tagged =
        mcp::register_tagged_tools<^^object_demo::tagged_ns, ^^mcp::tool_group, mini::parser>();
    static_assert(actions.size() == 2);
    static_assert(statics.size() == 1);
    static_assert(prompts_arr.size() == 1);
    static_assert(tagged.size() == 1);

    const mcp::Action* set_tool = &actions[0];
    const mcp::Action* get_tool = &actions[1];
    if (set_tool->name != "set") std::swap(set_tool, get_tool);

    // The schema for a non-static tool exposes the injected handle.
    check(set_tool->input_schema.find("\"handle\"") != std::string::npos,
          "non-static tool schema exposes 'handle'");
    check(statics[0].input_schema.find("\"handle\"") == std::string::npos,
          "static tool schema has no 'handle'");
    check(std::string(set_tool->name) == "set" && std::string(get_tool->name) == "get",
          "tool names are the original function names");
    check(prompts_arr[0].arguments_schema.find("\"handle\"") != std::string::npos,
          "non-static prompt arguments include 'handle'");
    check(tagged[0].input_schema.find("\"handle\"") != std::string::npos,
          "tagged-discovered tool schema exposes 'handle'");
    check(std::string(tagged[0].name) == "tick", "tagged tool keeps the original name");

    counter a, b;
    mcp::object_handle<counter> ha(a);
    mcp::object_handle<counter> hb(b);
    check(ha.token() != hb.token(), "tokens are distinct");

    // Independent per-instance state: set on a, read a and b.
    const std::string set_a = "{\"handle\":" + std::to_string(ha.token()) + ",\"v\":42}";
    const std::string set_b = "{\"handle\":" + std::to_string(hb.token()) + ",\"v\":7}";
    check(call(*set_tool, "set", set_a).find("\"ok\":true") != std::string::npos, "set on A");
    check(call(*set_tool, "set", set_b).find("\"ok\":true") != std::string::npos, "set on B");

    const std::string get_a = "{\"handle\":" + std::to_string(ha.token()) + "}";
    const std::string get_b = "{\"handle\":" + std::to_string(hb.token()) + "}";
    check(call(*get_tool, "get", get_a).find("\"value\":42") != std::string::npos, "get on A returns 42");
    check(call(*get_tool, "get", get_b).find("\"value\":7") != std::string::npos, "get on B returns 7");

    // Static tool still works with no handle argument.
    check(call(statics[0], "ping", "{}").find("\"pong\":\"pong\"") != std::string::npos, "static ping works");

    // Unknown handle -> JSON-RPC error.
    const mcp::ToolResult unknown =
        call_raw(*get_tool, "get", "{\"handle\":123456789}");
    check(unknown.is_error, "unknown handle is an error");
    check(unknown.error_code == mcp::RPC_INVALID_PARAMS, "unknown handle error code");

    // Revoked handle -> JSON-RPC error. RAII revocation: the handle is revoked
    // when the scope's object_handle is destroyed.
    mcp::object_token revoked_token = 0;
    {
        counter c;
        mcp::object_handle<counter> hc(c);
        revoked_token = hc.token();
        const std::string get_c = "{\"handle\":" + std::to_string(revoked_token) + "}";
        check(call(*get_tool, "get", get_c).find("\"value\":0") != std::string::npos,
              "get on C before revocation");
    }   // hc destroyed here -> token revoked
    const mcp::ToolResult revoked = call_raw(*get_tool, "get", "{\"handle\":" + std::to_string(revoked_token) + "}");
    check(revoked.is_error, "revoked handle is an error");

    // Higher-level layers.
    test_protocol_roundtrip();
#ifdef MCPP_LEGACY_2025
    test_legacy_2025();
#endif
    test_registry_semantics();
    test_concurrency();
    test_tools_list_fields();
    test_unix_socket_path();
    test_arg_types();
    test_arg_types_d4();
    test_const_ref_and_bool();

    if (failures.load() == 0) std::printf("object_demo: ALL PASS\n");
    return failures.load() == 0 ? 0 : 1;
}

} // namespace object_demo

int main() {
    return object_demo::run();
}
