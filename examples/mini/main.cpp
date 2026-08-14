#include <cstdio>

#include <mcpp/mcpp.hpp>
#include "mini-json.hpp"
#include "tools.hpp"

namespace {

// Writer sink: stdout, newline-delimited. In a real embedded server this would
// be the serial/TCP byte pump.
void write_stdout(void*, std::string_view line) {
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fflush(stdout);
}

} // namespace

int main() {
    // No Server, no simdjson, no threads: drive the Protocol core directly with
    // the mini JSON parser. Async tools run inline (a sequential client, so
    // this is correct).
    mcp::Protocol protocol(mini::registry(), "mcp-demo", "0.1.0");

    mcp::Sink sink;
    sink.write = &write_stdout;

    mcp::StdioStream stream;
    while (true) {
        auto line = stream.read_line();
        if (!line) break;
        if (line->empty()) continue;

        mini::Document doc;
        if (!mini::parse(*line, doc)) {
            // JSON-RPC: reply to unparseable/invalid requests with id: null.
            sink.write(sink.ctx, mcp::error_response_null(mcp::RPC_PARSE_ERROR, "Parse error"));
            continue;
        }
        protocol.dispatch(*line, mini::root(doc), sink);
    }
    return 0;
}
