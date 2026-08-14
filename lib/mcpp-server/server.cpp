/// @file server.cpp
/// @brief Threaded MCP server implementation (worker pool, cancellation).
/// SPDX-License-Identifier: LGPL-3.0

#include <mcpp-server/server.hpp>

#include <cstdio>
#include <string_view>

#include <fmt/format.h>

namespace mcp {

namespace {

/// @brief Canonical cancel-table key for a request id.
/// @param id The request id to key.
/// @return String ids as text, numeric ids as decimal.
std::string id_key(const RpcId& id) {
    return id.is_string ? id.text : std::to_string(id.number);
}

/// @brief Default async pool size.
/// @return Hardware concurrency, falling back to a small pool of two.
std::size_t default_pool_size() {
    const unsigned hw = std::thread::hardware_concurrency();
    return hw ? hw : 2;
}

} // namespace

Server::Server(Stream& stream, const Registry& registry, std::string_view name, std::string_view version,
               const void* auth_ctx, authenticator_fn authenticate, std::size_t async_pool_size)
    : stream_(stream), protocol_(registry, name, version, auth_ctx), authenticate_(authenticate),
      pool_size_(async_pool_size ? async_pool_size : default_pool_size()) {
    protocol_.set_async_executor(AsyncExecutor{
        [](void* ctx, AsyncCall call) { static_cast<Server*>(ctx)->on_async_call(call); },
        [](void* ctx, const RpcId& id) { static_cast<Server*>(ctx)->cancel_request(id); },
        this });
    // The worker pool is a fixed set of threads draining the task queue.
    workers_.reserve(pool_size_);
    for (std::size_t i = 0; i < pool_size_; ++i) {
        workers_.emplace_back([this] { worker_loop(); });
    }
}

Server::~Server() {
    signal_and_join();
}

void Server::run() {
    // Tolerant first-line handshake: if an authenticator is installed and the
    // very first line is a recognized credential (e.g. {"auth":"admin"}), it is
    // consumed and its context applied to the session. Otherwise the line is
    // processed as a normal JSON-RPC message with default policies.
    {
        auto first = stream_.read_line();
        if (!first) {
            signal_and_join();
            return;
        }
        if (first->empty()) {
            signal_and_join();
            return;
        }
        if (authenticate_) {
            if (auto r = authenticate_(*first); r.consumed) {
                protocol_.set_auth_ctx(r.ctx);
            } else {
                process_line(*first);
            }
        } else {
            process_line(*first);
        }
    }

    while (true) {
        auto line = stream_.read_line();
        if (!line) break;
        if (line->empty()) continue;
        process_line(*line);
    }
    signal_and_join();
}

void Server::process_line(std::string_view line) {
    // Parse into this message's document; root points into it and stays
    // alive for the whole dispatch. The simdjson parser backend owns the
    // document + child arena, which outlive the dispatch.
    simdjson_parser::document doc;
    JsonView root;
    // Reuse the server's owned parser so the parse buffer is not reallocated
    // per request (the run loop is single-threaded, so parser_ is safe).
    if (!simdjson_parser::parse(line, doc, root, &parser_)) {
        fmt::println(stderr, "[mcp-demo] parse error on: {:.80}", line);
        // JSON-RPC: reply to unparseable/invalid requests with id: null.
        write_line(error_response_null(RPC_PARSE_ERROR, "Parse error"));
        return;
    }

    Sink sink;
    sink.write = [](void* ctx, std::string_view out) { static_cast<Server*>(ctx)->write_line(out); };
    sink.ctx = this;
    protocol_.dispatch(line, root, sink);
}

void Server::on_async_call(AsyncCall call) {
    // Enqueue the tool call on the worker pool. The tool parses its own copy of
    // the line, which must be an owned string: the view handed to us borrows
    // the reader loop's buffer, which the worker outlives.
    const std::string raw_line(call.raw_line);
    const tool_call_fn tool_call = call.call;
    const RpcId response_id = call.id;
    std::unique_lock tl(pool_mutex_);
    // Bounded queue: wait for a slot (backpressure) unless shutting down.
    pool_cv_.wait(tl, [this] { return stopping_ || queue_.size() < pool_size_; });
    // Shutting down: don't start the work, and don't register a cancel flag we
    // would never erase (the worker erases it on completion). The check and the
    // enqueue share the pool mutex, so a task admitted here is guaranteed to run.
    if (stopping_) return;
    auto cancel_flag = std::make_shared<std::atomic<bool>>(false);
    {
        std::lock_guard ck(cancel_mutex_);
        cancel_flags_[id_key(call.id)] = cancel_flag;
    }
    queue_.emplace_back([this, tool_call, raw_line, response_id, cancel_flag,
                         progress_token = call.progress_token,
                         has_progress_token = call.has_progress_token] {
        ActionContext ctx;
        ctx.cancelled = cancel_flag.get();
        ctx.has_progress_token = has_progress_token;
        ctx.progress_token = progress_token;
        ctx.send = [this](std::string_view full_line) { write_line(full_line); };
        const ToolResult result = tool_call(raw_line, ctx);
        if (result.is_error) {
            const std::string full = response_id.is_string
                ? error_response(response_id.text, result.error_code, result.error_message)
                : error_response(response_id.number, result.error_code, result.error_message);
            write_line(full);
        } else {
            write_line(protocol_.wrap_tool_result(response_id, result.text));
        }
        {
            std::lock_guard ck(cancel_mutex_);
            cancel_flags_.erase(id_key(response_id));
        }
    });
    pool_cv_.notify_one();
}

void Server::cancel_request(const RpcId& id) {
    const std::string key = id_key(id);
    std::lock_guard ck(cancel_mutex_);
    auto it = cancel_flags_.find(key);
    if (it != cancel_flags_.end()) it->second->store(true);
}

void Server::write_line(std::string_view full_line) {
    std::lock_guard lk(write_mutex_);
    if (!stopping_) stream_.write(full_line);
}

void Server::worker_loop() {
    std::function<void()> task;
    while (true) {
        std::unique_lock lk(pool_mutex_);
        pool_cv_.wait(lk, [this] { return stopping_ || !queue_.empty(); });
        if (stopping_ && queue_.empty()) return;
        // Drain any queued work on shutdown (tools observe the cancel flags set
        // below and produce no output, since write_line no-ops when stopping).
        task = std::move(queue_.front());
        queue_.pop_front();
        pool_cv_.notify_one();   // a slot freed for a blocked enqueuer
        lk.unlock();
        task();
    }
}

void Server::signal_and_join() {
    {
        std::lock_guard lk(pool_mutex_);
        stopping_ = true;
    }
    {
        std::lock_guard ck(cancel_mutex_);
        for (auto& [_, flag] : cancel_flags_) flag->store(true);
    }
    pool_cv_.notify_all();
    for (auto& t : workers_) {
        if (t.joinable()) t.join();
    }
}

} // namespace mcp
