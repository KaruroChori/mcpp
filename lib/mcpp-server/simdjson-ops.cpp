#include <mcpp-server/simdjson-ops.hpp>

namespace mcp {

namespace {

/// @file simdjson-ops.cpp
/// @brief simdjson JsonOps backend implementation.
///
/// This file provides the simdjson `JsonOps` table used by JsonView: the type
/// and scalar getter adapters, the child lookup, and the array_size/array_at
/// adapters. `simdjson_parser::parse` optionally reuses a caller-supplied
/// parser instead of allocating a local one.
///
/// A simdjson `dom::element` is a cheap handle into the document's tape, but
/// the handle object itself must outlive the JsonView. The adapters keep a
/// per-message arena (`std::vector<dom::element>`) and copy found children and
/// array elements into it; the arena outlives one dispatch.
/// SPDX-License-Identifier: LGPL-3.0

const JsonOps& simd_ops();

JsonType simd_type(const void* node, void*) {
    const auto* e = static_cast<const simdjson::dom::element*>(node);
    if (e->is_null()) return JsonType::null;
    switch (e->type()) {
        case simdjson::dom::element_type::BOOL:   return JsonType::boolean;
        case simdjson::dom::element_type::INT64:  return JsonType::integer;
        case simdjson::dom::element_type::UINT64: return JsonType::integer;
        case simdjson::dom::element_type::DOUBLE: return JsonType::number;
        case simdjson::dom::element_type::STRING: return JsonType::string;
        case simdjson::dom::element_type::ARRAY:  return JsonType::array;
        case simdjson::dom::element_type::OBJECT: return JsonType::object;
    }
    return JsonType::null;
}

bool simd_get_bool(const void* node, void*, bool* out) {
    return static_cast<const simdjson::dom::element*>(node)->get_bool().get(*out) == simdjson::SUCCESS;
}

bool simd_get_int64(const void* node, void*, int64_t* out) {
    return static_cast<const simdjson::dom::element*>(node)->get_int64().get(*out) == simdjson::SUCCESS;
}

bool simd_get_uint64(const void* node, void*, uint64_t* out) {
    return static_cast<const simdjson::dom::element*>(node)->get_uint64().get(*out) == simdjson::SUCCESS;
}

bool simd_get_double(const void* node, void*, double* out) {
    return static_cast<const simdjson::dom::element*>(node)->get_double().get(*out) == simdjson::SUCCESS;
}

bool simd_get_string(const void* node, void*, std::string_view* out) {
    return static_cast<const simdjson::dom::element*>(node)->get_string().get(*out) == simdjson::SUCCESS;
}

std::size_t simd_array_size(const void* node, void*) {
    const auto* e = static_cast<const simdjson::dom::element*>(node);
    simdjson::dom::array arr;
    if (e->get_array().get(arr) != simdjson::SUCCESS) return 0;
    return arr.size();
}

JsonView simd_array_at(const void* node, void* ctx, std::size_t index) {
    auto* arena = static_cast<std::vector<simdjson::dom::element>*>(ctx);
    const auto* e = static_cast<const simdjson::dom::element*>(node);
    simdjson::dom::array arr;
    if (e->get_array().get(arr) != simdjson::SUCCESS) {
        return make_view(nullptr, &simd_ops(), arena, false);
    }
    simdjson::dom::element out;
    if (arr.at(index).get(out) != simdjson::SUCCESS) {
        return make_view(nullptr, &simd_ops(), arena, false);
    }
    arena->push_back(out);
    return make_view(&arena->back(), &simd_ops(), arena);
}

JsonView simd_child(const void* node, void* ctx, std::string_view key) {
    auto* arena = static_cast<std::vector<simdjson::dom::element>*>(ctx);
    const auto* e = static_cast<const simdjson::dom::element*>(node);
    const std::string key_str(key);
    const auto child = (*e)[key_str];
    if (child.error() == simdjson::NO_SUCH_FIELD) {
        return make_view(nullptr, nullptr, arena, false);
    }
    simdjson::dom::element out;
    if (child.get(out) != simdjson::SUCCESS) {
        return make_view(nullptr, nullptr, arena, false);
    }
    arena->push_back(out);
    return make_view(&arena->back(), &simd_ops(), arena);
}

const JsonOps& simd_ops() {
    static const JsonOps ops = {
        .type = simd_type,
        .get_bool = simd_get_bool,
        .get_int64 = simd_get_int64,
        .get_uint64 = simd_get_uint64,
        .get_double = simd_get_double,
        .get_string = simd_get_string,
        .child = simd_child,
        .array_size = simd_array_size,
        .array_at = simd_array_at,
    };
    return ops;
}

} // namespace

const JsonOps* simdjson_parser::ops() {
    return &simd_ops();
}

/// @brief Decode `text` into `doc` and expose the document root as a JsonView.
///
/// @param text The JSON text to parse.
/// @param doc The destination document owning the parsed tape and child arena.
/// @param root Receives the JsonView of the document root on success.
/// @param reuse Optional parser to reuse; a local parser is used when null.
/// @return True when parsing succeeded, otherwise false.
bool simdjson_parser::parse(std::string_view text, document& doc, JsonView& root,
                            simdjson::dom::parser* reuse) {
    simdjson::dom::parser local;
    simdjson::dom::parser& p = reuse ? *reuse : local;
    if (p.parse_into_document(doc.doc, text).get(doc.root) != simdjson::SUCCESS) return false;
    doc.arena.clear();
    doc.arena.reserve(16);
    root = make_view(&doc.root, &simd_ops(), &doc.arena);
    return true;
}

} // namespace mcp
