#include <mcpp-server/mcpp-server.hpp>
#include "tools.hpp"

int main() {
    mcp::StdioStream stream;
    mcp::Server server(stream, hello::registry());
    server.run();
    return 0;
}
