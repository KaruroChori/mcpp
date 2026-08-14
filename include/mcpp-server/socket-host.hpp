#pragma once

/// @file socket-host.hpp
/// @brief Multi-session unix-socket host.
/// SPDX-License-Identifier: LGPL-3.0

#include <atomic>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <mcpp/action.hpp>
#include <mcpp/stream.hpp>
#include "server.hpp"

namespace mcp {

/// @brief A concrete multi-session host for the Server.
///
/// Owns a listening unix socket and serves each accepted connection on its own
/// thread with its own Server. This is the environment-specific layer for "an
/// already-running application offering MCP server slots": each connection
/// becomes an independent MCP session, sharing the one constexpr Registry but
/// carrying its own auth context.
///
/// The listener is opened by the caller (listen_unix_socket); SocketHost only
/// accepts and threads. `auth_ctx` is the default session context for a
/// connection (the app's "default policies"); `authenticate` optionally parses
/// a first-line credential handshake to mint a per-connection context instead.
///
/// @see the auth notes in action.hpp and Server.
class SocketHost {
public:
    /// @brief Creates the host over an already-listening unix socket.
    ///
    /// @param listener_fd       The caller-owned listening socket fd.
    /// @param registry          The shared Registry used by every session.
    /// @param name              Server name reported to clients.
    /// @param version           Server version reported to clients.
    /// @param auth_ctx          Default session auth context; see the class notes.
    /// @param authenticate      Optional first-line credential handshake parser.
    /// @param session_pool_size Trailing arg forwarded to each session Server's
    ///                          worker pool (0 selects the default size).
    SocketHost(int listener_fd, const Registry& registry,
               std::string_view name = "mcp-demo", std::string_view version = "0.1.0",
               const void* auth_ctx = nullptr, authenticator_fn authenticate = nullptr,
               std::size_t session_pool_size = 0);
    ~SocketHost();

    SocketHost(const SocketHost&) = delete;
    SocketHost& operator=(const SocketHost&) = delete;

    /// @brief Accept loop; blocks until the listener fails or is closed.
    void run();

private:
    /// @brief One per accepted connection.
    ///
    /// The session lambda captures everything by value (no `this`), so the
    /// thread runs independently of the owning host. The accept loop reaps
    /// finished sessions so the session vector stays bounded by active
    /// connections instead of growing forever.
    struct session {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
        ///< Set by the session thread when its Server exits, signalling the
        ///< accept loop to reap this session.
    };

    int listener_;
    Registry registry_;
    std::string name_;
    std::string version_;
    const void* auth_ctx_;
    authenticator_fn authenticate_;
    std::size_t session_pool_size_ = 0;
    std::vector<session> sessions_;
};

} // namespace mcp
