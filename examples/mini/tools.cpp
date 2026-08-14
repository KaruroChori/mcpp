#include "tools.hpp"

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include <mcpp/mcpp.hpp>
#include "mini-json.hpp"

namespace mini {

using std::this_thread::sleep_for;
using namespace std::chrono_literals;

namespace {

std::string run_cmd(const std::string& cmd) {
    std::string out;
    if (FILE* p = popen(cmd.c_str(), "r")) {
        char buf[512];
        while (std::fgets(buf, sizeof buf, p)) out += buf;
        pclose(p);
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}

std::string git(const std::string& args) {
    return run_cmd("git " + args + " 2>/dev/null");
}

} // namespace

// ---------------------------------------------------------------------------
// Result types. Tools return natural values; the reflection machinery
// serializes them (serializer.hpp) and reads arguments through mini::parser —
// so reflection works here with zero simdjson, the whole point of this example.
// ---------------------------------------------------------------------------

struct project_info_result {
    std::string branch;
    std::string head;
    std::string subject;
    bool dirty;
    std::int64_t changedFiles;
};
struct complete_result { std::string status; std::int64_t seconds; std::int64_t total; };
struct cancelled_result { std::string status; std::int64_t progress; std::int64_t total; };

// ---------------------------------------------------------------------------
// Tool group (reflected, using the mini parser backend).
// ---------------------------------------------------------------------------

struct [[= mcp::tool_group{} ]] device {
    [[= mcp::tool{ .description = mcp::str("Returns the message back verbatim. Use for connectivity sanity checks.") } ]]
    static auto echo(std::string message) {
        return (struct{ std::string echoed; }){ std::move(message) };
    }

    [[= mcp::tool{ .description = mcp::str("Adds two integers.") } ]]
    static auto add(std::int64_t a, std::int64_t b) {
        return (struct{ std::int64_t sum; }){ a + b };
    }

    [[= mcp::tool{ .description = mcp::str("Reports the current git branch, HEAD commit, and dirty status of the repo containing the server's working directory.") } ]]
    static project_info_result project_info() {
        const std::string branch = git("rev-parse --abbrev-ref HEAD");
        const std::string head = git("rev-parse --short HEAD");
        const std::string subject = git("log -1 --oneline");
        const std::string status = git("status --short");
        return project_info_result{
            branch,
            head,
            subject,
            !status.empty(),
            status.empty() ? 0 : static_cast<int64_t>(std::count(status.begin(), status.end(), '\n')) + 1,
        };
    }

    [[= mcp::tool{ .description = mcp::str("Simulates a long-running operation, emitting progress notifications. Demonstrates async tool execution, progress reporting, and cancellation."), .async = true } ]]
    static std::variant<complete_result, cancelled_result> slow_task(std::int64_t seconds, mcp::ActionContext& ctx) {
        seconds = std::clamp<int64_t>(seconds, 1, 30);
        const int64_t total = seconds * 10;
        for (int64_t s = 0; s <= total; s += 5) {
            if (ctx.cancelled && ctx.cancelled->load()) {
                return cancelled_result{ "cancelled", s, total };
            }
            if (ctx.has_progress_token) {
                const std::string params = mcp::json_object({
                    {"progressToken", std::to_string(ctx.progress_token)},
                    {"progress", std::to_string(s)},
                    {"total", std::to_string(total)},
                });
                ctx.send(mcp::notification("notifications/progress", params));
            }
            sleep_for(100ms);
        }
        return complete_result{ "complete", seconds, total };
    }
};

// Prompt: reflected member function returning the standard `messages` array.
struct [[= mcp::prompt_group{} ]] prompt_group {
    [[= mcp::tool{ .description = mcp::str("Asks the assistant to summarize the git diff in a given range") } ]]
    static std::string summarize_changes(std::string base) {
        if (base.empty()) base = "HEAD~1";
        const std::string text =
            "Please summarize the changes between " + base + " and HEAD.\n\n" +
            "Run the project_info tool and git diff --stat " + base +
            " HEAD for context, then give a concise, human-readable summary grouped by area.";
        return "[" + mcp::json_object({
            {"role", mcp::json_string("user")},
            {"content", mcp::json_object({
                {"type", mcp::json_string("text")},
                {"text", mcp::json_string(text)},
            })},
        }) + "]";
    }

    // completion/complete suggestions for the summarize_changes `base` argument.
    static std::string summarize_changes_complete(std::string_view arg_name, std::string_view arg_value) {
        if (arg_name != "base") return "[]";
        const std::string prefix(arg_value);
        const std::string log = git("log --format=%H --max-count=20");
        std::string out = "[";
        bool first = true;
        std::string line;
        for (char c : log) {
            if (c == '\n') {
                if (line.rfind(prefix, 0) == 0) {
                    if (!first) out += ",";
                    first = false;
                    out += mcp::json_string(line);
                }
                line.clear();
            } else {
                line += c;
            }
        }
        out += "]";
        return out;
    }
};

// Compile-time proof: reflection works with the mini parser backend.
static_assert(mcp::register_tools<^^device, mini::parser>().size() == 4);
static_assert(mcp::register_prompts<^^prompt_group, mini::parser>().size() == 1);

// ---------------------------------------------------------------------------
// Resources (hand-assembled: a Resource's uri/mime_type are not derivable from
// a function, so there is nothing for reflection to generate).
// ---------------------------------------------------------------------------

std::string server_info_text() {
    char cwd_buf[4096];
    const char* cwd = ::getcwd(cwd_buf, sizeof cwd_buf) ? cwd_buf : "?";
    return mcp::json_object({
        {"name", mcp::json_string("mcp-demo")},
        {"implementation", mcp::json_string("cpp-mini")},
        {"version", mcp::json_string("0.1.0")},
        {"transports", R"(["stdio"])"},
        {"cwd", mcp::json_string(cwd)},
        {"pid", std::to_string(::getpid())},
    });
}

std::string project_status_text() {
    return "branch: " + git("rev-parse --abbrev-ref HEAD") + "\nhead:   " +
           git("log -1 --oneline") + "\n\n" + git("status --short");
}

std::span<const mcp::Action> actions() {
    return mcp::register_tools<^^device, mini::parser>();
}

std::span<const mcp::Resource> resources() {
    static constexpr mcp::Resource value[] = {
        {
            .uri = "server://info",
            .name = "Server info",
            .description = "Runtime metadata about this MCP server",
            .mime_type = "application/json",
            .text = &server_info_text,
        },
        {
            .uri = "project://status",
            .name = "Project status",
            .description = "Git status (branch, HEAD, dirty files) of the repository containing the server's working directory",
            .mime_type = "text/plain",
            .text = &project_status_text,
        },
    };
    return value;
}

std::span<const mcp::Prompt> prompts() {
    return mcp::register_prompts<^^prompt_group, mini::parser>();
}

mcp::Registry registry() {
    return mcp::Registry{ actions(), resources(), prompts(), {} };
}

} // namespace mini
