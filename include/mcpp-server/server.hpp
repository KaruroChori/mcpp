#pragma once

/// @file server.hpp
/// @brief Threaded MCP server over the parser-independent protocol core.
/// SPDX-License-Identifier: LGPL-3.0

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <simdjson.h>

#include <mcpp/action.hpp>
#include <mcpp/json.hpp>
#include <mcpp/json-view.hpp>
#include <mcpp/protocol.hpp>
#include <mcpp/stream.hpp>
#include "simdjson-ops.hpp"

namespace mcp {

/// @brief Concrete MCP server over the parser-independent Protocol core.
///
/// This class owns what the core deliberately does not: a simdjson parser, a
/// bounded async worker pool for tools, a cancellation table, and the write
/// mutex. Everything protocol-shaped lives in Protocol; everything
/// host/thread/parse-shaped lives here.
///
/// The async worker pool is bounded rather than a thread per call; it defaults
/// to hardware_concurrency and is configurable via the trailing
/// `async_pool_size` constructor argument.
///
/// Servers that want none of this (no threads, a different parser, a
/// cooperative pump) can drive `Protocol` directly: see examples/mini.
///
/// Optional authorization: `auth_ctx` is the session's opaque context handed to
/// each capability's `authorized` check. `authenticate` (when supplied) allows
/// a tolerant first-line handshake: if the very first line looks like the
/// project's credential (e.g. `{"auth":"admin"}`), it is consumed and its
/// context used; otherwise the line is treated as the first JSON-RPC message
/// and default policies apply. See the auth notes in action.hpp.
class Server {
public:
    /// @brief Constructs the server over a stream and the protocol registry.
    /// @param stream The transport stream requests are read from and responses
    /// are written to.
    /// @param registry The capability registry this server exposes.
    /// @param name Server name reported to clients.
    /// @param version Server version reported to clients.
    /// @param auth_ctx Opaque session context handed to each capability's
    /// `authorized` check.
    /// @param authenticate Optional credential matcher for the tolerant
    /// first-line handshake; when null the first line is treated as the first
    /// JSON-RPC message.
    /// @param async_pool_size Bound for the async tool worker pool; zero (the
    /// default) selects hardware_concurrency.
    explicit Server(Stream& stream, const Registry& registry,
                    std::string_view name = "mcp-demo", std::string_view version = "0.1.0",
                    const void* auth_ctx = nullptr, authenticator_fn authenticate = nullptr,
                    std::size_t async_pool_size = 0);
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    /// @brief Runs until the stream hits EOF.
    void run();

private:
    void on_async_call(AsyncCall call);
    void cancel_request(const RpcId& id);
    void process_line(std::string_view line);

    void write_line(std::string_view full_line);
    void signal_and_join();
    void worker_loop();

    /// The transport stream requests are read from and responses written to.
    Stream& stream_;
    /// The parser-independent protocol core.
    Protocol protocol_;
    /// Credential matcher for the tolerant first-line handshake; null disables it.
    authenticator_fn authenticate_;
    /// Owned simdjson parser.
    simdjson::dom::parser parser_;
    /// Serializes responses and notifications on the write side.
    std::mutex write_mutex_;

    /// Guards `cancel_flags_`.
    std::mutex cancel_mutex_;
    /// Keyed by the canonical id string (numeric ids as decimals, string ids as
    /// text) so a `notifications/cancelled` lookup matches the registration.
    std::map<std::string, std::shared_ptr<std::atomic<bool>>> cancel_flags_;

    /// Async tool worker pool: a fixed set of worker threads draining a bounded
    /// queue. `on_async_call` enqueues (blocking on the bound as backpressure);
    /// workers run one tool at a time. The bound defaults to
    /// hardware_concurrency and is set via the trailing `async_pool_size`
    /// constructor argument. Lock order: pool_mutex_ -> cancel_mutex_.
    std::mutex pool_mutex_;
    /// Signals idle workers when work is queued or shutdown is requested.
    std::condition_variable pool_cv_;
    /// Pending async tool invocations awaiting a worker.
    std::deque<std::function<void()>> queue_;
    /// The fixed set of worker threads.
    std::vector<std::thread> workers_;
    /// True once shutdown has been requested.
    bool stopping_ = false;
    /// The configured pool bound.
    std::size_t pool_size_ = 0;
};

} // namespace mcp
