#pragma once

/// @file json-view.hpp
/// @brief Parser-independent JSON read surface (JsonView, JsonOps, the Parser concept).
/// SPDX-License-Identifier: LGPL-3.0

#include <cstdint>
#include <string_view>

namespace mcp {

/// A parser-independent read-only JSON value. The concrete parse (simdjson,
/// the mini parser in examples/mini, etc.) is hidden behind a small ops table,
/// so the protocol core can read requests without depending on any JSON
/// library. A `JsonView` is a borrowed handle: the node it points at is owned
/// by the backend (typically the adapter's document + a per-message arena),
/// which must outlive every use of the view.
enum class JsonType { null, boolean, integer, number, string, array, object };

struct JsonView {
    const void* node = nullptr;       ///< backend-owned; opaque to the core
    void* ctx = nullptr;              ///< backend-owned arena/context
    const struct JsonOps* ops = nullptr;
    bool present_ = false;            ///< key existed (vs JSON null / absent)

    JsonType type() const;
    bool is_null() const { return type() == JsonType::null; }
    bool get_bool(bool& out) const;
    bool get_int64(int64_t& out) const;
    bool get_uint64(uint64_t& out) const;
    bool get_double(double& out) const;
    bool get_string(std::string_view& out) const;

    /// Child by key. Absent keys yield a view with present() == false.
    JsonView operator[](std::string_view key) const;

    /// Number of elements if this is an array, else 0.
    std::size_t array_size() const;

    /// The i-th element if this is an array, else an absent view.
    JsonView array_at(std::size_t index) const;

    /// Whether the key existed (as opposed to holding a JSON null).
    bool present() const { return present_; }
};

/// Function-pointer table implementing the read surface of JsonView for one
/// parser backend. `ctx` is passed through from the view so a backend can use a
/// per-message arena (e.g. to store child elements). A backend provides one
/// instance and hands it to the views it creates (see make_view).
struct JsonOps {
    JsonType (*type)(const void* node, void* ctx);
    bool (*get_bool)(const void* node, void* ctx, bool* out);
    bool (*get_int64)(const void* node, void* ctx, int64_t* out);
    bool (*get_uint64)(const void* node, void* ctx, uint64_t* out);
    bool (*get_double)(const void* node, void* ctx, double* out);
    bool (*get_string)(const void* node, void* ctx, std::string_view* out);
    ///< Child lookup; must set the returned view's node/present_ correctly
    ///< (present_ = false when the key is absent).
    JsonView (*child)(const void* node, void* ctx, std::string_view key);
    ///< Array support (for array-typed tool arguments). Both must tolerate
    ///< non-array nodes (size 0 / absent element); a backend without array
    ///< support leaves them null (JsonView treats null as size 0 / absent).
    std::size_t (*array_size)(const void* node, void* ctx);
    JsonView (*array_at)(const void* node, void* ctx, std::size_t index);
};

inline JsonView make_view(const void* node, const JsonOps* ops, void* ctx = nullptr, bool present = true) {
    return JsonView{ node, ctx, ops, present };
}

/// ---------------------------------------------------------------------------
/// JsonView members (trivial dispatch through the ops table).
/// ---------------------------------------------------------------------------

inline JsonType JsonView::type() const {
    return node && ops ? ops->type(node, ctx) : JsonType::null;
}
inline bool JsonView::get_bool(bool& out) const {
    return node && ops && ops->get_bool(node, ctx, &out);
}
inline bool JsonView::get_int64(int64_t& out) const {
    return node && ops && ops->get_int64(node, ctx, &out);
}
inline bool JsonView::get_uint64(uint64_t& out) const {
    return node && ops && ops->get_uint64(node, ctx, &out);
}
inline bool JsonView::get_double(double& out) const {
    return node && ops && ops->get_double(node, ctx, &out);
}
inline bool JsonView::get_string(std::string_view& out) const {
    return node && ops && ops->get_string(node, ctx, &out);
}
inline JsonView JsonView::operator[](std::string_view key) const {
    /// Never dispatch into a null/absent node: an absent parent yields an
    /// absent child (a backend's child() may not tolerate a null node).
    if (!node) return JsonView{};
    return ops ? ops->child(node, ctx, key) : JsonView{};
}
inline std::size_t JsonView::array_size() const {
    return node && ops && ops->array_size ? ops->array_size(node, ctx) : 0;
}
inline JsonView JsonView::array_at(std::size_t index) const {
    if (!node || !ops || !ops->array_at) return JsonView{};
    return ops->array_at(node, ctx, index);
}

/// Where the protocol core hands finished output. The adapter implements
/// `write` (typically mutex-guarded -> stream).
struct Sink {
    void (*write)(void* ctx, std::string_view line) = nullptr;
    void* ctx = nullptr;
};

/// A JSON parser backend, as used by the reflection invoker and by the concrete
/// servers. A parser is a plain type satisfying:
///
///   struct MyParser {
///       struct document;                        // owns parsed storage
///       static const JsonOps* ops();            // read ops for this backend
///       static bool parse(std::string_view text,
///                         document& doc,        // caller-owned storage
///                         JsonView& root);      // out: root view into doc
///   };
///
/// `parse` fills the caller-owned document and hands back a root JsonView whose
/// node/ctx/ops all point into (or are valid for) that document. The document
/// must outlive every view derived from it. This keeps JsonView a borrowed
/// handle and keeps the core free of any concrete parser (simdjson, the mini
/// parser, etc. are all just `Parser` implementations).
template <typename Parser>
struct json_parser {};

/// The default parser: intentionally provides nothing, so instantiating a
/// reflected tool without choosing a real parser fails loudly at compile time.
/// The public registrars (register_tools<Group, Parser> etc.) force an explicit
/// Parser argument; this exists only to keep internal templates defaultable.
struct no_parser {
    struct document {};
    static const JsonOps* ops() { return nullptr; }
    static bool parse(std::string_view, document&, JsonView&) { return false; }
};

} // namespace mcp
