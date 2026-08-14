// stdio <-> Unix domain socket bridge with slot authentication.
//
// Both ends carry newline-delimited JSON-RPC frames, so after the handshake
// this is a pure byte relay with zero parsing. It lets clients that only speak
// stdio (opencode, Claude Desktop) request an MCP session from an
// already-running application that hosts MCP server slots on unix sockets:
//
//   client --stdio--> bridge --UDS--> running app
//
// The bridge picks the socket for the requested permission slot and injects
// the credential as the first line; the app's tolerant handshake consumes it
// (or falls back to default policies if it's absent).
//
// Slot selection:
//   - path:  mcpp-bridge /path/to.sock [slot]    explicit socket path
//   - env:   MCP_SLOT=admin mcpp-bridge          derive /tmp/mcp-demo-admin.sock
//
// If a slot name is provided (explicit arg, else $MCP_SLOT) the bridge sends
// {"auth":"<slot>"} as its first line. With no slot, no handshake is sent and
// the connection gets the app's default policies.

#include <unistd.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>

namespace {

int connect_unix_socket(const char* path) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

bool write_all(int fd, const char* data, std::size_t n) {
    while (n > 0) {
        const ssize_t w = ::write(fd, data, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        data += w;
        n -= static_cast<std::size_t>(w);
    }
    return true;
}

// Blocking relay loop: stdin -> fd and fd -> stdout, polled together so both
// directions flow concurrently. Pure byte relay after the handshake — no
// parsing. On stdin EOF, half-close the write side so the app sees EOF too,
// but keep draining the socket until it closes.
void relay(int fd) {
    char buf[8192];
    bool stdin_open = true;
    while (true) {
        pollfd pfds[2];
        int nfds = 0;
        if (stdin_open) {
            pfds[nfds++] = {STDIN_FILENO, POLLIN, 0};
        }
        pfds[nfds++] = {fd, POLLIN, 0};
        if (::poll(pfds, nfds, -1) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (int i = 0; i < nfds; ++i) {
            const int src = pfds[i].fd;
            if (!(pfds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            const ssize_t n = ::read(src, buf, sizeof buf);
            if (n < 0) {
                if (errno == EINTR) continue;
                return;
            }
            if (n == 0) {
                if (src == STDIN_FILENO) {
                    stdin_open = false;
                    if (::shutdown(fd, SHUT_WR) < 0) return;
                } else {
                    return;  // app closed the session
                }
                continue;
            }
            const int dst = src == STDIN_FILENO ? fd : STDOUT_FILENO;
            if (!write_all(dst, buf, static_cast<std::size_t>(n))) return;
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    const char* base_dir = std::getenv("MCP_SOCKET_DIR");
    const char* prefix = std::getenv("MCP_SOCKET_PREFIX");
    if (!base_dir) base_dir = "/tmp";
    if (!prefix) prefix = "mcp-demo";

    const char* slot = (argc >= 3) ? argv[2] : std::getenv("MCP_SLOT");
    std::string path;
    if (argc >= 2) {
        path = argv[1];
    } else if (slot) {
        path = std::string(base_dir) + "/" + prefix + "-" + slot + ".sock";
    } else {
        std::fprintf(stderr, "usage: mcpp-bridge [unix-socket-path] [slot]  (or set MCP_SLOT)\n");
        return 2;
    }

    const int fd = connect_unix_socket(path.c_str());
    if (fd < 0) {
        std::fprintf(stderr, "[bridge] connect %s: %s\n", path.c_str(), std::strerror(errno));
        return 1;
    }
    std::fprintf(stderr, "[bridge] connected to %s%s\n", path.c_str(), slot ? " (slot)" : "");

    if (slot) {
        const std::string auth = std::string("{\"auth\":\"") + slot + "\"}\n";
        if (!write_all(fd, auth.data(), auth.size())) {
            std::fprintf(stderr, "[bridge] handshake write failed\n");
            return 1;
        }
    }

    relay(fd);
    ::close(fd);
    return 0;
}
