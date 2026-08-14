# Protocol notes

Detailed notes on the MCP protocol revisions this library implements and on
the design choices around them.

## Implemented revisions

The library implements **2026-07-28**, the sessionless, stateless revision.
The supported set is public: `mcp::supported_protocol_versions` (currently a
single entry).

### Modern path (2026-07-28)

- There is no `initialize` handshake. Every request carries its protocol
  version and client capabilities in `_meta`:
  `io.modelcontextprotocol/protocolVersion` and
  `io.modelcontextprotocol/clientCapabilities`. Missing fields are rejected
  with `-32602`; an unsupported version with `UnsupportedProtocolVersionError`
  (`-32022`) carrying `data.supported` and `data.requested`.
- `server/discover` advertises `supportedVersions`, `capabilities`, and
  `serverInfo`. It answers even for a version the server does not support,
  because it is the version-selection probe.
- Every result carries `resultType` (`"complete"`) and
  `_meta.io.modelcontextprotocol/serverInfo`. The cacheable list/read
  endpoints (`tools/list`, `prompts/list`, `resources/list`,
  `resources/templates/list`, `resources/read`, `server/discover`) also carry
  `ttlMs` and `cacheScope`, configurable on `mcp::Registry`
  (`cache_ttl_ms`, `cache_scope`).
- Notifications cannot be answered, so strict `_meta` validation applies to
  requests only.

### Legacy path (dual-era)

Most current clients (opencode, Claude Desktop, ...) still speak the older
protocol: an `initialize` handshake, then requests without the per-request
`_meta` fields. The server therefore also serves the legacy **2025-11-25**
path by default (`xmake` option `legacy2025`, define `MCPP_LEGACY_2025`):

- An `initialize` request (without modern `_meta`) is answered with the 2025
  handshake: `protocolVersion` `2025-11-25`, `capabilities`, `serverInfo`.
- Requests without the modern `_meta` are served per the legacy revision with
  plain 2025 results (no `resultType`/`_meta`/`ttlMs`/`cacheScope`).
- Clients that send the modern `_meta` get the full 2026-07-28 behavior.
- The era is a property of the process or session, not of an individual
  request.

This is the spec's own prescription for the transition: the 2026-07-28
versioning page defines a compatibility matrix and says a server "MAY
implement both behaviors" (dual-era). The official MCP TypeScript SDK v2
implements 2026-07-28, but shipped clients are still on the legacy v1 SDK.

To build **modern-only** (drop the legacy path), configure
`xmake f --legacy2025=n`. The legacy code is guarded by
`#ifdef MCPP_LEGACY_2025` in `include/mcpp/protocol.hpp`; once clients
migrate to 2026-07-28, disable the option and delete those blocks.

## Object-bound tools and the 2026 direction

The object-handle model mirrors where MCP has converged. In the 2026-07-28
revision, protocol-level sessions were removed; cross-call application state
is carried by explicit, server-minted handles passed as ordinary tool
arguments (SEP-2567 "Sessionless MCP via Explicit State Handles" and SEP-2575
"Make MCP Stateless"). That is exactly the `handle`/`object_handle` pattern
the library exposes: the token the model threads through `move(handle, ...)`,
`interact(handle, ...)`, etc. is the protocol's recommended way to express
stateful workflows without sessions.

## Binary transports

MCP's wire format is JSON-RPC 2.0 (UTF-8) and remains so in the 2026
revision. Custom transports must preserve the JSON-RPC message format. For
embedded or low-power targets the practical split is: MCP (JSON-RPC) is the
agent-facing contract, and a binary protocol (Cap'n Proto, CBOR,
flatbuffers) is the device-facing contract, joined by a translating bridge
(the same role `mcpp-bridge` plays as a byte relay, but doing schema-aware
translation instead).

The core library is deliberately parser-agnostic so the reflection/tool layer
survives a transport swap: `JsonView` + the pluggable `Parser` concept sit
between the protocol core and any concrete codec. The `binary` example is a
proof of concept that the same registered tools (object handles included) can
be driven over a real Cap'n Proto framing (schema outside the dataframe,
field ordinals in place of JSON keys), even though no MCP client speaks it
today.

Cap'n Proto / KJ does not force exceptions: the headers and generated code
compile under `-fno-exceptions -fno-rtti` (`KJ_NO_EXCEPTIONS`). One practical
caveat when consuming the xmake package: its `libkj.a` is built with
exceptions, so an internal KJ error path (for example a corrupted incoming
frame) still throws and terminates the process. A fully no-exceptions
deployment must build capnp itself with `-fno-exceptions`. Valid frames and
application-level errors (like a revoked handle) work fine either way.

## References

- Vendored spec material in `assets/specs/` (schemas, changelogs, SEPs, key
  doc pages); refresh with `tools/specs/fetch-specs.sh`.
- Changelog: https://modelcontextprotocol.io/specification/2026-07-28/changelog
- Versioning: `assets/specs/docs/2026-07-28/basic/versioning.mdx`
- Discovery: `assets/specs/docs/2026-07-28/server/discover.mdx`
- SEP-2567 (state handles): https://modelcontextprotocol.io/seps/2567-sessionless-mcp
- SEP-2575 (stateless): https://modelcontextprotocol.io/seps/2575-stateless-mcp
