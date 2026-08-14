---
name: mcp-cpp-refactor
description: Add MCP server support to a C++26 or later codebase using the mcpp library. 
             Expose existing C++ operations as MCP tools, prompts, and resources.
---

# Adding MCP support with mcpp

mcpp turns existing C++ functions into a Model Context Protocol (MCP) server.
You annotate tool groups, and reflection derives tool names, JSON schemas, and
invocation, so there is no hand-written registry or marshalling. The user does
not write these annotations by hand; you do, guided by this skill.

The library is the authority. Its headers, `docs/protocol.md`, and the bundled
examples change faster than this skill. If a request does not fit an existing
interface, or the scope is unclear, ask the user. Do not invent new shapes and
do not modify the library itself.

## 1. Work in the current repo

- Commit a baseline first, so you can diff and revert.
- Edit the project's real files in place. No side projects or throwaway copies.
- Use the repo's existing build system and targets.

## 2. Assess before editing

Ask the user when unclear; do not silently pick a shape.

- **Toolchain.** The project must compile with C++26 and reflection (gcc 16+
  with `-freflection`, or an equivalent clang). Verify it, or stop and say the
  library cannot help on this toolchain.
- **How the library is available.** See "Getting the library".
- **How the server will run.** Per-process over stdio, or an already-running
  app exposed over a unix socket (`SocketHost`). HTTP is not trivially
  supported.
- **What to expose.** Which existing functions and state become tools,
  prompts, and resources. For sensitive operations, suggest an auth policy
  instead of exposing them to every session.

## 3. Getting the library

Preferred: the library is already installed with `xmake install -o <prefix>`,
to a user prefix (`~/.local`) or `/usr/local`. Find the prefix with
`command -v mcpp-bridge` (it lives in `<prefix>/bin`), or ask the user.

The installed layout:

- headers: `<prefix>/include/mcpp/` and `<prefix>/include/mcpp-server/`
- static library: `<prefix>/lib/libmcpp-server.a`
- tools: `<prefix>/bin/mcpp-bridge`, `<prefix>/bin/mcpp-skill`

Consumers compile against the installed headers and link the static library
with `-freflection` (required: the headers carry reflection splices) and, when
using `mcpp-server`, `-DSIMDJSON_EXCEPTIONS=0` (keeps simdjson headers
consistent with the prebuilt exception-free library), plus simdjson and fmt.
fmt is not part of the public headers, so a consumer's own fmt usage is never
affected. The library is built exception-free by default, but that is its own
profile: downstream applications keep their exceptions and RTTI. If the
application uses exceptions itself (e.g. its own fmt or simdjson with
exceptions), require the package with the `exceptions` config enabled so the
library is built and linked exception-enabled, with no exception defines
forced on the consumer.

If there is no install and the project does not use xmake, vendor the source
(`include/` + `lib/mcpp-server/`) into the project as a last resort.

The protocol needs no work from you: the server speaks 2026-07-28 and, by
default, the legacy 2025-11-25 handshake, so both old and new clients connect.

## 4. The basic shape

A tool group is a struct whose member functions are tools. Annotate and
register it; reflection does the rest.

```cpp
#include <mcpp/mcpp.hpp>
#include <mcpp-server/mcpp-server.hpp>

namespace app::groups {

struct [[= mcp::tool_group{} ]] ops {
    [[= mcp::tool{ .description = mcp::str("Adds two integers.") } ]]
    static auto add(std::int64_t a, std::int64_t b) {
        return (struct{ std::int64_t sum; }){ a + b };
    }
};

} // namespace app::groups

mcp::Registry registry() {
    return mcp::Registry{
        .actions = mcp::register_tagged_tools<^^app::groups, ^^mcp::tool_group, mcp::simdjson_parser>(),
        .resources = {}, .prompts = {}, .resource_templates = {},
    };
}

int main() {
    mcp::StdioStream stream;
    mcp::Server server(stream, registry());
    server.run();
}
```

Notes:

- `mcp::str("...")` is required for annotation values; a plain string does
  not work.
- Return natural values, not JSON text. A simple result can be an anonymous
  struct.
- Parameters can be any integral, floating point, `bool`, `std::string`, an
  enum (marshalled as its enumerator name), `std::optional<T>`,
  `std::vector<T>`, or a literal struct. `std::string_view` and `const char*`
  are not supported: the parsed document does not outlive the call.
- Non-static tools are object-bound. The client sends an injected `handle`
  token; the app registers the live object with `mcp::object_handle<T>`, and
  the member runs on that instance. An unknown or revoked handle returns a
  JSON-RPC error. Member state persists per instance.
- A trailing `mcp::ActionContext&` is injected for async tools, progress, and
  cancellation. Mark the tool `.async = true`.

## 5. Beyond the basics

For prompts, resources, completion hooks, auth (`mcp::with<...>`), custom
serializers, and the unix-socket host, read the bundled examples and the
library headers first. The full `app` example covers SocketHost, auth,
prompts, resources, and admin policies. `docs/protocol.md` covers the wire.
The vendored specs under `assets/specs/` cover protocol questions.

## 6. Verify

1. Build with the project's build command.
2. Smoke-test: `initialize` (or `tools/list` on a modern client), confirm
   `tools/list` shows the expected tools, and `tools/call` round-trips. For
   object-bound tools, pass the registered `handle` and confirm a revoked
   handle returns a JSON-RPC error.
3. If the mcpp repo is at hand, `xmake test` runs the self-checking demos.
4. If the user changed any client config, remind them to restart the client.
