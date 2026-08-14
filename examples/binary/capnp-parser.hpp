#pragma once

// A Parser backend for the mcpp `Parser` concept (json-view.hpp) that reads a
// tools/call request from a Cap'n Proto message instead of JSON text.
//
// The whole point of the seam: the protocol core and the reflection layer never
// touch JSON directly — they go through JsonView + JsonOps. So by providing a
// Parser whose `document` is a decoded Cap'n Proto message and whose JsonOps
// read from it, the EXACT same registered tools work over a binary framing with
// the schema outside the dataframe. See tools.capnp for the envelope schema.
//
// Mapping: the Cap'n Proto message mirrors the JSON shape the invoker expects:
//   root["params"]["arguments"][field]
// JsonOps child() descends request -> params -> arguments -> a named argument
// field; getters read the typed field out of the struct.

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include <capnp/message.h>
#include <capnp/serialize.h>

#include <mcpp/json-view.hpp>

#include "tools.capnp.h"

namespace binary {

struct capnp_parser {
    struct document {
        // Owned, aligned storage for the Cap'n Proto words. The reader points
        // into it and every string_view handed out by get_string() borrows the
        // message, so this must outlive all views.
        std::vector<capnp::word> words;
        std::unique_ptr<capnp::FlatArrayMessageReader> reader;

        // Arena for the node values handed out through JsonView. Each child()
        // call produces a new value view that must stay valid for the request's
        // lifetime, so we keep them all alive in a vector keyed by node index.
        struct Value {
            enum class Kind : uint8_t { Null, Object, UInt, Int, Bool, String } kind = Kind::Null;
            // Object nodes: which level of the envelope (0=request, 1=params,
            // 2=arguments).
            int level = 0;
            // Leaf values.
            uint64_t u = 0;
            int64_t i = 0;
            bool b = false;
            std::string_view s;
        };
        std::vector<Value> arena;
    };

    static const mcp::JsonOps* ops();
    static bool parse(std::string_view bytes, document& doc, mcp::JsonView& root);
};

namespace detail {

// capnp::Text::Reader -> std::string_view (borrows the message, which lives in
// the document).
inline std::string_view to_sv(capnp::Text::Reader text) {
    return std::string_view(text.begin(), text.size());
}

mcp::JsonType value_type(const void* node, void*) {
    const auto* v = static_cast<const capnp_parser::document::Value*>(node);
    switch (v->kind) {
    case capnp_parser::document::Value::Kind::Object: return mcp::JsonType::object;
    case capnp_parser::document::Value::Kind::UInt:
    case capnp_parser::document::Value::Kind::Int:    return mcp::JsonType::integer;
    case capnp_parser::document::Value::Kind::Bool:   return mcp::JsonType::boolean;
    case capnp_parser::document::Value::Kind::String: return mcp::JsonType::string;
    case capnp_parser::document::Value::Kind::Null:   return mcp::JsonType::null;
    }
    return mcp::JsonType::null;
}

bool value_bool(const void* node, void*, bool* out) {
    const auto* v = static_cast<const capnp_parser::document::Value*>(node);
    if (v->kind != capnp_parser::document::Value::Kind::Bool) return false;
    *out = v->b;
    return true;
}

bool value_int64(const void* node, void*, int64_t* out) {
    const auto* v = static_cast<const capnp_parser::document::Value*>(node);
    if (v->kind != capnp_parser::document::Value::Kind::Int) return false;
    *out = v->i;
    return true;
}

bool value_uint64(const void* node, void*, uint64_t* out) {
    const auto* v = static_cast<const capnp_parser::document::Value*>(node);
    if (v->kind != capnp_parser::document::Value::Kind::UInt) return false;
    *out = v->u;
    return true;
}

bool value_double(const void* node, void*, double* out) {
    const auto* v = static_cast<const capnp_parser::document::Value*>(node);
    if (v->kind == capnp_parser::document::Value::Kind::Int) {
        *out = static_cast<double>(v->i);
        return true;
    }
    if (v->kind == capnp_parser::document::Value::Kind::UInt) {
        *out = static_cast<double>(v->u);
        return true;
    }
    return false;
}

bool value_string(const void* node, void*, std::string_view* out) {
    const auto* v = static_cast<const capnp_parser::document::Value*>(node);
    if (v->kind != capnp_parser::document::Value::Kind::String) return false;
    *out = v->s;
    return true;
}

// Descend into a child of the envelope. The message is decoded once into the
// document's reader; child() walks the Cap'n Proto structs by field.
mcp::JsonView value_child(const void* node, void* ctx, std::string_view key) {
    auto* doc = static_cast<capnp_parser::document*>(ctx);
    const auto* parent = static_cast<const capnp_parser::document::Value*>(node);

    auto absent = [&] { return mcp::make_view(nullptr, capnp_parser::ops(), ctx, false); };
    auto push = [&](capnp_parser::document::Value v) {
        doc->arena.push_back(v);
        return mcp::make_view(&doc->arena.back(), capnp_parser::ops(), ctx);
    };

    if (parent->kind != capnp_parser::document::Value::Kind::Object) return absent();

    switch (parent->level) {
    case 0: // request: {"params": {...}}
        if (key == "params") {
            capnp_parser::document::Value v;
            v.kind = capnp_parser::document::Value::Kind::Object;
            v.level = 1;
            return push(v);
        }
        return absent();
    case 1: // params: {"name": ..., "arguments": {...}}
        if (key == "arguments") {
            capnp_parser::document::Value v;
            v.kind = capnp_parser::document::Value::Kind::Object;
            v.level = 2;
            return push(v);
        }
        if (key == "name") {
            capnp_parser::document::Value v;
            v.kind = capnp_parser::document::Value::Kind::String;
            v.s = to_sv(doc->reader->getRoot<CallRequest>().getParams().getName());
            return push(v);
        }
        return absent();
    case 2: { // arguments: one named field per tool argument
        const auto args = doc->reader->getRoot<CallRequest>().getParams().getArguments();
        capnp_parser::document::Value v;
        if (key == "handle") {
            v.kind = capnp_parser::document::Value::Kind::UInt;
            v.u = args.getHandle();
        } else if (key == "direction") {
            v.kind = capnp_parser::document::Value::Kind::String;
            v.s = to_sv(args.getDirection());
        } else if (key == "radius") {
            v.kind = capnp_parser::document::Value::Kind::Int;
            v.i = args.getRadius();
        } else if (key == "payload") {
            v.kind = capnp_parser::document::Value::Kind::String;
            v.s = to_sv(args.getPayload());
        } else {
            return absent();
        }
        return push(v);
    }
    default:
        return absent();
    }
}

inline const mcp::JsonOps* ops() {
    static const mcp::JsonOps value = {
        .type = value_type,
        .get_bool = value_bool,
        .get_int64 = value_int64,
        .get_uint64 = value_uint64,
        .get_double = value_double,
        .get_string = value_string,
        .child = value_child,
    };
    return &value;
}

} // namespace detail

inline const mcp::JsonOps* capnp_parser::ops() { return detail::ops(); }

inline bool capnp_parser::parse(std::string_view bytes, document& doc, mcp::JsonView& root) {
    // Copy the raw bytes into aligned word storage, then decode the message.
    const size_t n_words = (bytes.size() + sizeof(capnp::word) - 1) / sizeof(capnp::word);
    doc.words.assign(n_words, capnp::word{});
    if (!bytes.empty()) std::memcpy(doc.words.data(), bytes.data(), bytes.size());
    doc.reader = std::make_unique<capnp::FlatArrayMessageReader>(
        kj::ArrayPtr<const capnp::word>(doc.words.data(), n_words));

    doc.arena.clear();
    capnp_parser::document::Value v;
    v.kind = capnp_parser::document::Value::Kind::Object;
    v.level = 0;
    doc.arena.push_back(v);
    root = mcp::make_view(&doc.arena[0], ops(), &doc);
    return true;
}

} // namespace binary
