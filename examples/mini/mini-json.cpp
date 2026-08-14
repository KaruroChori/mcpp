#include "mini-json.hpp"

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace mini {

namespace {

// JsonOps implementations over a mini::Node (node = &doc.nodes[idx], ctx = &doc).

const mcp::JsonOps* ops();

mcp::JsonType node_type(const void* node, void*) {
    const auto* n = static_cast<const Node*>(node);
    switch (n->kind) {
        case Node::Kind::Null:   return mcp::JsonType::null;
        case Node::Kind::Bool:   return mcp::JsonType::boolean;
        case Node::Kind::Int:
        case Node::Kind::UInt:   return mcp::JsonType::integer;
        case Node::Kind::String: return mcp::JsonType::string;
        case Node::Kind::Array:  return mcp::JsonType::array;
        case Node::Kind::Object: return mcp::JsonType::object;
    }
    return mcp::JsonType::null;
}

std::size_t node_array_size(const void* node, void*) {
    const auto* n = static_cast<const Node*>(node);
    return n->kind == Node::Kind::Array ? n->elements.size() : 0;
}

mcp::JsonView node_array_at(const void* node, void* ctx, std::size_t index) {
    const auto* n = static_cast<const Node*>(node);
    if (n->kind != Node::Kind::Array || index >= n->elements.size()) {
        return mcp::make_view(nullptr, ops(), ctx, false);
    }
    auto* doc = static_cast<Document*>(ctx);
    return mcp::make_view(&doc->nodes[n->elements[index]], ops(), ctx);
}

bool node_bool(const void* node, void*, bool* out) {
    const auto* n = static_cast<const Node*>(node);
    if (n->kind != Node::Kind::Bool) return false;
    *out = n->b;
    return true;
}

bool node_int64(const void* node, void*, int64_t* out) {
    const auto* n = static_cast<const Node*>(node);
    if (n->kind != Node::Kind::Int) return false;
    *out = n->i;
    return true;
}

bool node_uint64(const void* node, void*, uint64_t* out) {
    const auto* n = static_cast<const Node*>(node);
    if (n->kind != Node::Kind::UInt) return false;
    *out = n->u;
    return true;
}

bool node_string(const void* node, void*, std::string_view* out) {
    const auto* n = static_cast<const Node*>(node);
    if (n->kind != Node::Kind::String) return false;
    *out = n->s;
    return true;
}

bool node_double(const void* node, void*, double* out) {
    const auto* n = static_cast<const Node*>(node);
    if (n->kind == Node::Kind::Int) {
        *out = static_cast<double>(n->i);
        return true;
    }
    if (n->kind == Node::Kind::UInt) {
        *out = static_cast<double>(n->u);
        return true;
    }
    return false;
}

mcp::JsonView node_child(const void* node, void* ctx, std::string_view key) {
    const auto* n = static_cast<const Node*>(node);
    auto* doc = static_cast<Document*>(ctx);
    for (const auto& [k, idx] : n->members) {
        if (k == key) return mcp::make_view(&doc->nodes[idx], ops(), ctx);
    }
    return mcp::make_view(nullptr, ops(), ctx, false);
}

const mcp::JsonOps* ops() {
    static const mcp::JsonOps value = {
        .type = node_type,
        .get_bool = node_bool,
        .get_int64 = node_int64,
        .get_uint64 = node_uint64,
        .get_double = node_double,
        .get_string = node_string,
        .child = node_child,
        .array_size = node_array_size,
        .array_at = node_array_at,
    };
    return &value;
}

// -- tiny recursive-descent parser -------------------------------------------

struct Cursor {
    std::string_view src;
    size_t pos = 0;

    void skip_ws() {
        while (pos < src.size() && (src[pos] == ' ' || src[pos] == '\t' || src[pos] == '\n' || src[pos] == '\r')) {
            ++pos;
        }
    }
    bool peek(char c) const { return pos < src.size() && src[pos] == c; }
    bool match(char c) {
        if (peek(c)) { ++pos; return true; }
        return false;
    }
};

bool parse_value(Cursor& cur, Document& doc, size_t& out_index);

bool parse_string(Cursor& cur, std::string_view& out) {
    if (!cur.match('"')) return false;
    const size_t begin = cur.pos;
    while (cur.pos < cur.src.size() && cur.src[cur.pos] != '"') ++cur.pos;
    if (cur.pos >= cur.src.size()) return false;
    out = cur.src.substr(begin, cur.pos - begin);  // no escape handling (deliberate)
    ++cur.pos;
    return true;
}

bool parse_array(Cursor& cur, Document& doc, size_t& out_index) {
    if (!cur.match('[')) return false;
    doc.nodes.emplace_back();
    const size_t idx = doc.nodes.size() - 1;
    cur.skip_ws();
    if (cur.match(']')) { out_index = idx; return true; }
    while (true) {
        cur.skip_ws();
        size_t child = 0;
        if (!parse_value(cur, doc, child)) return false;
        doc.nodes[idx].kind = Node::Kind::Array;
        doc.nodes[idx].elements.push_back(child);
        cur.skip_ws();
        if (cur.match(']')) break;
        if (!cur.match(',')) return false;
    }
    out_index = idx;
    return true;
}

bool parse_object(Cursor& cur, Document& doc, size_t& out_index) {
    if (!cur.match('{')) return false;
    doc.nodes.emplace_back();
    const size_t idx = doc.nodes.size() - 1;
    cur.skip_ws();
    if (cur.match('}')) { out_index = idx; return true; }
    while (true) {
        cur.skip_ws();
        std::string_view key;
        if (!parse_string(cur, key)) return false;
        cur.skip_ws();
        if (!cur.match(':')) return false;
        cur.skip_ws();
        size_t child = 0;
        if (!parse_value(cur, doc, child)) return false;
        doc.nodes[idx].kind = Node::Kind::Object;
        doc.nodes[idx].members.emplace_back(key, child);
        cur.skip_ws();
        if (cur.match('}')) break;
        if (!cur.match(',')) return false;
    }
    out_index = idx;
    return true;
}

bool parse_number(Cursor& cur, Document& doc, size_t& out_index) {
    const size_t begin = cur.pos;
    bool negative = false;
    if (cur.peek('-')) { negative = true; ++cur.pos; }
    if (cur.pos >= cur.src.size() || !std::isdigit(static_cast<unsigned char>(cur.src[cur.pos]))) return false;
    while (cur.pos < cur.src.size() && std::isdigit(static_cast<unsigned char>(cur.src[cur.pos]))) ++cur.pos;
    // Reject floats for simplicity: treat as integer only. Slice the digits
    // without the leading sign: strtoull also parses a sign, so passing the
    // '-' in `text` would double-negate and lose it (-123 -> 123).
    const std::string_view text = cur.src.substr(begin + (negative ? 1 : 0), cur.pos - begin - (negative ? 1 : 0));
    doc.nodes.emplace_back();
    const size_t idx = doc.nodes.size() - 1;
    if (negative) {
        doc.nodes[idx].kind = Node::Kind::Int;
        doc.nodes[idx].i = -static_cast<int64_t>(std::strtoull(std::string(text).c_str(), nullptr, 10));
    } else {
        const uint64_t v = std::strtoull(std::string(text).c_str(), nullptr, 10);
        if (v <= static_cast<uint64_t>(INT64_MAX)) {
            doc.nodes[idx].kind = Node::Kind::Int;
            doc.nodes[idx].i = static_cast<int64_t>(v);
        } else {
            doc.nodes[idx].kind = Node::Kind::UInt;
            doc.nodes[idx].u = v;
        }
    }
    out_index = idx;
    return true;
}

bool parse_literal(Cursor& cur, Document& doc, size_t& out_index) {
    doc.nodes.emplace_back();
    const size_t idx = doc.nodes.size() - 1;
    if (cur.src.substr(cur.pos, 4) == "true") {
        doc.nodes[idx].kind = Node::Kind::Bool;
        doc.nodes[idx].b = true;
        cur.pos += 4;
    } else if (cur.src.substr(cur.pos, 5) == "false") {
        doc.nodes[idx].kind = Node::Kind::Bool;
        cur.pos += 5;
    } else if (cur.src.substr(cur.pos, 4) == "null") {
        doc.nodes[idx].kind = Node::Kind::Null;
        cur.pos += 4;
    } else {
        return false;
    }
    out_index = idx;
    return true;
}

bool parse_value(Cursor& cur, Document& doc, size_t& out_index) {
    cur.skip_ws();
    if (cur.peek('{')) return parse_object(cur, doc, out_index);
    if (cur.peek('[')) return parse_array(cur, doc, out_index);
    if (cur.peek('"')) {
        std::string_view s;
        if (!parse_string(cur, s)) return false;
        doc.nodes.emplace_back();
        const size_t idx = doc.nodes.size() - 1;
        doc.nodes[idx].kind = Node::Kind::String;
        doc.nodes[idx].s = s;
        out_index = idx;
        return true;
    }
    if (cur.peek('-') || std::isdigit(static_cast<unsigned char>(cur.src[cur.pos]))) {
        return parse_number(cur, doc, out_index);
    }
    return parse_literal(cur, doc, out_index);
}

} // namespace

bool parse(std::string_view source, Document& doc) {
    doc.nodes.clear();
    doc.ok = false;
    Cursor cur{ source, 0 };
    size_t root_idx = 0;
    if (!parse_value(cur, doc, root_idx)) return false;
    cur.skip_ws();
    if (cur.pos != source.size()) return false;
    doc.ok = true;
    return true;
}

mcp::JsonView root(const Document& doc) {
    if (!doc.ok || doc.nodes.empty()) return mcp::make_view(nullptr, ops(), const_cast<Document*>(&doc), false);
    return mcp::make_view(&doc.nodes[0], ops(), const_cast<Document*>(&doc));
}

mcp::JsonView parse_arguments(std::string_view raw_line, Document& doc) {
    if (!parse(raw_line, doc)) return mcp::make_view(nullptr, ops(), &doc, false);
    return root(doc)["params"]["arguments"];
}

std::string_view arg_string(const mcp::JsonView& args, std::string_view name) {
    std::string_view out;
    args[name].get_string(out);
    return out;
}

int64_t arg_int(const mcp::JsonView& args, std::string_view name) {
    int64_t out = 0;
    args[name].get_int64(out);
    return out;
}

// -- Parser backend -----------------------------------------------------------

const mcp::JsonOps* parser::ops() {
    return mini::ops();
}

bool parser::parse(std::string_view source, document& doc, mcp::JsonView& root_out) {
    if (!mini::parse(source, doc)) return false;
    root_out = root(doc);
    return true;
}

} // namespace mini
