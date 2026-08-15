#pragma once

/// @file meta.hpp
/// @brief Reflection layer: tool/prompt registration and schema generation.
/// SPDX-License-Identifier: LGPL-3.0

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

#include <meta>

#include "action.hpp"
#include "json.hpp"
#include "json-view.hpp"
#include "object.hpp"
#include "read-arg.hpp"
#include "serializer.hpp"

namespace meta = std::meta;

namespace mcp {

/// ---------------------------------------------------------------------------
/// Reflection over a tool/prompt group `Group`, a struct whose member functions
/// are tools. For each function member `Fn`:
///   - name        <- the `tool` decorator's `name`, else identifier_of(Fn)
///   - schema      <- parameters_of(Fn) (skipping a trailing ActionContext&)
///   - call        <- generated: read JSON args by reflected name, then invoke
///                    the function (splicing the object instance when non-static)
///   - description/title/flags <- the `tool` decorator (string_view config)
/// ---------------------------------------------------------------------------

consteval bool is_ctx_param(meta::info p) {
    const auto t = meta::type_of(p);
    return meta::is_same_type(t, ^^mcp::ActionContext)
        || meta::is_same_type(t, ^^mcp::ActionContext&)
        || meta::is_same_type(t, ^^mcp::ActionContext&&);
}

consteval std::size_t param_count(meta::info fn) {
    return meta::parameters_of(fn).size();
}

/// Number of reflected JSON parameters (excluding a trailing ActionContext&).
/// @return The number of parameters exposed in the JSON schema.
consteval std::size_t json_arity(meta::info fn) {
    const auto ps = meta::parameters_of(fn);
    if (ps.empty()) return 0;
    return ps.size() - (is_ctx_param(ps[ps.size() - 1]) ? 1 : 0);
}

/// True for non-static tools, which take one extra injected JSON parameter, the `handle`.
/// @return `true` when `Fn` is a non-static member function.
consteval bool is_object_tool(meta::info fn) {
    return !meta::is_static_member(fn);
}

template <meta::info Fn>
consteval std::string_view fn_name() {
    return meta::identifier_of(Fn);
}

/// JSON type keyword for a reflected type.
/// @param t The reflected type.
/// @return "integer", "number", "string", or "boolean".
consteval const char* json_type_name(meta::info t) {
    t = meta::dealias(t);
    if (meta::is_integral_type(t)) return "integer";
    if (meta::is_floating_point_type(t)) return "number";
    if (meta::is_same_type(t, ^^std::string) || meta::is_same_type(t, ^^std::string_view)) return "string";
    if (meta::is_same_type(t, ^^bool)) return "boolean";
    return "string";
}

/// JSON Schema for a single parameter type.
/// @tparam T The parameter type to describe.
/// @return JSON Schema text: primitives as before; enums as a string enum list
/// (they marshal by enumerator name); `std::optional<T>` unwraps to T's schema;
/// `std::vector<T>` becomes an array; literal structs become an object with
/// per-member schemas (optional members are not marked required).
template <typename T>
consteval std::string param_schema();

template <typename T, std::size_t... Is>
consteval std::string struct_schema_impl(std::index_sequence<Is...>) {
    constexpr auto infos = arg_detail::struct_member_infos<T>();
    constexpr auto names = arg_detail::struct_member_names<T>();
    std::string props;
    std::string req;
    ((void)([&] {
        using MT = std::remove_cvref_t<typename [: meta::type_of(infos[Is]) :]>;
        if (!props.empty()) props += ",";
        props += "\"" + std::string(names[Is]) + "\":" + param_schema<MT>();
        if constexpr (!is_optional<MT>::value) {
            if (!req.empty()) req += ",";
            req += "\"" + std::string(names[Is]) + "\"";
        }
    }()), ...);
    return "{\"type\":\"object\",\"properties\":{" + props + "},\"required\":[" + req + "]}";
}

template <typename T>
consteval std::string param_schema() {
    if constexpr (is_optional<T>::value) {
        return param_schema<typename T::value_type>();
    } else if constexpr (is_vector<T>::value) {
        return "{\"type\":\"array\",\"items\":" + param_schema<typename T::value_type>() + "}";
    } else if constexpr (std::is_enum_v<T>) {
        // Enumerator names are identifiers: safe to quote without escaping.
        constexpr auto table = arg_detail::enum_table<T>();
        std::string enums;
        for (const auto& [ename, value] : table) {
            if (!enums.empty()) enums += ",";
            enums += "\"" + std::string(ename) + "\"";
        }
        return "{\"type\":\"string\",\"enum\":[" + enums + "]}";
    } else if constexpr (std::is_class_v<T> && !std::is_same_v<std::remove_cv_t<T>, std::string>) {
        return struct_schema_impl<T>(std::make_index_sequence<arg_detail::struct_member_count<T>()>());
    } else {
        return "{\"type\":\"" + std::string(json_type_name(^^T)) + "\"}";
    }
}

/// Parameter introspection at compile time. Everything that touches a
/// `meta::info` must happen at consteval (a `meta::info` is a consteval-only
/// type), so these helpers feed plain values/types to the runtime lambda that
/// assembles the schema.
template <meta::info Fn, std::size_t I>
consteval meta::info param_at() {
    const auto ps = meta::parameters_of(Fn);
    return ps[I];
}
template <meta::info Fn, std::size_t I>
consteval std::string_view param_name_at() {
    return meta::identifier_of(param_at<Fn, I>());
}
template <meta::info Fn, std::size_t I>
consteval bool param_is_ctx_at() {
    return is_ctx_param(param_at<Fn, I>());
}
template <meta::info Fn, std::size_t I>
using param_type_at = std::remove_cvref_t<typename [: meta::type_of(param_at<Fn, I>()) :]>;

template <meta::info Fn, std::size_t... Is>
consteval std::string schema_of_impl(std::index_sequence<Is...>) {
    // Field names are identifiers: safe to quote without escaping.
    auto q = [](std::string_view s) { return "\"" + std::string(s) + "\""; };
    std::string out = R"({"type":"object","properties":{)";
    std::string req;
    bool first = true;
    // Non-static tools take the injected object handle first.
    if (is_object_tool(Fn)) {
        out += "\"handle\":{\"type\":\"integer\",\"description\":\"Opaque handle of the target object instance\"}";
        req += "\"handle\"";
        first = false;
    }
    ((void)([&] {
        // The ctx check must be `if constexpr` so the injected ActionContext&
        // never reaches param_schema (its reflected members would not be
        // schema-able).
        if constexpr (param_is_ctx_at<Fn, Is>()) {
        } else {
            using PT = param_type_at<Fn, Is>;
            if (!first) out += ",";
            first = false;
            out += q(param_name_at<Fn, Is>()) + ":" + param_schema<PT>();
            // std::optional parameters are not required.
            if constexpr (!is_optional<PT>::value) {
                if (!req.empty()) req += ",";
                req += q(param_name_at<Fn, Is>());
            }
        }
    }()), ...);
    out += "},\"required\":[" + req + "]}";
    return out;
}

/// JSON Schema for the reflected parameters of `Fn`.
/// @tparam Fn The function to describe.
/// @return An object schema of the tool's JSON arguments.
template <meta::info Fn>
consteval std::string schema_of() {
    return schema_of_impl<Fn>(std::make_index_sequence<param_count(Fn)>());
}

/// Prompt arguments list, one entry per reflected parameter.
/// @tparam Fn The function to describe.
/// @return A JSON array of `{"name":"...","required":true}` entries.
template <meta::info Fn>
consteval std::string prompt_arguments_of() {
    std::string out = "[";
    bool first = true;
    if (is_object_tool(Fn)) {
        out += "{\"name\":\"handle\",\"required\":true}";
        first = false;
    }
    for (meta::info p : meta::parameters_of(Fn)) {
        if (is_ctx_param(p)) continue;
        if (!first) out += ",";
        first = false;
        out += "{\"name\":\"" + std::string(meta::identifier_of(p)) + "\",\"required\":true}";
    }
    out += "]";
    return out;
}

/// Materialize a consteval-built string into static storage. `std::string` is
/// non-structural, so `std::define_static_string` promotes it to a stable,
/// NUL-terminated static char array.
template <meta::info Fn>
consteval std::string_view schema_sv() {
    return std::string_view(std::define_static_string(schema_of<Fn>()));
}

template <meta::info Fn>
consteval std::string_view prompt_args_sv() {
    return std::string_view(std::define_static_string(prompt_arguments_of<Fn>()));
}

/// -- parameter name/type tables ----------------------------------------------

template <meta::info Fn, std::size_t... Is>
consteval std::array<std::string_view, sizeof...(Is)> names_from(std::index_sequence<Is...>) {
    return { meta::identifier_of(meta::parameters_of(Fn)[Is])... };
}

template <meta::info Fn, typename Seq>
struct types_from;

template <meta::info Fn, std::size_t... Is>
struct types_from<Fn, std::index_sequence<Is...>> {
    using type = std::tuple<typename [: meta::type_of(meta::parameters_of(Fn)[Is]) :]...>;
};

/// -- generated invocation (Approach B) ---------------------------------------
///
/// `fn_ptr` splices the function (or member function). Non-static tools operate
/// on a caller-selected object: the client sends an opaque `handle` token as an
/// extra first argument, the server resolves it through `object_registry<Dev>`,
/// and the member function is invoked on that live instance. A static tool has
/// no object and no handle. `call_value` returns the function's natural result;
/// `call` returns a `ToolResult` (either serialized JSON text or a JSON-RPC
/// error for an invalid/revoked handle).
///
/// The tool ABI is a raw JSON-RPC request line (`std::string_view`); parsing is
/// internal and delegated to a backend `Parser` (see json-view.hpp). The parser
/// owns the document; `invoker` extracts `params.arguments` as a JsonView and
/// reads the reflected parameters through `read_arg`.

template <meta::info Group, meta::info Fn, template <typename> class Ser = json_serializer,
          typename Parser = no_parser>
struct invoker {
    using Dev = typename [: Group :];
    using result_type = typename [: meta::return_type_of(Fn) :];
    static constexpr bool is_static = meta::is_static_member(Fn);
    static constexpr std::size_t total = param_count(Fn);
    static constexpr std::size_t arity = json_arity(Fn);
    static constexpr bool has_ctx = total != arity;
    static constexpr std::array<std::string_view, arity> names =
        names_from<Fn>(std::make_index_sequence<arity>());
    using arg_types = typename types_from<Fn, std::make_index_sequence<arity>>::type;
    static constexpr auto fn_ptr = &[:Fn:];

    // Static tool: apply the reflected args (plus ctx) directly.
    template <std::size_t... Is>
    static result_type apply_static(const JsonView& args, ActionContext& ctx,
                                    std::index_sequence<Is...>) {
        auto tup = std::make_tuple(read_arg<std::tuple_element_t<Is, arg_types>>(args, names[Is])...);
        if constexpr (has_ctx) {
            return std::apply(fn_ptr, std::tuple_cat(tup, std::tie(ctx)));
        } else {
            return std::apply(fn_ptr, tup);
        }
    }

    // Non-static tool: resolve the handle, then apply on the live object.
    template <std::size_t... Is>
    static result_type apply_object(Dev& obj, const JsonView& args, ActionContext& ctx,
                                    std::index_sequence<Is...>) {
        auto tup = std::make_tuple(read_arg<std::tuple_element_t<Is, arg_types>>(args, names[Is])...);
        if constexpr (has_ctx) {
            return std::apply(fn_ptr, std::tuple_cat(std::tie(obj), tup, std::tie(ctx)));
        } else {
            return std::apply(fn_ptr, std::tuple_cat(std::tie(obj), tup));
        }
    }

    static result_type call_value(std::string_view raw_line, ActionContext& ctx) {
        typename Parser::document doc;
        JsonView root;
        if (!Parser::parse(raw_line, doc, root)) return {};
        const JsonView args = root["params"]["arguments"];
        if constexpr (is_static) {
            return apply_static(args, ctx, std::make_index_sequence<arity>());
        } else {
            Dev* obj = object_registry<Dev>::resolve(read_arg<object_token>(args, "handle"));
            if (!obj) return {};
            return apply_object(*obj, args, ctx, std::make_index_sequence<arity>());
        }
    }

    static ToolResult call(std::string_view raw_line, ActionContext& ctx) {
        typename Parser::document doc;
        JsonView root;
        if (!Parser::parse(raw_line, doc, root)) {
            return ToolResult{ {}, true, RPC_INVALID_PARAMS, "Parse error" };
        }
        const JsonView args = root["params"]["arguments"];
        if constexpr (is_static) {
            return ToolResult{ Ser<result_type>::to_json(
                apply_static(args, ctx, std::make_index_sequence<arity>())), false, 0, {} };
        } else {
            Dev* obj = object_registry<Dev>::resolve(read_arg<object_token>(args, "handle"));
            if (!obj) {
                return ToolResult{ {}, true, RPC_INVALID_PARAMS,
                                   "Invalid or revoked object handle" };
            }
            return ToolResult{ Ser<result_type>::to_json(
                apply_object(*obj, args, ctx, std::make_index_sequence<arity>())), false, 0, {} };
        }
    }
};

/// -- config struct lookup ----------------------------------------------------
///
/// The tool's config is the decorator `[[= mcp::tool{ .description = mcp::str("...") } ]]` on the
/// function, a structural class template `tool<N1,N2,N3,Ser>` (exact-size
/// strings). We match any instantiation by its template and extract with the
/// spliced exact type.

consteval meta::info find_meta(meta::info fn) {
    for (meta::info a : meta::annotations_of(fn)) {
        // Only `tool` annotations matter here. `complete_for` is a plain struct
        // and template_of would throw on it, so skip it by type identity.
        if (meta::is_same_type(meta::remove_const(meta::type_of(a)), ^^mcp::complete_for)) continue;
        if (meta::template_of(meta::remove_const(meta::type_of(a))) == ^^mcp::tool) {
            return a;
        }
    }
    return {};
}

template <meta::info Fn>
consteval auto tool_of() {
    return meta::extract<typename [: meta::type_of(find_meta(Fn)) :]>(find_meta(Fn));
}

/// The group-level tag (`tool_group<Auth>` / `prompt_group<Auth>`) on the group
/// struct itself. Auth is recovered from its template argument.
consteval meta::info find_group_meta(meta::info group) {
    for (meta::info a : meta::annotations_of(group)) {
        const meta::info t = meta::remove_const(meta::type_of(a));
        if (meta::template_of(t) == ^^mcp::tool_group || meta::template_of(t) == ^^mcp::prompt_group) {
            return a;
        }
    }
    return {};
}

/// Recover the auth policy from a group config type (`tool_group<A>` / `prompt_group<A>`).
/// @tparam Config The group tag instantiation.
template <typename Config>
using group_auth_of = typename group_auth<Config>::type;

template <meta::info Group, meta::info Fn>
constexpr auth_fn make_authorized() {
    // Combine the group's auth (if any) with the tool's own auth: both must pass.
    using Config = std::remove_cv_t<typename [: meta::type_of(find_meta(Fn)) :]>;
    using ToolAuth = typename tool_auth<Config>::type;
    if constexpr (find_group_meta(Group) != meta::info{}) {
        using GroupConfig = std::remove_cv_t<typename [: meta::type_of(find_group_meta(Group)) :]>;
        using GroupAuth = group_auth_of<GroupConfig>;
        return [](const void* ctx) { return GroupAuth::authorized(ctx) && ToolAuth::authorized(ctx); };
    } else {
        return [](const void* ctx) { return ToolAuth::authorized(ctx); };
    }
}

consteval meta::info find_complete_meta(meta::info fn) {
    for (meta::info a : meta::annotations_of(fn)) {
        if (meta::is_same_type(meta::remove_const(meta::type_of(a)), ^^mcp::complete_for)) return a;
    }
    return {};
}

/// Find the completion hook for a prompt: a sibling carrying `complete_for` with
/// a `prompt` reference to this prompt, else the `<prompt>_complete` convention.
/// `M` is a template parameter so the `complete_for` annotation value can be
/// extracted (a loop variable's info is not a constant expression).

consteval std::size_t func_member_count(meta::info group);
template <meta::info Group>
consteval std::array<meta::info, func_member_count(Group)> func_members();

/// All function members (registered tools/prompts AND their completion hooks),
/// needed to discover a hook that is not itself registered.
consteval std::size_t all_func_member_count(meta::info group) {
    std::size_t n = 0;
    for (meta::info m : meta::members_of(group, meta::access_context::current())) {
        if (meta::is_function(m) && meta::has_identifier(m)) ++n;
    }
    return n;
}

template <meta::info Group>
consteval std::array<meta::info, all_func_member_count(Group)> all_func_members() {
    auto ms = meta::members_of(Group, meta::access_context::current());
    std::array<meta::info, all_func_member_count(Group)> out{};
    std::size_t j = 0;
    for (meta::info m : ms) {
        if (meta::is_function(m) && meta::has_identifier(m)) out[j++] = m;
    }
    return out;
}

template <meta::info M, meta::info Prompt>
consteval meta::info try_complete() {
    if constexpr (find_complete_meta(M) == meta::info{}) {
        return meta::info{};
    } else {
        constexpr complete_for cfg = meta::extract<complete_for>(find_complete_meta(M));
        return cfg.prompt == Prompt ? M : meta::info{};
    }
}

template <meta::info Group, meta::info Prompt, std::size_t... Is>
consteval meta::info find_completion_impl(std::index_sequence<Is...>) {
    // Scan ALL function members: a completion hook is not registered, so the
    // registered-only func_members<Group>() would never see it.
    constexpr auto fns = all_func_members<Group>();
    constexpr std::string_view convention =
        std::string_view(meta::identifier_of(Prompt));
    meta::info out{};
    ((void)([&] {
        if (out != meta::info{}) return;
        if (meta::identifier_of(fns[Is]) == std::string(convention) + "_complete") {
            out = fns[Is];
        }
    }()), ...);
    if (out != meta::info{}) return out;
    const meta::info results[] = { try_complete<fns[Is], Prompt>()... };
    for (meta::info r : results) {
        if (r != meta::info{}) return r;
    }
    return meta::info{};
}

template <meta::info Group, meta::info Prompt>
consteval meta::info find_completion() {
    return find_completion_impl<Group, Prompt>(
        std::make_index_sequence<all_func_member_count(Group)>());
}

/// -- factory: one Action per reflected tool -----------------------------------

template <meta::info Group, meta::info Fn, typename Parser>
constexpr Action make_tool() {
    // Materialize the config into static storage once; the string_views point
    // into it (stable for the life of the process). The callable lambda is
    // stateless, so it converts to a function pointer in a constant expression.
    // The config type carries the tool's serializer (default json_serializer),
    // recovered through tool_serializer so invoker serializes with it.
    using Config = std::remove_cv_t<typename [: meta::type_of(find_meta(Fn)) :]>;
    static constexpr auto meta = tool_of<Fn>();
    constexpr std::string_view name_override = std::string_view(meta.name.data);
    constexpr std::string_view name = name_override.empty() ? fn_name<Fn>() : name_override;
    constexpr std::string_view schema = schema_sv<Fn>();
    return Action{
        .name = name,
        .title = std::string_view(meta.title.data),
        .description = std::string_view(meta.description.data),
        .input_schema = schema,
        .is_read_only = meta.is_read_only,
        .is_destructive = meta.is_destructive,
        .is_idempotent = meta.is_idempotent,
        .is_open_world = meta.is_open_world,
        .async = meta.async,
        .call = [](std::string_view raw_line, ActionContext& ctx) -> ToolResult {
            return invoker<Group, Fn, tool_serializer<Config>::template type, Parser>::call(raw_line, ctx);
        },
        .authorized = make_authorized<Group, Fn>(),
    };
}

template <meta::info Group, meta::info Fn, typename Parser>
constexpr Prompt make_prompt() {
    static constexpr auto meta = tool_of<Fn>();
    constexpr std::string_view name_override = std::string_view(meta.name.data);
    constexpr std::string_view name = name_override.empty() ? fn_name<Fn>() : name_override;
    constexpr std::string_view args_schema = prompt_args_sv<Fn>();
    constexpr meta::info completion_fn = find_completion<Group, Fn>();
    Prompt p{
        .name = name,
        .description = std::string_view(meta.description.data),
        .arguments_schema = args_schema,
        .messages = [](std::string_view raw_line) -> std::string {
            ActionContext dummy;
            return invoker<Group, Fn, json_serializer, Parser>::call_value(raw_line, dummy);   // raw messages text
        },
        .authorized = make_authorized<Group, Fn>(),
    };
    if constexpr (completion_fn != meta::info{}) {
        p.complete = &[: completion_fn :];
    }
    return p;
}

/// -- registry: discover every tool/prompt of a group -------------------------
///
/// A function is registered as a tool/prompt iff it carries a `tool` decorator.
/// Completion hooks carry `complete_for` (or nothing, relying on the `<prompt>_complete`
/// name convention), so they are automatically excluded from registration.
consteval bool is_registered(meta::info m) {
    return meta::is_function(m) && meta::has_identifier(m) && find_meta(m) != meta::info{};
}

consteval std::size_t func_member_count(meta::info group) {
    auto ms = meta::members_of(group, meta::access_context::current());
    std::size_t n = 0;
    for (meta::info m : ms) {
        if (is_registered(m)) ++n;
    }
    return n;
}

template <meta::info Group>
consteval std::array<meta::info, func_member_count(Group)> func_members() {
    auto ms = meta::members_of(Group, meta::access_context::current());
    std::array<meta::info, func_member_count(Group)> out{};
    std::size_t j = 0;
    for (meta::info m : ms) {
        if (is_registered(m)) out[j++] = m;
    }
    return out;
}

template <meta::info Group, typename Parser, std::size_t... Is>
constexpr std::array<Action, sizeof...(Is)> register_tools_impl(std::index_sequence<Is...>) {
    constexpr auto fns = func_members<Group>();
    return { make_tool<Group, fns[Is], Parser>()... };
}

/// Register every `tool`-tagged member of `Group` as an `Action`.
/// @tparam Group The tool group struct (pass `^^Group`).
/// @tparam Parser Backend JSON parser (see json-view.hpp).
/// @return A static array of `Action`, one per registered tool.
template <meta::info Group, typename Parser>
constexpr const std::array<Action, func_member_count(Group)>& register_tools() {
    static constexpr auto value =
        register_tools_impl<Group, Parser>(std::make_index_sequence<func_member_count(Group)>());
    return value;
}

template <meta::info Group, typename Parser, std::size_t... Is>
constexpr std::array<Prompt, sizeof...(Is)> register_prompts_impl(std::index_sequence<Is...>) {
    constexpr auto fns = func_members<Group>();
    return { make_prompt<Group, fns[Is], Parser>()... };
}

/// Register every `tool`-tagged member of `Group` as a `Prompt`.
/// @tparam Group The tool group struct (pass `^^Group`).
/// @tparam Parser Backend JSON parser (see json-view.hpp).
/// @return A static array of `Prompt`, one per registered tool.
template <meta::info Group, typename Parser>
constexpr const std::array<Prompt, func_member_count(Group)>& register_prompts() {
    static constexpr auto value =
        register_prompts_impl<Group, Parser>(std::make_index_sequence<func_member_count(Group)>());
    return value;
}

/// -- tagged-group discovery --------------------------------------------------
///
/// `struct [[= mcp::tool_group{} ]] X { ... };` marks a tool group;
/// `struct [[= mcp::prompt_group{} ]] Y { ... };` marks a prompt group.
/// `register_tagged_tools<^^ns>()` / `register_tagged_prompts<^^ns>()` enumerate
/// the namespace, keep every struct tagged with the matching tag, and register
/// its members, no manual list. Groups mix freely with untagged structs in the
/// same namespace (untagged is skipped).

template <meta::info Tag>
consteval bool is_tagged_group(meta::info m) {
    // is_class_type throws for non-type entities (e.g. a class template in the
    // scanned namespace); query is_type first so the scan tolerates them.
    if (!meta::is_type(m) || !meta::is_class_type(m)) return false;
    for (meta::info a : meta::annotations_of(m)) {
        // Tags are class templates (tool_group<Auth> / prompt_group<Auth>);
        // match any instantiation by template.
        if (meta::template_of(meta::remove_const(meta::type_of(a))) == Tag) return true;
    }
    return false;
}

template <meta::info NS, meta::info Tag>
consteval std::size_t tagged_group_count() {
    std::size_t n = 0;
    for (meta::info m : meta::members_of(NS, meta::access_context::current())) {
        if (is_tagged_group<Tag>(m)) ++n;
    }
    return n;
}

template <meta::info NS, meta::info Tag>
consteval std::array<meta::info, tagged_group_count<NS, Tag>()> tagged_groups() {
    std::array<meta::info, tagged_group_count<NS, Tag>()> out{};
    std::size_t j = 0;
    for (meta::info m : meta::members_of(NS, meta::access_context::current())) {
        if (is_tagged_group<Tag>(m)) out[j++] = m;
    }
    return out;
}

/// Concatenate per-group arrays into one static array (`T` trivially copyable).
/// @tparam T Element type.
/// @return The concatenated array.
template <typename T, std::size_t... Ns>
constexpr std::array<T, (Ns + ...)> array_concat(const std::array<T, Ns>&... arrs) {
    std::array<T, (Ns + ...)> out{};
    std::size_t pos = 0;
    ((std::copy(arrs.begin(), arrs.end(), out.begin() + static_cast<long>(pos)),
      pos += arrs.size()),
     ...);
    return out;
}

template <meta::info NS, meta::info Tag>
consteval std::size_t tagged_total() {
    constexpr auto gs = tagged_groups<NS, Tag>();
    std::size_t n = 0;
    for (auto g : gs) n += func_member_count(g);
    return n;
}

template <meta::info NS, meta::info Tag, typename Parser, std::size_t... Is>
constexpr std::array<Action, (func_member_count(tagged_groups<NS, Tag>()[Is]) + ...)>
reg_tagged_tools_helper(std::index_sequence<Is...>) {
    constexpr auto gs = tagged_groups<NS, Tag>();
    return array_concat(register_tools_impl<gs[Is], Parser>(std::make_index_sequence<func_member_count(gs[Is])>())...);
}

/// Register the tools of every struct in `NS` tagged with `Tag`.
/// @tparam NS Namespace to scan (pass `^^ns`).
/// @tparam Tag The tag template to match (`mcp::tool_group`).
/// @tparam Parser Backend JSON parser (see json-view.hpp).
/// @return A static array of `Action` across all tagged groups.
template <meta::info NS, meta::info Tag, typename Parser>
constexpr const std::array<Action, tagged_total<NS, Tag>()>& register_tagged_tools() {
    static constexpr auto value =
        reg_tagged_tools_helper<NS, Tag, Parser>(std::make_index_sequence<tagged_group_count<NS, Tag>()>());
    return value;
}

template <meta::info NS, meta::info Tag, typename Parser, std::size_t... Is>
constexpr std::array<Prompt, (func_member_count(tagged_groups<NS, Tag>()[Is]) + ...)>
reg_tagged_prompts_helper(std::index_sequence<Is...>) {
    constexpr auto gs = tagged_groups<NS, Tag>();
    return array_concat(register_prompts_impl<gs[Is], Parser>(std::make_index_sequence<func_member_count(gs[Is])>())...);
}

/// Register the prompts of every struct in `NS` tagged with `Tag`.
/// @tparam NS Namespace to scan (pass `^^ns`).
/// @tparam Tag The tag template to match (`mcp::prompt_group`).
/// @tparam Parser Backend JSON parser (see json-view.hpp).
/// @return A static array of `Prompt` across all tagged groups.
template <meta::info NS, meta::info Tag, typename Parser>
constexpr const std::array<Prompt, tagged_total<NS, Tag>()>& register_tagged_prompts() {
    static constexpr auto value =
        reg_tagged_prompts_helper<NS, Tag, Parser>(std::make_index_sequence<tagged_group_count<NS, Tag>()>());
    return value;
}

} // namespace mcp
