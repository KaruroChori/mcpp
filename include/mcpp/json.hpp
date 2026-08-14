#pragma once

/// @file json.hpp
/// @brief JSON emitter helpers and JSON-RPC response builders.
/// SPDX-License-Identifier: LGPL-3.0

#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

namespace mcp {

/// @brief JSON-RPC 2.0 reserved error codes.
constexpr int64_t RPC_PARSE_ERROR = -32700;
constexpr int64_t RPC_INVALID_REQUEST = -32600;
constexpr int64_t RPC_METHOD_NOT_FOUND = -32601;
constexpr int64_t RPC_INVALID_PARAMS = -32602;
constexpr int64_t RPC_INTERNAL_ERROR = -32603;

/// @brief MCP-specified error codes in the reserved -32020..-32099 range.
constexpr int64_t RPC_HEADER_MISMATCH = -32020;                       ///< HeaderMismatchError
constexpr int64_t RPC_MISSING_REQUIRED_CAPABILITY = -32021;            ///< MissingRequiredClientCapabilityError
constexpr int64_t RPC_UNSUPPORTED_PROTOCOL_VERSION = -32022;           ///< UnsupportedProtocolVersionError

/// @brief Escape a string for inclusion in a JSON string literal.
/// @param s The string to escape.
/// @return The escaped string.
inline std::string json_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

/// @brief Wrap a string in a JSON string literal.
/// @param s The string to wrap and escape.
/// @return A JSON string literal.
inline std::string json_string(std::string_view s) {
    return "\"" + json_escape(s) + "\"";
}

/// @brief Build a JSON object from an initializer list of fields.
/// @param fields Pairs of field name and pre-encoded JSON value.
/// @return A JSON object literal.
inline std::string json_object(std::initializer_list<std::pair<std::string_view, std::string>> fields) {
    std::string out = "{";
    bool first = true;
    for (const auto& [k, v] : fields) {
        if (!first) out += ",";
        first = false;
        out += json_string(k);
        out += ":";
        out += v;
    }
    out += "}";
    return out;
}

/// @brief Build a successful JSON-RPC response for a numeric request id.
/// @param id The request id echoed back.
/// @param result The pre-encoded JSON result value.
/// @return A JSON-RPC 2.0 response string.
inline std::string ok_response(int64_t id, std::string_view result) {
    return "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(id) + ",\"result\":" + std::string(result) + "}\n";
}

/// @brief Build a successful JSON-RPC response for a string request id.
/// @param id The request id echoed back.
/// @param result The pre-encoded JSON result value.
/// @return A JSON-RPC 2.0 response string.
inline std::string ok_response(std::string_view id, std::string_view result) {
    return "{\"jsonrpc\":\"2.0\",\"id\":" + json_string(id) + ",\"result\":" + std::string(result) + "}\n";
}

/// @brief Build a JSON-RPC error response for a numeric request id.
/// @param id The request id echoed back.
/// @param code The JSON-RPC error code.
/// @param message The error message text.
/// @return A JSON-RPC 2.0 error response string.
inline std::string error_response(int64_t id, int64_t code, std::string_view message) {
    return "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(id) +
           ",\"error\":" + json_object({{"code", std::to_string(code)}, {"message", json_string(message)}}) + "}\n";
}

/// @brief Build a JSON-RPC error response for a string request id.
/// @param id The request id echoed back.
/// @param code The JSON-RPC error code.
/// @param message The error message text.
/// @return A JSON-RPC 2.0 error response string.
inline std::string error_response(std::string_view id, int64_t code, std::string_view message) {
    return "{\"jsonrpc\":\"2.0\",\"id\":" + json_string(id) +
           ",\"error\":" + json_object({{"code", std::to_string(code)}, {"message", json_string(message)}}) + "}\n";
}

/// @brief Build a JSON-RPC error response with a `data` member, for a numeric request id.
/// The `data` member carries structured error details, e.g. the MCP
/// UnsupportedProtocolVersionError's `data.supported` / `data.requested`.
/// @param id The request id echoed back.
/// @param code The JSON-RPC error code.
/// @param message The error message text.
/// @param data The pre-encoded JSON value for the `data` member.
/// @return A JSON-RPC 2.0 error response string.
inline std::string error_response(int64_t id, int64_t code, std::string_view message, std::string_view data) {
    return "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(id) +
           ",\"error\":" + json_object({{"code", std::to_string(code)},
                                        {"message", json_string(message)},
                                        {"data", std::string(data)}}) + "}\n";
}

/// @brief Build a JSON-RPC error response with a `data` member, for a string request id.
/// The `data` member carries structured error details, e.g. the MCP
/// UnsupportedProtocolVersionError's `data.supported` / `data.requested`.
/// @param id The request id echoed back.
/// @param code The JSON-RPC error code.
/// @param message The error message text.
/// @param data The pre-encoded JSON value for the `data` member.
/// @return A JSON-RPC 2.0 error response string.
inline std::string error_response(std::string_view id, int64_t code, std::string_view message, std::string_view data) {
    return "{\"jsonrpc\":\"2.0\",\"id\":" + json_string(id) +
           ",\"error\":" + json_object({{"code", std::to_string(code)},
                                        {"message", json_string(message)},
                                        {"data", std::string(data)}}) + "}\n";
}

/// @brief Build a JSON-RPC error response with a null id.
/// Per JSON-RPC 2.0, parse errors and invalid-request errors reply with `id`: null.
/// @param code The JSON-RPC error code.
/// @param message The error message text.
/// @return A JSON-RPC 2.0 error response string.
inline std::string error_response_null(int64_t code, std::string_view message) {
    return "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":" +
           json_object({{"code", std::to_string(code)}, {"message", json_string(message)}}) + "}\n";
}

/// @brief Build a JSON-RPC notification.
/// @param method The method name.
/// @param params The pre-encoded JSON params value.
/// @return A JSON-RPC 2.0 notification string.
inline std::string notification(std::string_view method, std::string_view params) {
    return "{\"jsonrpc\":\"2.0\",\"method\":" + json_string(method) + ",\"params\":" + std::string(params) + "}\n";
}

} // namespace mcp
