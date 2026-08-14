// Binary-transport proof of concept: the same reflection/tool layer that serves
// JSON MCP can be driven over a Cap'n Proto framing where the schema lives
// outside the dataframe. We register a tool group with the Cap'n Proto Parser
// backend, pack a tools/call request into a Cap'n Proto message, and call the
// generated `Action`s directly — proving the mcpp tool layer is transport-
// agnostic.
//
// Build/run via the mcpp example target `mcp-example-binary` (xmake with
// `--with_demo=y`). No stdin needed.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <capnp/message.h>
#include <capnp/serialize.h>

#include <mcpp/mcpp.hpp>

#include "capnp-parser.hpp"
#include "tools.capnp.h"

namespace binary_demo {

// A stateful tool group. Non-static so the injected `handle` is part of the
// request, proving the object-handle model survives the transport swap too.
//
// The tool parameters are named exactly like the schema fields in tools.capnp
// (radius, payload): the schema IS the argument contract, and the wire carries
// no keys — the reflection layer reads the params straight out of the struct.
struct [[= mcp::tool_group{} ]] counter {
    [[= mcp::tool{ .description = mcp::str("Stores a value on the counter.") } ]]
    auto set(std::int64_t radius, std::string payload) {
        state_ = radius;
        note_ = std::move(payload);
        return (struct{ bool ok; }){ true };
    }

    [[= mcp::tool{ .description = mcp::str("Returns the counter's stored value and note.") } ]]
    auto get() {
        return (struct{ std::int64_t value; std::string note; }){ state_, note_ };
    }

    std::int64_t state_ = 0;
    std::string note_;
};

namespace {

int failures = 0;

void check(bool cond, const char* what) {
    if (!cond) {
        std::printf("binary_demo: FAILED %s\n", what);
        ++failures;
    } else {
        std::printf("binary_demo: ok      %s\n", what);
    }
}

// Pack a CallRequest (tool name + arguments) into a byte string.
std::string pack(std::string_view name, uint64_t handle, const std::string& dir,
                 std::int64_t radius, const std::string& payload) {
    capnp::MallocMessageBuilder message;
    auto req = message.initRoot<CallRequest>();
    auto params = req.initParams();
    params.setName(kj::StringPtr(name.data(), name.size()).begin());
    auto args = params.initArguments();
    args.setHandle(handle);
    args.setDirection(kj::StringPtr(dir.data(), dir.size()));
    args.setRadius(static_cast<int32_t>(radius));
    args.setPayload(kj::StringPtr(payload.data(), payload.size()));

    kj::Array<capnp::word> flat = capnp::messageToFlatArray(message);
    const char* base = reinterpret_cast<const char*>(flat.begin());
    return std::string(base, flat.size() * sizeof(capnp::word));
}

mcp::ToolResult call_tool(const mcp::Action& tool, const std::string& bytes) {
    mcp::ActionContext ctx;
    return tool.call(bytes, ctx);
}

} // namespace

int run() {
    static constexpr auto actions =
        mcp::register_tools<^^binary_demo::counter, binary::capnp_parser>();
    static_assert(actions.size() == 2);

    const mcp::Action* set_tool = &actions[0];
    const mcp::Action* get_tool = &actions[1];
    if (set_tool->name != "set") std::swap(set_tool, get_tool);

    // Schema lives outside the dataframe: the tool's declared JSON schema still
    // advertises `handle`, but the actual request bytes are Cap'n Proto.
    check(set_tool->input_schema.find("\"handle\"") != std::string::npos,
          "tool schema still exposes 'handle'");

    counter c;
    mcp::object_handle<counter> hc(c);

    // set(radius=42, payload="hello") over Cap'n Proto.
    const mcp::ToolResult set_res =
        call_tool(*set_tool, pack("set", hc.token(), "north", 42, "hello"));
    check(!set_res.is_error && set_res.text.find("\"ok\":true") != std::string::npos,
          "set(radius=42, payload=hello) over Cap'n Proto frame");

    // get() over Cap'n Proto -> value=42, note=hello.
    const mcp::ToolResult get_res =
        call_tool(*get_tool, pack("get", hc.token(), "", 0, ""));
    check(!get_res.is_error && get_res.text.find("\"value\":42") != std::string::npos &&
              get_res.text.find("\"note\":\"hello\"") != std::string::npos,
          "get() reads 42 + hello back over Cap'n Proto frame");

    // Revoked handle -> clean JSON-RPC error through the same frame.
    const mcp::object_token revoked_token = hc.token();
    mcp::object_registry<counter>::revoke(revoked_token);
    const mcp::ToolResult revoked =
        call_tool(*get_tool, pack("get", revoked_token, "", 0, ""));
    check(revoked.is_error && revoked.error_code == mcp::RPC_INVALID_PARAMS,
          "revoked handle -> error over Cap'n Proto");

    if (failures == 0) std::printf("binary_demo: ALL PASS\n");
    return failures == 0 ? 0 : 1;
}

} // namespace binary_demo

int main() {
    return binary_demo::run();
}
