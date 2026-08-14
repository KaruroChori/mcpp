#pragma once

/// @file protocol.hpp
/// @brief Parser-independent MCP protocol core and dispatch (dual-era).
/// SPDX-License-Identifier: LGPL-3.0

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>

#include "action.hpp"
#include "json.hpp"
#include "json-view.hpp"

namespace mcp {

/// MCP protocol revisions this library implements for the modern (per-request
/// `_meta`) path: `2026-07-28` only, the sessionless, stateless revision.
/// `server/discover` advertises this list.
///
/// When `MCPP_LEGACY_2025` is defined (the default, xmake option
/// `legacy2025`), the server is dual-era: it also answers the legacy
/// `initialize` handshake and serves requests without the modern `_meta`
/// fields per `2025-11-25`, so clients still on the old protocol (opencode,
/// Claude Desktop, ...) work. Disable the option once clients migrate and the
/// `#ifdef MCPP_LEGACY_2025` blocks below become inert (then delete them).
inline constexpr std::string_view supported_protocol_versions[] = { "2026-07-28" };

#ifdef MCPP_LEGACY_2025
/// The single legacy revision the dual-era path serves.
inline constexpr std::string_view kLegacyProtocol = "2025-11-25";
#endif

/// A JSON-RPC request id: a number or a string.
struct RpcId {
    bool is_string = false;
    int64_t number = 0;
    std::string text;
};

/// Hand-off describing an async tool call. The protocol core knows async tools
/// exist (so it doesn't block or write a premature response) but does not own
/// threads: it delegates the actual execution to an AsyncExecutor supplied by
/// the server. The executor is free to spawn a worker thread (thread-pool
/// server), enqueue on a cooperative pump (embedded), or run inline.
struct AsyncCall {
    std::string_view raw_line;
    tool_call_fn call = nullptr;
    RpcId id;
    int64_t progress_token = 0;
    bool has_progress_token = false;
};

struct AsyncExecutor {
    void (*run)(void* ctx, AsyncCall call);
    /// Optional cancellation hook: set the cancel flag for a request id.
    void (*cancel)(void* ctx, const RpcId& id);
    void* ctx = nullptr;
};

/// Parser-independent MCP protocol core (2026-07-28). Reads a raw JSON-RPC
/// request line through a JsonView, validates the per-request `_meta` protocol
/// fields, dispatches on the method, and writes responses through a Sink. No
/// JSON parser, no threads, no allocation beyond a handful of `std::string`s
/// for response building (json.hpp emitter). A project brings its own parsing
/// and its own server; this is the reusable part.
///
/// Every request must carry `_meta.io.modelcontextprotocol/protocolVersion` and
/// `.../clientCapabilities`; missing fields are rejected with `-32602`, an
/// unsupported version with `UnsupportedProtocolVersionError` (`-32022`). Every
/// result carries `resultType` and `_meta.io.modelcontextprotocol/serverInfo`;
/// the cacheable list/read endpoints also carry `ttlMs` + `cacheScope`.
///
/// @see supported_protocol_versions, kLegacyProtocol
class Protocol {
public:
    Protocol(const Registry& registry, std::string_view name, std::string_view version,
             const void* auth_ctx = nullptr);

    void set_async_executor(AsyncExecutor executor) { executor_ = executor; }
    void set_auth_ctx(const void* ctx) { auth_ctx_ = ctx; }

    /// @brief Dispatch one request. Writes zero or more lines through `sink`.
    /// @param raw_line The raw JSON-RPC line, forwarded verbatim to tools.
    /// @param request The parsed request as a JsonView.
    /// @param sink The sink receiving the response lines.
    void dispatch(std::string_view raw_line, const JsonView& request, Sink& sink);

    /// @brief Builds the standard CallToolResult envelope for a finished call.
    /// @param id The request id the result answers.
    /// @param text The tool output text.
    /// @return The JSON-RPC response line.
    std::string wrap_tool_result(const RpcId& id, std::string_view text) const;

private:
    bool allowed(const Action& a) const { return !a.authorized || a.authorized(auth_ctx_); }
    bool allowed(const Resource& r) const { return !r.authorized || r.authorized(auth_ctx_); }
    bool allowed(const ResourceTemplate& t) const { return !t.authorized || t.authorized(auth_ctx_); }
    bool allowed(const Prompt& p) const { return !p.authorized || p.authorized(auth_ctx_); }

    void on_discover(const RpcId& id, Sink& sink);
    void on_tools_list(const RpcId& id, Sink& sink);
    void on_tools_call(const RpcId& id, const JsonView& params, std::string_view raw_line, Sink& sink);
    void on_resources_list(const RpcId& id, Sink& sink);
    void on_resources_templates_list(const RpcId& id, Sink& sink);
    void on_resources_read(const RpcId& id, const JsonView& params, Sink& sink);
    void on_prompts_list(const RpcId& id, Sink& sink);
    void on_prompts_get(const RpcId& id, const JsonView& params, std::string_view raw_line, Sink& sink);
    void on_completion(const RpcId& id, const JsonView& params, Sink& sink);
    void on_cancelled(const JsonView& params);
    void route(const RpcId& id, std::string_view method, const JsonView& params,
               std::string_view raw_line, Sink& sink);
#ifdef MCPP_LEGACY_2025
    void on_initialize(const RpcId& id, const JsonView& params, Sink& sink);
#endif

    void send_ok(const RpcId& id, std::string_view result, Sink& sink, bool cacheable = false);
    void send_error(const RpcId& id, int64_t code, std::string_view message, Sink& sink);
    void send_error_data(const RpcId& id, int64_t code, std::string_view message,
                         std::string_view data, Sink& sink);
    void write_line(std::string_view full_line, Sink& sink) const;
    std::string modern_result(std::string_view payload, bool cacheable) const;
    std::string supported_versions_json() const;
    std::string capabilities_json() const;
    bool has_completions() const;

    Registry registry_;
    std::string server_name_;
    std::string server_version_;
    AsyncExecutor executor_;
    const void* auth_ctx_ = nullptr;
#ifdef MCPP_LEGACY_2025
    bool legacy_ = false;   ///< set once a legacy (initialize / no-`_meta`) request arrives
#endif
};

namespace {

bool supports(std::string_view v) {
    for (auto s : supported_protocol_versions) {
        if (s == v) return true;
    }
    return false;
}

/// @brief Reads an integer-ish value as int64_t, accepting uint64 or int64.
/// @param id The JsonView to read.
/// @param out Receives the parsed value.
/// @return True if the value parsed as an integer.
bool read_id_int(const JsonView& id, int64_t& out) {
    uint64_t u;
    if (id.get_uint64(u)) {
        out = static_cast<int64_t>(u);
        return true;
    }
    int64_t v;
    if (id.get_int64(v)) {
        out = v;
        return true;
    }
    return false;
}

} // namespace

inline Protocol::Protocol(const Registry& registry, std::string_view name, std::string_view version,
                   const void* auth_ctx)
    : registry_(registry), server_name_(name), server_version_(version), auth_ctx_(auth_ctx) {}

inline void Protocol::write_line(std::string_view full_line, Sink& sink) const {
    if (sink.write) sink.write(sink.ctx, full_line);
}

/// @brief Wraps a result payload with the 2026-07-28 envelope fields:
/// `resultType`, the optional CacheableResult `ttlMs`/`cacheScope`, and the
/// per-result `_meta.serverInfo`.
/// @param payload A JSON object whose fields are spliced into the result.
/// @param cacheable Whether the result carries `ttlMs`/`cacheScope`.
/// @return The full 2026-07-28 result object.
inline std::string Protocol::modern_result(std::string_view payload, bool cacheable) const {
    std::string out = R"({"resultType":"complete")";
    out += ",";
    out += payload.substr(1, payload.size() - 2);   // drop the outer braces
    if (cacheable) {
        out += ",\"ttlMs\":" + std::to_string(registry_.cache_ttl_ms);
        out += ",\"cacheScope\":" + json_string(registry_.cache_scope);
    }
    out += ",\"_meta\":{\"io.modelcontextprotocol/serverInfo\":{\"name\":" +
           json_string(server_name_) + ",\"version\":" + json_string(server_version_) + "}}";
    out += "}";
    return out;
}

inline std::string Protocol::supported_versions_json() const {
    std::string out = "[";
    bool first = true;
    for (auto v : supported_protocol_versions) {
        if (!first) out += ",";
        first = false;
        out += json_string(v);
    }
    return out + "]";
}

inline void Protocol::send_ok(const RpcId& id, std::string_view result, Sink& sink, bool cacheable) {
#ifdef MCPP_LEGACY_2025
    // Legacy results are the plain 2025 shape (no resultType / _meta / ttlMs).
    if (legacy_) {
        write_line(id.is_string ? ok_response(id.text, result) : ok_response(id.number, result), sink);
        return;
    }
#endif
    const std::string full = modern_result(result, cacheable);
    write_line(id.is_string ? ok_response(id.text, full) : ok_response(id.number, full), sink);
}

inline void Protocol::send_error(const RpcId& id, int64_t code, std::string_view message, Sink& sink) {
    const std::string full =
        id.is_string ? error_response(id.text, code, message) : error_response(id.number, code, message);
    write_line(full, sink);
}

inline void Protocol::send_error_data(const RpcId& id, int64_t code, std::string_view message,
                                      std::string_view data, Sink& sink) {
    const std::string full = id.is_string ? error_response(id.text, code, message, data)
                                          : error_response(id.number, code, message, data);
    write_line(full, sink);
}

inline std::string Protocol::wrap_tool_result(const RpcId& id, std::string_view text) const {
#ifdef MCPP_LEGACY_2025
    // Legacy CallToolResult: the plain `{"content":[...]}` 2025 shape.
    if (legacy_) {
        const std::string envelope = json_object({{"content", "[" + json_object({
            {"type", json_string("text")},
            {"text", json_string(text)},
        }) + "]"}});
        return id.is_string ? ok_response(id.text, envelope) : ok_response(id.number, envelope);
    }
#endif
    // CallToolResult: content + resultType + _meta.serverInfo.
    const std::string envelope = json_object({
        {"content", "[" + json_object({
            {"type", json_string("text")},
            {"text", json_string(text)},
        }) + "]"},
        {"resultType", json_string("complete")},
        {"_meta", json_object({
            {"io.modelcontextprotocol/serverInfo", json_object({
                {"name", json_string(server_name_)},
                {"version", json_string(server_version_)},
            })},
        })},
    });
    return id.is_string ? ok_response(id.text, envelope) : ok_response(id.number, envelope);
}

inline void Protocol::dispatch(std::string_view raw_line, const JsonView& request, Sink& sink) {
    const JsonView params = request["params"];

    std::string_view method;
    if (!request["method"].get_string(method)) {
        // A response from the client is unexpected (we never send requests).
        write_line(error_response_null(RPC_INVALID_REQUEST, "Invalid request"), sink);
        return;
    }

    const JsonView id = request["id"];
    const bool is_notification = !id.present();

    RpcId rid;
    std::string_view text_id;
    if (!is_notification) {
        if (id.get_string(text_id)) {
            rid.is_string = true;
            rid.text = std::string(text_id);
        } else if (!read_id_int(id, rid.number)) {
            write_line(error_response_null(RPC_INVALID_REQUEST, "Invalid request"), sink);
            return;
        }
    }

#ifdef MCPP_LEGACY_2025
    // Dual-era: a request without the modern `_meta` is a legacy client. An
    // `initialize` selects the legacy handshake; anything else (opencode's
    // post-initialize traffic) is served per the legacy revision. The era is a
    // property of this process/session, so once set it stays.
    if (!is_notification && !params["_meta"]["io.modelcontextprotocol/protocolVersion"].present()) {
        legacy_ = true;
        if (method == "initialize") {
            on_initialize(rid, params, sink);
        } else {
            route(rid, method, params, raw_line, sink);
        }
        return;
    }
#endif

    // 2026-07-28: every request carries its protocol version + client
    // capabilities in `_meta`. Notifications cannot be answered, so strict
    // validation applies to requests only.
    if (!is_notification) {
        const JsonView meta = params["_meta"];
        std::string_view version;
        if (!meta["io.modelcontextprotocol/protocolVersion"].get_string(version)) {
            send_error(rid, RPC_INVALID_PARAMS,
                       "Missing _meta.io.modelcontextprotocol/protocolVersion", sink);
            return;
        }
        if (!meta["io.modelcontextprotocol/clientCapabilities"].present()) {
            send_error(rid, RPC_INVALID_PARAMS,
                       "Missing _meta.io.modelcontextprotocol/clientCapabilities", sink);
            return;
        }
        // `server/discover` is the version-selection probe: it answers even for
        // a version this server does not support (that is its point).
        if (method != "server/discover" && !supports(version)) {
            const std::string data = json_object({
                {"supported", supported_versions_json()},
                {"requested", json_string(version)},
            });
            send_error_data(rid, RPC_UNSUPPORTED_PROTOCOL_VERSION,
                            "Unsupported protocol version", data, sink);
            return;
        }
    }

    if (is_notification) {
        if (method == "notifications/cancelled") {
            on_cancelled(params);
        }
        return;
    }

    route(rid, method, params, raw_line, sink);
}

#ifdef MCPP_LEGACY_2025
/// @brief The legacy 2025-11-25 initialize handshake.
/// We serve this one legacy revision only: a client requesting an older
/// revision still gets 2025-11-25 back (the legacy lifecycle requires
/// responding with a version we support).
inline void Protocol::on_initialize(const RpcId& id, const JsonView& params, Sink& sink) {
    (void)params;
    send_ok(id, json_object({
        {"protocolVersion", json_string(kLegacyProtocol)},
        {"capabilities", "{" + capabilities_json() + "}"},
        {"serverInfo", json_object({
            {"name", json_string(server_name_)},
            {"version", json_string(server_version_)},
        })},
    }), sink);
}
#endif

inline void Protocol::route(const RpcId& id, std::string_view method, const JsonView& params,
                            std::string_view raw_line, Sink& sink) {
    if (method == "server/discover") {
        on_discover(id, sink);
    } else if (method == "tools/list") {
        on_tools_list(id, sink);
    } else if (method == "tools/call") {
        on_tools_call(id, params, raw_line, sink);
    } else if (method == "resources/list") {
        on_resources_list(id, sink);
    } else if (method == "resources/templates/list") {
        on_resources_templates_list(id, sink);
    } else if (method == "resources/read") {
        on_resources_read(id, params, sink);
    } else if (method == "prompts/list") {
        on_prompts_list(id, sink);
    } else if (method == "prompts/get") {
        on_prompts_get(id, params, raw_line, sink);
    } else if (method == "completion/complete") {
        on_completion(id, params, sink);
    } else {
        send_error(id, RPC_METHOD_NOT_FOUND, "Method not found: " + std::string(method), sink);
    }
}

// ---------------------------------------------------------------------------
// discovery
// ---------------------------------------------------------------------------

inline std::string Protocol::capabilities_json() const {
    // Disclose only the capabilities this server actually serves. The
    // capabilities describe the registry, not the per-session auth filter.
    std::string capabilities;
    auto add = [&](std::string_view cap) {
        if (!capabilities.empty()) capabilities += ",";
        capabilities += cap;
    };
    if (!registry_.actions.empty()) add(R"("tools":{})");
    if (!registry_.resources.empty() || !registry_.resource_templates.empty()) add(R"("resources":{})");
    if (!registry_.prompts.empty()) add(R"("prompts":{})");
    if (has_completions()) add(R"("completions":{})");
    return capabilities;
}

inline bool Protocol::has_completions() const {
    for (const auto& p : registry_.prompts) if (p.complete) return true;
    for (const auto& r : registry_.resources) if (r.complete) return true;
    return false;
}

inline void Protocol::on_discover(const RpcId& id, Sink& sink) {
    // DiscoverResult (cacheable): supportedVersions + capabilities.
    send_ok(id, json_object({
        {"supportedVersions", supported_versions_json()},
        {"capabilities", "{" + capabilities_json() + "}"},
    }), sink, /*cacheable=*/true);
}

// ---------------------------------------------------------------------------
// tools
// ---------------------------------------------------------------------------

inline void Protocol::on_tools_list(const RpcId& id, Sink& sink) {
    std::string tools;
    for (const auto& a : registry_.actions) {
        if (!allowed(a)) continue;   // hide tools the session isn't authorized for
        if (!tools.empty()) tools += ",";
        // Standard MCP Tool: optional title + annotations hints. Each optional
        // field is emitted only when present (title and annotations must not
        // clobber each other).
        std::string title_field;
        if (!a.title.empty()) {
            title_field = ",\"title\":" + json_string(a.title);
        }
        // Annotations are emitted only when the hints deviate from the MCP
        // schema defaults (readOnly=false, destructive=true, idempotent=false,
        // openWorld=true); a tool that states nothing is presumed to follow
        // the defaults. When emitted, every key carries the effective value.
        std::string annotations_field;
        if (a.is_read_only || !a.is_destructive || a.is_idempotent || !a.is_open_world) {
            annotations_field = ",\"annotations\":" + json_object({
                {"readOnlyHint", a.is_read_only ? "true" : "false"},
                {"destructiveHint", a.is_destructive ? "true" : "false"},
                {"idempotentHint", a.is_idempotent ? "true" : "false"},
                {"openWorldHint", a.is_open_world ? "true" : "false"},
            });
        }
        const std::string tool =
            "{\"name\":" + json_string(a.name) +
            ",\"description\":" + json_string(a.description) +
            ",\"inputSchema\":" + std::string(a.input_schema) +
            title_field + annotations_field + "}";
        tools += tool;
    }
    send_ok(id, "{\"tools\":[" + tools + "]}", sink, /*cacheable=*/true);
}

namespace {

/// @brief Extracts the `_meta.progressToken` from a tools/call request, if present.
/// @param params The request params.
/// @param token Receives the progress token.
/// @param has True if the token was present.
void read_progress_token(const JsonView& params, int64_t& token, bool& has) {
    has = false;
    token = 0;
    const JsonView meta = params["_meta"];
    const JsonView tok = meta["progressToken"];
    if (!tok.present()) return;
    uint64_t u;
    if (tok.get_uint64(u)) {
        token = static_cast<int64_t>(u);
        has = true;
    } else {
        int64_t v;
        if (tok.get_int64(v)) {
            token = v;
            has = true;
        }
    }
}

} // namespace

inline void Protocol::on_tools_call(const RpcId& id, const JsonView& params, std::string_view raw_line, Sink& sink) {
    std::string_view name;
    if (!params["name"].get_string(name)) {
        send_error(id, RPC_INVALID_PARAMS, "Missing tool name", sink);
        return;
    }
    const auto& list = registry_.actions;
    const auto it = std::find_if(list.begin(), list.end(), [&](const Action& a) { return a.name == name; });
    if (it == list.end() || !allowed(*it)) {
        // Hidden tools are treated as unknown so a caller can't probe for them.
        send_error(id, RPC_INVALID_PARAMS, "Unknown tool: " + std::string(name), sink);
        return;
    }

    int64_t progress_token = 0;
    bool has_progress_token = false;
    read_progress_token(params, progress_token, has_progress_token);

    if (!it->async || !executor_.run) {
        // Fast path: run inline. The tool parses its own copy of the line.
        ActionContext ctx;
        ctx.has_progress_token = has_progress_token;
        ctx.progress_token = progress_token;
        ctx.send = [&sink](std::string_view line) { if (sink.write) sink.write(sink.ctx, line); };
        const ToolResult result = it->call(raw_line, ctx);
        if (result.is_error) {
            send_error(id, result.error_code, result.error_message, sink);
        } else {
            write_line(wrap_tool_result(id, result.text), sink);
        }
        return;
    }

    // Async path: hand off to the executor. The tool re-parses the line itself,
    // so the worker needs no JSON document of its own.
    AsyncCall call;
    call.raw_line = raw_line;
    call.call = it->call;
    call.id = id;
    call.progress_token = progress_token;
    call.has_progress_token = has_progress_token;
    executor_.run(executor_.ctx, call);
}

// ---------------------------------------------------------------------------
// resources
// ---------------------------------------------------------------------------

inline void Protocol::on_resources_list(const RpcId& id, Sink& sink) {
    std::string list;
    for (const auto& r : registry_.resources) {
        if (!allowed(r)) continue;
        if (!list.empty()) list += ",";
        list += json_object({
            {"uri", json_string(r.uri)},
            {"name", json_string(r.name)},
            {"description", json_string(r.description)},
            {"mimeType", json_string(r.mime_type)},
        });
    }
    send_ok(id, "{\"resources\":[" + list + "]}", sink, /*cacheable=*/true);
}

inline void Protocol::on_resources_templates_list(const RpcId& id, Sink& sink) {
    std::string list;
    for (const auto& t : registry_.resource_templates) {
        if (!allowed(t)) continue;
        if (!list.empty()) list += ",";
        list += json_object({
            {"uriTemplate", json_string(t.uri_template)},
            {"name", json_string(t.name)},
            {"description", json_string(t.description)},
            {"mimeType", json_string(t.mime_type)},
        });
    }
    send_ok(id, "{\"resourceTemplates\":[" + list + "]}", sink, /*cacheable=*/true);
}

inline void Protocol::on_resources_read(const RpcId& id, const JsonView& params, Sink& sink) {
    std::string_view uri;
    if (!params["uri"].get_string(uri)) {
        send_error(id, RPC_INVALID_PARAMS, "Missing resource uri", sink);
        return;
    }
    const auto& list = registry_.resources;
    const auto it = std::find_if(list.begin(), list.end(), [&](const Resource& r) { return r.uri == uri; });
    if (it == list.end() || !allowed(*it)) {
        send_error(id, RPC_INVALID_PARAMS, "Resource " + std::string(uri) + " not found", sink);
        return;
    }
    send_ok(id, "{\"contents\":[" + json_object({
        {"uri", json_string(it->uri)},
        {"mimeType", json_string(it->mime_type)},
        {"text", json_string(it->text())},
    }) + "]}", sink, /*cacheable=*/true);
}

// ---------------------------------------------------------------------------
// prompts
// ---------------------------------------------------------------------------

inline void Protocol::on_prompts_list(const RpcId& id, Sink& sink) {
    std::string list;
    for (const auto& p : registry_.prompts) {
        if (!allowed(p)) continue;
        if (!list.empty()) list += ",";
        list += json_object({
            {"name", json_string(p.name)},
            {"description", json_string(p.description)},
            {"arguments", std::string(p.arguments_schema)},
        });
    }
    send_ok(id, "{\"prompts\":[" + list + "]}", sink, /*cacheable=*/true);
}

inline void Protocol::on_prompts_get(const RpcId& id, const JsonView& params, std::string_view raw_line, Sink& sink) {
    std::string_view name;
    if (!params["name"].get_string(name)) {
        send_error(id, RPC_INVALID_PARAMS, "Missing prompt name", sink);
        return;
    }
    const auto& list = registry_.prompts;
    const auto it = std::find_if(list.begin(), list.end(), [&](const Prompt& p) { return p.name == name; });
    if (it == list.end() || !allowed(*it)) {
        send_error(id, RPC_INVALID_PARAMS, "Unknown prompt: " + std::string(name), sink);
        return;
    }
    send_ok(id, "{\"messages\":" + it->messages(raw_line) + "}", sink);
}

inline void Protocol::on_completion(const RpcId& id, const JsonView& params, Sink& sink) {
    // completion/complete: suggest values for an argument of a prompt or
    // resource. The request carries the referenced object plus the argument
    // name and partial value the client is completing.
    std::string_view ref_type;
    const JsonView ref = params["ref"];
    if (!ref["type"].get_string(ref_type)) {
        send_error(id, RPC_INVALID_PARAMS, "completion/complete requires ref.type", sink);
        return;
    }
    std::string_view arg_name;
    std::string_view arg_value;
    if (!params["argument"]["name"].get_string(arg_name)) {
        send_error(id, RPC_INVALID_PARAMS, "completion/complete requires argument.name", sink);
        return;
    }
    [[maybe_unused]] auto unused = params["argument"]["value"].get_string(arg_value);

    completion_fn complete = nullptr;
    if (ref_type == "ref/prompt") {
        std::string_view name;
        if (!ref["name"].get_string(name)) {
            send_error(id, RPC_INVALID_PARAMS, "completion/complete ref missing name", sink);
            return;
        }
        const auto& list = registry_.prompts;
        const auto it = std::find_if(list.begin(), list.end(), [&](const Prompt& p) { return p.name == name; });
        if (it != list.end() && allowed(*it)) complete = it->complete;
    } else if (ref_type == "ref/resource") {
        std::string_view uri;
        if (!ref["uri"].get_string(uri)) {
            send_error(id, RPC_INVALID_PARAMS, "completion/complete ref missing uri", sink);
            return;
        }
        const auto& list = registry_.resources;
        const auto it = std::find_if(list.begin(), list.end(), [&](const Resource& r) { return r.uri == uri; });
        if (it != list.end() && allowed(*it)) complete = it->complete;
    }

    const std::string values = complete ? complete(arg_name, arg_value) : "[]";
    send_ok(id, "{\"completion\":{\"values\":" + values + ",\"hasMore\":false}}", sink);
}

// ---------------------------------------------------------------------------
// cancellation
// ---------------------------------------------------------------------------

inline void Protocol::on_cancelled(const JsonView& params) {
    // Cancellation is server policy: the executor owns the cancel flags, so the
    // core only extracts the requestId (a number or a string, per the standard)
    // and routes it through the executor's cancel hook. A server that wants
    // cancellation support installs that hook; the plain Protocol ignores it.
    const JsonView req = params["requestId"];
    RpcId id;
    std::string_view text_id;
    if (req.get_string(text_id)) {
        id.is_string = true;
        id.text = std::string(text_id);
    } else if (!read_id_int(req, id.number)) {
        return;   // no usable requestId
    }
    if (executor_.cancel) executor_.cancel(executor_.ctx, id);
}

} // namespace mcp
