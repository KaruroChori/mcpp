#pragma once

/// @file simdjson-ops.hpp
/// @brief simdjson backend for the mcpp Parser concept.
/// SPDX-License-Identifier: LGPL-3.0

#include <simdjson.h>

#include <string_view>
#include <vector>

#include <mcpp/json-view.hpp>

namespace mcp {

/// @brief The simdjson parser backend for the mcpp core's Parser concept.
/// @see json-view.hpp's Parser concept
///
/// This is the concrete JSON implementation the extended server uses to parse
/// requests and that reflected tools use to read their arguments.
///
/// The document owns the parsed storage: a simdjson document plus an arena of
/// child elements (simdjson elements are cheap handles into the document tape,
/// but the handle objects must outlive the views). parse() fills a caller-owned
/// document and hands back a root JsonView into it.
struct simdjson_parser {
    struct document {
        simdjson::dom::document doc;
        simdjson::dom::element root;   ///< root handle; lives as long as the document
        std::vector<simdjson::dom::element> arena;
    };

    static const JsonOps* ops();

    /// @brief Parses `text` into `doc`.
    ///
    /// A caller that owns a long-lived simdjson::dom::parser (e.g. the Server)
    /// can pass it via `reuse` so the parse buffer is reused across requests
    /// instead of allocated per call; `nullptr` (the default, used by reflected
    /// tools) allocates a fresh one.
    ///
    /// @param text The JSON text to parse.
    /// @param doc The caller-owned document that receives the parsed storage.
    /// @param root Receives the root JsonView into `doc`.
    /// @param reuse Optional reusable simdjson::dom::parser; nullptr allocates
    /// a fresh one.
    /// @return True on success, false on parse failure.
    static bool parse(std::string_view text, document& doc, JsonView& root,
                      simdjson::dom::parser* reuse = nullptr);
};

} // namespace mcp
