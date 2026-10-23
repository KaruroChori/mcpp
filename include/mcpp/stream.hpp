#pragma once

/// @file stream.hpp
/// @brief Stream abstraction (stdio and unix sockets).
/// The stream abstraction is POSIX-only: stdio reads via read()/STDIN_FILENO
/// and unix domain sockets. Linux, macOS and the BSDs are fine; anything else
/// fails here with a clear message instead of a chain of missing-header errors.
/// SPDX-License-Identifier: LGPL-3.0
#if !defined(__unix__) && !defined(__APPLE__)
#error "mcpp requires a POSIX platform (stdio + unix sockets); Windows is not a target"
#endif

#include <expected>
#include <string>
#include <string_view>
#include <variant>

#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>

namespace mcp {

/// @brief A reliable, newline-delimited byte stream. Mirrors the MCP stdio transport:
/// one JSON-RPC message per line, no embedded newlines.
///
/// @note `write` must be callable from multiple threads; `read_line` blocks
/// until a line is complete and returns std::unexpected on EOF/error
/// (monostate: no further detail).
class Stream {
public:
    virtual ~Stream() = default;
    virtual std::expected<std::string, std::monostate> read_line() = 0;
    virtual void write(std::string_view data) = 0;
    virtual void close() = 0;
};

class StdioStream final : public Stream {
public:
    std::expected<std::string, std::monostate> read_line() override;
    void write(std::string_view data) override;
    void close() override;

private:
    std::string pending_;
};

/// Linux/BSD send with MSG_NOSIGNAL. macOS has no MSG_NOSIGNAL; it uses
/// SO_NOSIGPIPE on the accepted socket instead.
inline constexpr int kUnixSendFlags =
#if defined(MSG_NOSIGNAL)
    MSG_NOSIGNAL;
#else
    0;
#endif

class UnixSocketStream final : public Stream {
public:
    explicit UnixSocketStream(int fd);
    ~UnixSocketStream() override { close(); }
    std::expected<std::string, std::monostate> read_line() override;
    void write(std::string_view data) override;
    void close() override;

private:
    int fd_;
    std::string pending_;
    bool closed_ = false;
};

/// @brief Binds a listening unix domain socket.
///
/// @param path filesystem path for the socket
/// @return the listener fd (>= 0), or -1 on failure. Paths that are too long
///         for sockaddr_un are rejected with errno ENAMETOOLONG.
/// @see Stream, StdioStream, UnixSocketStream
inline int listen_unix_socket(const std::string& path) {
    /// The path plus its NUL terminator must fit in sun_path; a longer path must
    /// be rejected, not silently truncated (which would bind the wrong socket).
    if (path.size() >= sizeof(sockaddr_un{}.sun_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    ::unlink(path.c_str());

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    const socklen_t len = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path.size() + 1);

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), len) < 0 ||
        ::listen(fd, 4) < 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

// ---------------------------------------------------------------------------
// StdioStream
// ---------------------------------------------------------------------------

inline std::expected<std::string, std::monostate> StdioStream::read_line() {
    /// Read byte-by-byte until '\n' or EOF. Simple and correct; stdio in a
    /// subprocess is line-buffered so this is fine for a demo.
    while (true) {
        auto nl = pending_.find('\n');
        if (nl != std::string::npos) {
            std::string line = pending_.substr(0, nl);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            pending_.erase(0, nl + 1);
            return line;
        }
        char buf[4096];
        const ssize_t n = ::read(STDIN_FILENO, buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR) continue;
            return std::unexpected(std::monostate{});
        }
        if (n == 0) {
            /// Flush any trailing partial line, then signal EOF.
            if (pending_.empty()) return std::unexpected(std::monostate{});
            std::string line = std::move(pending_);
            pending_.clear();
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return line;
        }
        pending_.append(buf, static_cast<size_t>(n));
    }
}

inline void StdioStream::write(std::string_view data) {
    std::fwrite(data.data(), 1, data.size(), stdout);
    std::fflush(stdout);
}

inline void StdioStream::close() {
    std::fflush(stdout);
}

// ---------------------------------------------------------------------------
// UnixSocketStream
// ---------------------------------------------------------------------------

inline UnixSocketStream::UnixSocketStream(int fd) : fd_(fd) {
#if defined(SO_NOSIGPIPE)
    int one = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
}

inline std::expected<std::string, std::monostate> UnixSocketStream::read_line() {
    while (true) {
        auto nl = pending_.find('\n');
        if (nl != std::string::npos) {
            std::string line = pending_.substr(0, nl);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            pending_.erase(0, nl + 1);
            return line;
        }
        char buf[4096];
        const ssize_t n = ::recv(fd_, buf, sizeof buf, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return std::unexpected(std::monostate{});
        }
        if (n == 0) {
            if (pending_.empty()) return std::unexpected(std::monostate{});
            std::string line = std::move(pending_);
            pending_.clear();
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return line;
        }
        pending_.append(buf, static_cast<size_t>(n));
    }
}

inline void UnixSocketStream::write(std::string_view data) {
    if (closed_) return;
    size_t off = 0;
    while (off < data.size()) {
        const ssize_t n = ::send(fd_, data.data() + off, data.size() - off, kUnixSendFlags);
        if (n < 0) {
            if (errno == EINTR) continue;
            closed_ = true;
            return;
        }
        off += static_cast<size_t>(n);
    }
}

inline void UnixSocketStream::close() {
    if (!closed_) {
        closed_ = true;
        ::close(fd_);
    }
}

} // namespace mcp
