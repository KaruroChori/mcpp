#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include <mcpp/json-view.hpp>

namespace mini {

/**
 * A deliberately small JSON parser implementing mcp::JsonOps. This is the
 * "fast, not fully correct" parser the library is designed to accept: it
 * understands objects, arrays, strings, integers, booleans and null (no
 * string escapes, no float literals — numbers are an int/uint split). It is
 * enough to drive the whole conformance suite against mcp::Protocol — the
 * point being that the protocol core has no dependency on any particular
 * parser.
 */
struct Node {
    enum class Kind { Null, Bool, Int, UInt, String, Array, Object } kind = Kind::Null;
    bool b = false;
    int64_t i = 0;
    uint64_t u = 0;
    std::string_view s;                       // points into the source buffer
    std::vector<std::pair<std::string_view, size_t>> members;  // object: key -> child index
    std::vector<size_t> elements;             // array: child indices
};

/** A parse result: the node arena plus the parsed root. Outlives the views. */
struct Document {
    std::vector<Node> nodes;
    bool ok = false;
};

/**
 * The mini parser backend for the mcpp core's Parser concept (json-view.hpp):
 * owns a Document and hands back a root JsonView into it.
 */
struct parser {
    using document = Document;
    static const mcp::JsonOps* ops();
    static bool parse(std::string_view source, document& doc, mcp::JsonView& root);
};

/** Parses `source` into `doc`. Returns false on a syntax error. */
bool parse(std::string_view source, Document& doc);

/** Builds a JsonView over the parsed root. The doc must outlive the view. */
mcp::JsonView root(const Document& doc);

/** Convenience: parse then hand back the arguments object of a tools/call. */
mcp::JsonView parse_arguments(std::string_view raw_line, Document& doc);

/** Reads a string argument by name from an arguments view ("" if absent). */
std::string_view arg_string(const mcp::JsonView& args, std::string_view name);

/** Reads an int64 argument by name from an arguments view (0 if absent). */
int64_t arg_int(const mcp::JsonView& args, std::string_view name);

} // namespace mini
