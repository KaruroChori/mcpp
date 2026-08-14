#pragma once

/// @file serializer.hpp
/// @brief Reflection-based JSON result serialization.
/// Runtime JSON serialization of a tool's return value.
///
/// Customization point: specialize `json_serializer<MyType>` to override how a
/// type is serialized. The primary template covers, in dispatch order:
///   string-ish -> JSON string; bool/integral/floating -> JSON scalar;
///   enum -> enumerator name (reflection); std::optional -> value|null;
///   std::variant -> active alternative; std::expected -> value|error;
///   map-like -> JSON object; iterable -> JSON array;
///   user literal struct -> JSON object via member reflection + runtime
///   member-access splices `v.[:member:]`.
/// SPDX-License-Identifier: LGPL-3.0

#include <array>
#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <meta>

#include "json.hpp"

namespace meta = std::meta;

namespace mcp {

template <typename T>
struct is_optional : std::false_type {}; ///< primary template: not an optional
template <typename U>
struct is_optional<std::optional<U>> : std::true_type {}; ///< true for std::optional<U>

template <typename T>
struct is_variant : std::false_type {}; ///< primary template: not a variant
template <typename... U>
struct is_variant<std::variant<U...>> : std::true_type {}; ///< true for std::variant<U...>

template <typename T>
struct is_map : std::false_type {}; ///< primary template: not a map
template <typename K, typename V, typename C, typename A>
struct is_map<std::map<K, V, C, A>> : std::true_type {}; ///< true for std::map
template <typename K, typename V, typename H, typename E, typename A>
struct is_map<std::unordered_map<K, V, H, E, A>> : std::true_type {}; ///< true for std::unordered_map

template <typename T>
struct json_serializer;

namespace ser_detail {

/// Number of nonstatic data members of @tparam T.
template <typename T>
consteval std::size_t member_count() {
    return meta::nonstatic_data_members_of(^^T, meta::access_context::current()).size();
}

/// struct -> object via reflection + runtime member-access splices.
/// @tparam T The literal struct type.
/// @param v The value to serialize.
/// @return @p v serialized as a JSON object.
template <typename T, std::size_t... Is>
std::string struct_impl(const T& v, std::index_sequence<Is...>) {
    static constexpr std::array<meta::info, sizeof...(Is)> members = [] {
        auto ms = meta::nonstatic_data_members_of(^^T, meta::access_context::current());
        std::array<meta::info, sizeof...(Is)> arr{};
        for (std::size_t i = 0; i < sizeof...(Is); ++i) arr[i] = ms[i];
        return arr;
    }();
    std::string out = "{";
    bool first = true;
    ((void)([&] {
        if (!first) out += ",";
        first = false;
        auto&& m = v.[:members[Is]:];
        out += json_string(meta::identifier_of(members[Is])) + ":" +
               json_serializer<std::remove_cvref_t<decltype(m)>>::to_json(m);
    }()), ...);
    out += "}";
    return out;
}

/// Number of enumerators of @tparam T.
template <typename T>
consteval std::size_t enumerator_count() {
    return meta::enumerators_of(^^T).size();
}

/// enum -> enumerator name via reflection.
/// @tparam T The enum type.
/// @param v The value whose enumerator name is serialized.
/// @return The JSON string of the matching enumerator name, or @c "?" if none match.
template <typename T, std::size_t... Is>
std::string enum_impl(T v, std::index_sequence<Is...>) {
    static constexpr std::array<meta::info, sizeof...(Is)> enums = [] {
        auto es = meta::enumerators_of(^^T);
        std::array<meta::info, sizeof...(Is)> arr{};
        for (std::size_t i = 0; i < sizeof...(Is); ++i) arr[i] = es[i];
        return arr;
    }();
    using U = std::underlying_type_t<T>;
    const U uv = static_cast<U>(v);
    std::string name;
    ((void)([&] {
        if (!name.empty()) return;
        // Splice the enumerator constant: meta::extract cannot yield a value here.
        if (static_cast<U>([: enums[Is] :]) == uv) name = std::string(meta::identifier_of(enums[Is]));
    }()), ...);
    return json_string(name.empty() ? "?" : name);
}

} // namespace ser_detail

/// Serialize @p v to its JSON representation.
/// @return The JSON string from @p v's @c json_serializer.
template <typename T>
std::string serialize_result(const T& v) {
    return json_serializer<T>::to_json(v);
}

/// Primary serializer: dispatches @tparam T to the encoding described above.
/// Specialize this template to override the behavior for a given type.
template <typename T>
struct json_serializer {
    static std::string to_json(const T& v) {
        if constexpr (std::is_convertible_v<const T&, std::string_view>) {
            return json_string(std::string_view(v));
        } else if constexpr (std::is_same_v<T, bool>) {
            return v ? "true" : "false";
        } else if constexpr (std::is_integral_v<T>) {
            return std::to_string(v);
        } else if constexpr (std::is_floating_point_v<T>) {
            char buf[64];
            std::snprintf(buf, sizeof buf, "%g", static_cast<double>(v));
            return buf;
        } else if constexpr (std::is_enum_v<T>) {
            return ser_detail::enum_impl(v, std::make_index_sequence<ser_detail::enumerator_count<T>()>());
        } else if constexpr (is_optional<T>::value) {
            return v ? json_serializer<std::remove_cvref_t<decltype(*v)>>::to_json(*v) : "null";
        } else if constexpr (is_variant<T>::value) {
            return std::visit([](const auto& a) {
                return json_serializer<std::remove_cvref_t<decltype(a)>>::to_json(a);
            }, v);
        } else if constexpr (requires { v.has_value(); v.error(); }) {
            // std::expected
            return v ? json_serializer<std::remove_cvref_t<decltype(*v)>>::to_json(*v)
                     : json_serializer<std::remove_cvref_t<decltype(v.error())>>::to_json(v.error());
        } else if constexpr (is_map<T>::value) {
            std::string out = "{";
            bool first = true;
            for (const auto& [k, x] : v) {
                if (!first) out += ",";
                first = false;
                out += json_string(std::string(k)) + ":" +
                       json_serializer<std::remove_cvref_t<decltype(x)>>::to_json(x);
            }
            out += "}";
            return out;
        } else if constexpr (requires { v.begin() != v.end(); v.size(); }) {
            std::string out = "[";
            bool first = true;
            for (const auto& x : v) {
                if (!first) out += ",";
                first = false;
                out += json_serializer<std::remove_cvref_t<decltype(x)>>::to_json(x);
            }
            out += "]";
            return out;
        } else if constexpr (std::is_class_v<T>) {
            return ser_detail::struct_impl(v, std::make_index_sequence<ser_detail::member_count<T>()>());
        } else {
            static_assert(!std::is_same_v<T, T>, "json_serializer: unsupported type");
        }
    }
};

} // namespace mcp
