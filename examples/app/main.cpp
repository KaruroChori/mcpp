#include <unistd.h>
#include <sys/socket.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

#include <fmt/format.h>

#include <mcpp-server/mcpp-server.hpp>
#include "tools.hpp"

namespace {

void usage() {
    fmt::println(stderr, "mcp-demo-cpp-refl 0.1.0 (c++26 reflection, -fno-exceptions -fno-rtti)");
    fmt::println(stderr, "");
    fmt::println(stderr, "Usage:");
    fmt::println(stderr, "  mcp-demo-cpp-refl --stdio");
    fmt::println(stderr, "  mcp-demo-cpp-refl --uds /path/to.sock [--auth]");
    fmt::println(stderr, "");
    fmt::println(stderr, "  Default: --stdio. With --uds, each connection is a");
    fmt::println(stderr, "  session. Add --auth to enable the tolerant first-line");
    fmt::println(stderr, "  credential handshake ({{\"auth\":\"admin\"}}).");
    std::exit(2);
}

int run_stdio() {
    mcp::StdioStream stream;
    // stdio is a single session with default policies (no handshake).
    mcp::Server server(stream, app::registry());
    server.run();
    return 0;
}

int run_unix_socket(const std::string& path, bool auth) {
    const int listener = mcp::listen_unix_socket(path);
    if (listener < 0) {
        fmt::println(stderr, "[mcp-demo] failed to bind unix socket {}: {}", path, std::strerror(errno));
        return 1;
    }
    fmt::println(stderr, "[mcp-demo] listening on unix socket {} (auth: {})", path, auth ? "on" : "off");

    // Default session context: a basic (non-admin) slot. `--auth` additionally
    // installs the tolerant handshake so a connection can mint an admin slot.
    static constexpr app::AuthContext basic_ctx{ .admin = false };
    const void* auth_ctx = &basic_ctx;
    mcp::authenticator_fn authenticate = nullptr;
    if (auth) {
        authenticate = &app::authenticate;
    }

    mcp::SocketHost host(listener, app::registry(), "mcp-demo", "0.1.0", auth_ctx, authenticate);
    host.run();
    ::close(listener);
    ::unlink(path.c_str());
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::string_view uds;
    bool auth = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--uds" && i + 1 < argc) {
            uds = argv[++i];
        } else if (std::string_view(argv[i]) == "--auth") {
            auth = true;
        } else if (std::string_view(argv[i]) != "--stdio") {
            usage();
        }
    }
    if (!uds.empty()) {
        return run_unix_socket(std::string(uds), auth);
    }
    return run_stdio();
}
