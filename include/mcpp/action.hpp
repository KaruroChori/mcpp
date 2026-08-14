#pragma once

/// @file action.hpp
/// @brief Tool and capability metadata types (Action, Resource, Prompt, Registry, decorators).
/// SPDX-License-Identifier: LGPL-3.0

#include <atomic>
#include <functional>
#include <meta>
#include <span>
#include <string>
#include <string_view>

namespace mcp {

/// Default result serializer, declared here so `tool` can default to it; the
/// primary template is defined in serializer.hpp. Custom serializers satisfy
/// the same shape: `template <typename T> struct S { static std::string
/// to_json(const T&); }`.
template <typename T>
struct json_serializer;

/// Runtime context handed to a tool call: cancellation and progress reporting.
struct ActionContext {
    const std::atomic<bool>* cancelled = nullptr;    ///< Non-null when the request is cancellable; poll to observe cancellation.
    bool has_progress_token = false;                 ///< Whether the caller supplied a progress token.
    int64_t progress_token = 0;                      ///< Opaque progress token to attach to progress notifications.
    std::function<void(std::string_view line)> send; ///< Sends a JSON-RPC line (for example a progress notification).
};

/// Exact-size structural string, used for annotation values. Annotation
/// values must be structural on this compiler, so a string cannot be a
/// `string_view`. `str<N>` holds exactly `N` characters; `N == 1` denotes the
/// empty string (a single NUL).
/// @tparam N  The string length.
template <std::size_t N>
struct str {
    char data[N]{};   ///< The characters, NUL padded.
    constexpr str() = default;
    constexpr str(const char (&s)[N]) {
        for (std::size_t i = 0; i < N; ++i) data[i] = s[i];
    }
};
template <std::size_t N>
str(const char (&)[N]) -> str<N>;

/// Binds a completion hook to the prompt it completes, overriding the default
/// `<prompt>_complete` naming convention. Carried as the decorator on the
/// hook function itself; the value is a direct reference to the prompt
/// function, so no string is involved:
///
///     struct prompt_group {
///         [[= mcp::tool{ .description = mcp::str("...") } ]]
///         static std::string summarize_changes(std::string base);
///
///         [[= mcp::complete_for{ .prompt = ^^prompt_group::summarize_changes } ]]
///         static std::string refs(std::string_view arg, std::string_view value);
///     };
///
/// Without the decorator, the hook is found by the default name convention
/// `<prompt>_complete`.
struct complete_for {
    std::meta::info prompt;   ///< The reflected prompt function this hook completes.
};

// ---------------------------------------------------------------------------
// Authorization. A policy is a type with `static bool authorized(const void*
// ctx)`; it decides whether a capability is enabled for a given session. The
// context is opaque to the library, the project defines what it carries (a
// permission level, a token, capabilities). `no_auth` always allows.
// ---------------------------------------------------------------------------

/// Default policy: always authorized (unguarded capability).
struct no_auth {
    static bool authorized(const void*) { return true; }
};

/// Authorization callback: returns whether a session context allows the
/// capability.
/// @param ctx  The session's opaque context.
/// @return True when authorized.
using auth_fn = bool (*)(const void* ctx);

/// Result of authenticating the first line of a connection.
/// @see authenticator_fn
struct AuthResult {
    const void* ctx = nullptr;   ///< The session context to use.
    bool consumed = false;       ///< True when the line was a credential handshake and must not be processed as JSON-RPC.
};

/// Optional per-connection authenticator for the tolerant first-line
/// handshake. The project supplies one that recognizes its own credential
/// format (for example `{"auth":"admin"}`); unknown or missing handshakes
/// simply mean default policies apply.
/// @param first_line  The first line of the connection.
/// @return An `AuthResult`; `consumed = false` means default policies and the
///         line is processed as a normal message.
using authenticator_fn = AuthResult (*)(std::string_view first_line);

/// Struct-level tag marking a struct as a tool group for automatic discovery:
///
///     struct [[= mcp::tool_group{} ]] arithmetic { ... };
///     struct [[= mcp::with<admin>::tool_group{} ]] admin_area { ... };
///
/// `register_tagged_tools<^^ns>()` collects every struct tagged `tool_group`
/// in a namespace and registers its tools.
/// @tparam Auth  Group-level authorization policy inherited by every tool in the group.
template <typename Auth = no_auth>
struct tool_group { };

/// Same as `tool_group`, but for prompt groups:
/// `register_tagged_prompts<^^ns>()` collects structs tagged `prompt_group`.
/// Separate tags keep tools and prompts distinguishable within one namespace.
/// @tparam Auth  Group-level authorization policy.
template <typename Auth = no_auth>
struct prompt_group { };

/// Per-tool configuration struct, carried as the decorator
/// `[[= mcp::tool{ .description = mcp::str("Adds two integers."), .async = true } ]]`.
/// A class template over the string lengths so each string is exact-size
/// (arbitrary length, no cap) yet still one structural annotation value.
/// `async` is internal (worker thread); the rest map to the standard MCP Tool
/// fields (title, description, annotation hints).
///
/// The trailing template parameters bind the serializer (a class template
/// over the value type, defaulting to `json_serializer`) and an authorization
/// policy (a type, defaulting to `no_auth`). Select them with `mcp::with<...>`,
/// which binds them while still deducing the string lengths from the literals:
///
///     [[= mcp::with<app_serializer>::tool{ .description = mcp::str("...") } ]]
///     [[= mcp::with<no_auth, app_serializer>::tool{ .description = mcp::str("...") } ]]
///     [[= mcp::with<admin>::tool{ .description = mcp::str("...") } ]]
///
/// `app_serializer` must satisfy `template <typename T> struct S { static
/// std::string to_json(const T&); }`, the same shape as `json_serializer`. An
/// auth policy must satisfy `static bool authorized(const void* ctx)`.
/// @tparam ND    Length of the description string.
/// @tparam NT    Length of the title string.
/// @tparam NN    Length of the name override string.
/// @tparam Ser   Result serializer, a class template over the value type.
/// @tparam Auth  Authorization policy.
template <std::size_t ND = 1, std::size_t NT = 1, std::size_t NN = 1,
          template <typename> class Ser = json_serializer, typename Auth = no_auth>
struct tool {
    str<ND> description;    ///< Tool description.
    str<NT> title;          ///< Optional display title.
    str<NN> name;           ///< Name override; empty means the function name.
    /// Annotation hints. Defaults follow the MCP schema: readOnlyHint=false,
    /// destructiveHint=true (presumed destructive unless told otherwise),
    /// idempotentHint=false, openWorldHint=true (presumed open-world).
    bool is_read_only = false;    ///< annotations.readOnlyHint
    bool is_destructive = true;   ///< annotations.destructiveHint
    bool is_idempotent = false;   ///< annotations.idempotentHint
    bool is_open_world = true;    ///< annotations.openWorldHint
    bool async = false;           ///< Internal: run on a worker thread, not serialized.
};

/// Recovers the serializer type from a `tool` instantiation. The decorator
/// carries it as a template parameter; exposed as a nested alias so call
/// sites can write `tool_serializer<Config>::template type<T>::to_json(...)`.
template <typename T>
struct tool_serializer;
template <std::size_t ND, std::size_t NT, std::size_t NN, template <typename> class Ser,
          typename Auth>
struct tool_serializer<tool<ND, NT, NN, Ser, Auth>> {
    template <typename T>
    using type = Ser<T>;
};

/// Recovers the authorization policy from a `tool` instantiation.
template <typename T>
struct tool_auth;
template <std::size_t ND, std::size_t NT, std::size_t NN, template <typename> class Ser,
          typename Auth>
struct tool_auth<tool<ND, NT, NN, Ser, Auth>> {
    using type = Auth;
};

/// Recovers the authorization policy from a `tool_group`/`prompt_group` tag.
template <typename T>
struct group_auth;
template <typename Auth>
struct group_auth<tool_group<Auth>> {
    using type = Auth;
};
template <typename Auth>
struct group_auth<prompt_group<Auth>> {
    using type = Auth;
};

/// Binds a serializer and/or an authorization policy to the tool decorator
/// while preserving CTAD for the exact-size string literals.
/// `mcp::with<Auth, Ser>::tool` is `mcp::tool` with the serializer and policy
/// already set, so the string lengths are still deduced from the literals in
/// the annotation. Both parameters default:
///
///     mcp::with<admin>::tool                 // auth only, default serializer
///     mcp::with<no_auth, plain_ser>::tool    // serializer only
///     mcp::with<admin, plain_ser>::tool      // both
///
/// The plain `mcp::tool{ .description = ... }` form keeps working and uses
/// `json_serializer` and `no_auth` by default.
/// @tparam Auth  Authorization policy.
/// @tparam Ser   Result serializer.
template <typename Auth = no_auth, template <typename> class Ser = json_serializer>
struct with {
    template <std::size_t ND = 1, std::size_t NT = 1, std::size_t NN = 1>
    using tool = mcp::tool<ND, NT, NN, Ser, Auth>;
    template <typename A = Auth>
    using tool_group = mcp::tool_group<A>;
    template <typename A = Auth>
    using prompt_group = mcp::prompt_group<A>;
};

/// The outcome of a tool call: either a successful result text (MCP
/// tool-result content) or a JSON-RPC error (code + message). This is what
/// lets a tool signal a protocol-level error, for example an invalid or
/// revoked object handle, instead of only ever producing a result envelope.
struct ToolResult {
    std::string text;            ///< Successful result text, wrapped in the MCP envelope.
    bool is_error = false;       ///< True when the call produced a JSON-RPC error instead.
    int64_t error_code = 0;      ///< JSON-RPC error code, used when `is_error`.
    std::string error_message;   ///< Error message, used when `is_error`.
};

/// Tool call ABI. The caller hands the tool the raw JSON-RPC request line and
/// gets a `ToolResult` back. Parsing is entirely internal, so the tool
/// signature mentions no JSON library; a project can bring its own server (or
/// a different JSON parser) without touching this ABI.
/// @param raw_line  The raw JSON-RPC request line.
/// @param ctx       The runtime action context.
/// @return The tool result, or a JSON-RPC error.
using tool_call_fn = ToolResult (*)(std::string_view raw_line, ActionContext& ctx);

/// A registered MCP tool, mapped to the standard MCP `Tool` object. Metadata
/// strings are carried in the decorator config; `call` is generated from
/// reflection (meta.hpp). The callable is a plain function pointer so the
/// whole registry is `constexpr` (the generated lambdas are stateless and
/// convert to function pointers).
struct Action {
    std::string_view name;           ///< Tool name (identifier_of, or explicit override).
    std::string_view title;          ///< Optional display title.
    std::string_view description;    ///< Human-readable description.
    std::string_view input_schema;   ///< JSON Schema for the tool arguments.
    /// Annotation hints, defaulting to the MCP schema values (see `tool`).
    bool is_read_only = false;     ///< annotations.readOnlyHint
    bool is_destructive = true;    ///< annotations.destructiveHint
    bool is_idempotent = false;    ///< annotations.idempotentHint
    bool is_open_world = true;     ///< annotations.openWorldHint
    bool async = false;            ///< Internal: run on a worker thread, not serialized.
    tool_call_fn call = nullptr;   ///< The callable.
    auth_fn authorized = nullptr;  ///< Authorization check; null means always allowed.
};

/// Resource read callback: returns the resource content.
using resource_text_fn = std::string (*)();

/// Argument completion hook for `completion/complete`. Given the argument
/// name and the partial value the client is completing, returns a JSON array
/// of candidate strings (for example `["HEAD~1","HEAD~2"]`). Null means no
/// suggestions (the server replies with an empty list).
/// @param arg_name   The argument being completed.
/// @param arg_value  The partial value typed so far.
/// @return A JSON array of candidate strings.
using completion_fn = std::string (*)(std::string_view arg_name, std::string_view arg_value);

/// A resource: named data with a read callback (URI is not derivable from code).
struct Resource {
    std::string_view uri;             ///< Resource URI.
    std::string_view name;            ///< Display name.
    std::string_view description;     ///< Human-readable description.
    std::string_view mime_type;       ///< MIME type of the content.
    resource_text_fn text = nullptr;  ///< Reads the content.
    completion_fn complete = nullptr; ///< Optional argument completion hook.
    auth_fn authorized = nullptr;     ///< Authorization check; null means always allowed.
};

/// A resource template: a URI pattern (`file:///{path}`) with optional
/// completions for its parameters. Exposed via `resources/templates/list`.
struct ResourceTemplate {
    std::string_view uri_template;  ///< URI pattern with `{placeholders}`.
    std::string_view name;          ///< Display name.
    std::string_view description;   ///< Human-readable description.
    std::string_view mime_type;     ///< MIME type of the content.
    auth_fn authorized = nullptr;   ///< Authorization check; null means always allowed.
};

/// Prompt messages callback: returns the standard MCP `messages` array.
using prompt_messages_fn = std::string (*)(std::string_view raw_line);

/// A prompt: a reflected function whose return is the standard `messages` array.
struct Prompt {
    std::string_view name;               ///< Prompt name.
    std::string_view description;        ///< Human-readable description.
    std::string_view arguments_schema;   ///< JSON Schema for the prompt arguments.
    prompt_messages_fn messages = nullptr; ///< Builds the messages array.
    completion_fn complete = nullptr;      ///< Optional argument completion hook.
    auth_fn authorized = nullptr;          ///< Authorization check; null means always allowed.
};

// ---------------------------------------------------------------------------
// The tool ABI (raw request line in, text out) and the registry of
// capabilities. Marshalling (read_arg) lives in read-arg.hpp; the reflective
// invoker lives in meta.hpp. This header is parser-free so the core can be
// reused with any JSON backend.
// ---------------------------------------------------------------------------

/// The full set of capabilities a server serves: tools, resources, prompts,
/// and resource templates. Each is a span over a `constexpr` array in static
/// storage (built by reflection in meta.hpp, or hand-assembled). A Server is
/// decoupled from where these come from; the owning project assembles the
/// spans and passes the registry in.
struct Registry {
    std::span<const Action> actions;                      ///< Registered tools.
    std::span<const Resource> resources;                  ///< Registered resources.
    std::span<const Prompt> prompts;                      ///< Registered prompts.
    std::span<const ResourceTemplate> resource_templates; ///< Registered resource templates.

    /// Cacheability for the 2026-07-28 CacheableResult endpoints (tools/list,
    /// prompts/list, resources/list, resources/templates/list, resources/read,
    /// server/discover). `cache_ttl_ms` is a freshness hint; `cache_scope`
    /// ("public" or "private") controls shared-intermediary caching.
    std::string_view cache_scope = "public";   ///< Cache scope for the cacheable endpoints.
    std::int64_t cache_ttl_ms = 3600000;       ///< Cache freshness hint in milliseconds.
};

} // namespace mcp
