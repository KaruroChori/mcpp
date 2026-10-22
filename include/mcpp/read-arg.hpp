#pragma once

/// @file read-arg.hpp
/// @brief Runtime JSON-to-C++ argument marshalling.
/// Runtime JSON -> C++ argument reading (the marshalling layer).
///
/// Reads reflected tool parameters out of a `JsonView` of the request's
/// `params.arguments`. This is the parser-free seam: the view is produced by a
/// backend `Parser` (simdjson, the mini parser, ...) and `read_arg` only ever
/// touches JsonView. The reflection invoker in meta.hpp is the only caller.
///
/// Supported parameter types: any integral (signed/unsigned, incl. char) and
/// floating point scalar, bool, std::string (copied, `std::string_view` and
/// `const char*` are deliberately NOT supported: the view would point into the
/// per-call parser document, which is destroyed when the tool returns, and a
/// tool that retains it would dangle), enums (marshal as their enumerator name
/// on the wire), `std::optional<T>` (absent arg -> nullopt, and the schema does
/// not mark it required), `std::vector<T>` (a JSON array), and literal structs
/// (a JSON object, read member-by-member via reflection).
///
/// `read_arg<T>(args, name)` reads a named argument; `read_value<T>(view)` reads
/// a value from a view directly (used recursively for array elements and struct
/// members).
/// SPDX-License-Identifier: LGPL-3.0

#include <array>
#include <cstdint>
#include <map>
#include <meta>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "json-view.hpp"

namespace mcp {

/// `is_optional` is defined in serializer.hpp (shared with the write side);
/// forward-declared here so `read_value` can use it (resolved at instantiation).
template <typename T>
struct is_optional;

template <typename T>
struct is_vector : std::false_type {};
template <typename U, typename A>
struct is_vector<std::vector<U, A>> : std::true_type {};

template <typename T>
using read_arg_type = std::remove_cv_t<std::remove_reference_t<T>>;

template <typename T>
read_arg_type<T> read_value(const JsonView& v);

namespace arg_detail {

/// Enum -> name/value table built by reflection at compile time. The enumerator
/// values are spliced (`[: es[Is] :]`), not `meta::extract`ed: gcc's extract
/// cannot yield an enumerator's value.
template <typename T>
consteval std::size_t enumerator_count() {
    return std::meta::enumerators_of(^^T).size();
}

template <typename T, std::size_t... Is>
consteval std::array<std::pair<std::string_view, T>, sizeof...(Is)>
enum_table_impl(std::index_sequence<Is...>) {
    static constexpr std::array<std::meta::info, sizeof...(Is)> es = [] {
        const auto v = std::meta::enumerators_of(^^T);
        std::array<std::meta::info, sizeof...(Is)> arr{};
        for (std::size_t i = 0; i < v.size(); ++i) arr[i] = v[i];
        return arr;
    }();
    return { std::pair<std::string_view, T>{ std::meta::identifier_of(es[Is]), [: es[Is] :] }... };
}

template <typename T>
consteval auto enum_table() {
    return enum_table_impl<T>(std::make_index_sequence<enumerator_count<T>()>{});
}

/// Literal struct -> member infos/names via reflection.
template <typename T>
consteval std::size_t struct_member_count() {
    return std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::current()).size();
}

template <typename T>
consteval std::array<std::meta::info, struct_member_count<T>()> struct_member_infos() {
    const auto ms = std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::current());
    std::array<std::meta::info, struct_member_count<T>()> out{};
    for (std::size_t i = 0; i < ms.size(); ++i) out[i] = ms[i];
    return out;
}

template <typename T>
consteval std::array<std::string_view, struct_member_count<T>()> struct_member_names() {
    const auto ms = std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::current());
    std::array<std::string_view, struct_member_count<T>()> out{};
    for (std::size_t i = 0; i < ms.size(); ++i) out[i] = std::meta::identifier_of(ms[i]);
    return out;
}

template <typename T, std::size_t... Is>
T read_struct(const JsonView& obj, std::index_sequence<Is...>) {
    static constexpr auto infos = struct_member_infos<T>();
    static constexpr auto names = struct_member_names<T>();
    T value{};
    (([&] {
        auto&& m = value.[: infos[Is] :];
        using MT = std::remove_cvref_t<decltype(m)>;
        m = read_value<MT>(obj[names[Is]]);
    }()), ...);
    return value;
}

} // namespace arg_detail

template <typename T>
read_arg_type<T> read_value(const JsonView& v) {
    using U = read_arg_type<T>;
    if constexpr (std::is_same_v<U, std::string>) {
        std::string_view s;
        if (v.get_string(s)) return std::string(s);
        return {};
    } else if constexpr (std::is_same_v<U, bool>) {
        bool b = false;
        v.get_bool(b);
        return b;
    } else if constexpr (std::is_integral_v<U>) {
        if constexpr (std::is_unsigned_v<U>) {
            std::uint64_t u = 0;
            if (v.get_uint64(u)) return static_cast<U>(u);
            std::int64_t i = 0;
            if (v.get_int64(i)) return static_cast<U>(i);
            return static_cast<U>(0);
        } else {
            std::int64_t x = 0;
            v.get_int64(x);
            return static_cast<U>(x);
        }
    } else if constexpr (std::is_floating_point_v<U>) {
        double d = 0;
        v.get_double(d);
        return static_cast<U>(d);
    } else if constexpr (std::is_enum_v<U>) {
        std::string_view s;
        if (v.get_string(s)) {
            constexpr auto table = arg_detail::enum_table<U>();
            for (const auto& [ename, val] : table) {
                if (ename == s) return val;
            }
        }
        return static_cast<U>(0);
    } else if constexpr (is_optional<U>::value) {
        using V = typename U::value_type;
        if (!v.present()) return std::nullopt;
        return U(read_value<V>(v));
    } else if constexpr (is_vector<U>::value) {
        using V = typename U::value_type;
        U out;
        const std::size_t n = v.array_size();
        out.reserve(n);
        for (std::size_t i = 0; i < n; ++i) out.push_back(read_value<V>(v.array_at(i)));
        return out;
    } else if constexpr (std::is_class_v<U>) {
        return arg_detail::read_struct<U>(
            v, std::make_index_sequence<arg_detail::struct_member_count<U>()>());
    } else {
        static_assert(!std::is_same_v<U, U>, "read_arg: unsupported parameter type");
    }
}

template <typename T>
read_arg_type<T> read_arg(const JsonView& args, std::string_view name) {
    return read_value<T>(args[name]);
}

} // namespace mcp
