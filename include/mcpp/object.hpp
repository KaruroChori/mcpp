#pragma once

/// @file object.hpp
/// @brief Object-bound tools: tokens, registry, and RAII handles.
/// SPDX-License-Identifier: LGPL-3.0

#include <cstdint>
#include <mutex>
#include <random>
#include <unordered_map>

#include "read-arg.hpp"

namespace mcp {

/// Opaque handle identifying a registered object instance. A non-static member
/// function of a tool group carries an implicit `this`; over MCP that becomes an
/// explicit token the client sends back with each call. Tokens are random (not
/// sequential), so a client cannot guess or enumerate another object's handle.
/// Access is granted and revoked through the token: `object_registry` maps a
/// token to a live object, and `revoke` makes every later call fail.
/// @see object_registry, object_handle
using object_token = std::uint64_t;

/// Read the injected `handle` argument from a request's tool arguments. Tokens
/// are unsigned 64-bit; accept both a JSON integer (int64 or uint64 form).
/// @param args The request's tool arguments.
/// @param name The argument name to read.
/// @return The resolved token, or 0 when the argument is absent or not an integer.
template <>
inline object_token read_arg<object_token>(const JsonView& args, std::string_view name) {
    uint64_t u = 0;
    if (args[name].get_uint64(u)) return u;
    int64_t v = 0;
    if (args[name].get_int64(v)) return static_cast<uint64_t>(v);
    return 0;
}

/// Registry of live object instances, keyed by opaque token. One registry per
/// `Dev` type (a function-local static), guarded by its own mutex so the
/// multi-session server can resolve handles concurrently.
/// @tparam Dev The device type whose instances are registered.
template <typename Dev>
class object_registry {
public:
    /// Register an object, returning its fresh opaque token.
    /// @param obj The object to register.
    /// @return The newly issued token, never 0 and kept within 2^53.
    static object_token issue(Dev& obj) {
        std::lock_guard<std::mutex> lk(mutex());
        const object_token t = next_token();
        table()[t] = &obj;
        return t;
    }

    /// Resolve a token to its object, or null when unknown or revoked.
    /// @param token The token to look up.
    /// @return Pointer to the registered object, or nullptr when unknown or revoked.
    static Dev* resolve(object_token token) {
        std::lock_guard<std::mutex> lk(mutex());
        auto it = table().find(token);
        return it == table().end() ? nullptr : it->second;
    }

    /// Revoke a token; subsequent resolves return null.
    /// @param token The token to revoke.
    static void revoke(object_token token) {
        std::lock_guard<std::mutex> lk(mutex());
        table().erase(token);
    }

private:
    static std::unordered_map<object_token, Dev*>& table() {
        static std::unordered_map<object_token, Dev*> t;
        return t;
    }

    static std::mutex& mutex() {
        static std::mutex m;
        return m;
    }

    static object_token next_token() {
        static std::mt19937_64 rng{std::random_device{}()};
        /// Keep tokens within the JSON-safe integer range (2^53): a JS-based MCP
        /// client (opencode, Claude Desktop) represents JSON numbers as doubles,
        /// which round values above 2^53. The client would send back a slightly
        /// different token and resolution would fail. 2^53 still leaves plenty of
        /// unpredictable, unforgeable space.
        ///
        /// Token 0 is reserved as the "missing handle" convention (read_arg
        /// yields 0 for an absent handle argument), so it must never be issued:
        /// a real token 0 would make missing-handle requests silently resolve to
        /// its object. Skip it.
        object_token t;
        do {
            t = rng() & ((1ULL << 53) - 1);
        } while (t == 0);
        return t;
    }
};

/// RAII guard for a registered object: issues a token on construction and
/// revokes it on destruction, so a stack object is automatically revoked when it
/// goes out of scope. Move-only; a moved-from handle revokes nothing.
/// @tparam Dev The device type whose instance is guarded.
template <typename Dev>
class object_handle {
public:
    explicit object_handle(Dev& obj) : obj_(&obj), token_(object_registry<Dev>::issue(obj)) {}

    ~object_handle() {
        if (obj_) object_registry<Dev>::revoke(token_);
    }

    object_handle(const object_handle&) = delete;
    object_handle& operator=(const object_handle&) = delete;

    object_handle(object_handle&& o) noexcept : obj_(o.obj_), token_(o.token_) { o.obj_ = nullptr; }
    object_handle& operator=(object_handle&& o) noexcept {
        if (this != &o) {
            if (obj_) object_registry<Dev>::revoke(token_);
            obj_ = o.obj_;
            token_ = o.token_;
            o.obj_ = nullptr;
        }
        return *this;
    }

    object_token token() const { return token_; }
    Dev* get() const { return obj_; }

private:
    Dev* obj_ = nullptr;        ///< Guarded object, or nullptr when moved-from.
    object_token token_ = 0;    ///< Issued token for the guarded object.
};

} // namespace mcp
