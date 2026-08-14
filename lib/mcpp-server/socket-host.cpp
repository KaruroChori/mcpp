/// @file socket-host.cpp
/// @brief SocketHost implementation (session reaping).
/// SPDX-License-Identifier: LGPL-3.0
#include <mcpp-server/socket-host.hpp>

#include <unistd.h>
#include <sys/socket.h>

#include <cerrno>

namespace mcp {

SocketHost::SocketHost(int listener_fd, const Registry& registry, std::string_view name,
                       std::string_view version, const void* auth_ctx, authenticator_fn authenticate,
                       std::size_t session_pool_size)
    : listener_(listener_fd), registry_(registry), name_(name), version_(version),
      auth_ctx_(auth_ctx), authenticate_(authenticate), session_pool_size_(session_pool_size) {}

SocketHost::~SocketHost() {
    /// Detaches any sessions still running when the host goes away. They own
    /// their Server, and the session lambda captures everything by value, so
    /// `this` is never touched after the thread starts.
    for (auto& s : sessions_) {
        if (s.thread.joinable()) s.thread.detach();
    }
}

void SocketHost::run() {
    while (true) {
        /// Reap finished sessions so `sessions_` stays bounded by the number of
        /// active connections rather than growing for the life of the host.
        for (auto it = sessions_.begin(); it != sessions_.end();) {
            if (it->done->load()) {
                it->thread.join();
                it = sessions_.erase(it);
            } else {
                ++it;
            }
        }

        const int conn = ::accept(listener_, nullptr, nullptr);
        if (conn < 0) {
            if (errno == EINTR) continue;
            break;
        }

        /// One thread per connection: an independent MCP session sharing the
        /// registry but with its own stream and auth context. Everything the
        /// session needs is captured by value (no `this`), and `done` lets the
        /// accept loop reap the finished session.
        auto done = std::make_shared<std::atomic<bool>>(false);
        sessions_.emplace_back(std::thread(
            [conn, registry = registry_, name = name_, version = version_,
             auth_ctx = auth_ctx_, authenticate = authenticate_,
             pool = session_pool_size_, done] {
                UnixSocketStream stream(conn);
                Server server(stream, registry, name, version, auth_ctx, authenticate, pool);
                server.run();
                done->store(true);
            }), done);
    }
}

} // namespace mcp
