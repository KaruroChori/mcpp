#include "tools.hpp"

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <span>
#include <thread>
#include <type_traits>

#include <mcpp/mcpp.hpp>
#include <mcpp-server/simdjson-ops.hpp>

namespace app {

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

std::string project_status_text() {
    const std::string branch = git("rev-parse --abbrev-ref HEAD");
    const std::string head = git("log -1 --oneline");
    const std::string status = git("status --short");
    return "branch: " + (branch.empty() ? "<none>" : branch) + "\n" +
           "head:   " + (head.empty() ? "<none>" : head) + "\n\n" + status;
}

} // namespace

// ---------------------------------------------------------------------------
// Result types. Tools return their natural values; the machinery serializes
// them to JSON via reflection (serializer.hpp). Single-field results use the
// anonymous-struct form (verified: `return_type_of` reflects the unnamed type
// and it serializes like any literal struct); multi-field results and variant
// alternatives are named for readability.
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
// Tool groups. Each member function is a tool, decorated with a config
// struct: `[[= mcp::tool{ .description = mcp::str("...") } ]]`. Name, schema,
// flags and the invocation are derived by reflection; the registry discovers
// everything automatically — no manual registration list. Non-static tools are
// called on a static instance of the group (a tool group is a stateful object).
//
// Groups are spread across multiple structs, each tagged `[[= mcp::tool_group{} ]]`
// so `register_tagged_tools<^^groups>()` picks them all up from the namespace.
// ---------------------------------------------------------------------------

namespace groups {

struct [[= mcp::tool_group{} ]] arithmetic {
    [[= mcp::tool{ .description = mcp::str("Returns the message back verbatim. Use for connectivity sanity checks.") } ]]
    static auto echo(std::string message) {
        return (struct{ std::string echoed; }){ std::move(message) };
    }

    [[= mcp::tool{ .description = mcp::str("Adds two integers.") } ]]
    static auto add(std::int64_t a, std::int64_t b) {
        return (struct{ std::int64_t sum; }){ a + b };
    }
};

struct [[= mcp::tool_group{} ]] repo {
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

    // Example non-static tool: called on a static `repo` instance.
    [[= mcp::tool{ .description = mcp::str("Reports a count of how many tool calls this group instance has served.") } ]]
    auto state() const {
        return (struct{ std::int64_t served; }){ served };
    }

    // Guarded tool: only visible/callable on an admin-granted session.
    [[= mcp::with<admin_policy>::tool{ .description = mcp::str("Reports the raw, unredacted project status. Admin only.") } ]]
    static std::string raw_project_info() {
        return mcp::json_object({
            {"branch", mcp::json_string(git("rev-parse --abbrev-ref HEAD"))},
            {"head", mcp::json_string(git("rev-parse --short HEAD"))},
            {"status", mcp::json_string(git("status --short"))},
        });
    }

    mutable std::int64_t served = 0;
};

struct banner_result {
    std::string text;
    bool bold;
};

// A custom serializer: the whole point of mcp::with<...>::tool. This one is
// deliberately tiny and independent of the reflection machinery — it only knows
// how to emit the single result type of this tool, so it never pulls in
// <variant>/<optional>/<map> or the reflection-based json_serializer. The
// alias binds the serializer while CTAD still deduces the string lengths from
// the literals, so the annotation reads exactly like the plain form.
template <typename T>
struct plain_serializer {
    static std::string to_json(const T& v) {
        static_assert(std::is_same_v<T, banner_result>,
                      "app::plain_serializer only knows how to serialize banner_result");
        return mcp::json_object({
            {"text", mcp::json_string(v.text)},
            {"bold", v.bold ? "true" : "false"},
        });
    }
};

struct [[= mcp::tool_group{} ]] formatted {
    [[= mcp::with<mcp::no_auth, plain_serializer>::tool{ .description = mcp::str("Returns a fixed banner. Demonstrates a custom serializer selected via mcp::with<...>::tool.") } ]]
    static banner_result banner() {
        return banner_result{ "mcp-demo", true };
    }
};

// Prompt: a function whose return is the standard MCP `messages` array. The
// completion hook is the sibling `summarize_changes_complete` (default
// `<prompt>_complete` naming convention); the reflection layer wires it in.
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
        // Suggest recent commit-ish values when the user has typed something.
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

} // namespace groups

// Compile-time proof that the tagged discovery picks up every group.
static_assert(mcp::register_tagged_tools<^^groups, ^^mcp::tool_group, mcp::simdjson_parser>().size() == 7);
static_assert(mcp::register_tagged_tools<^^groups, ^^mcp::tool_group, mcp::simdjson_parser>()[0].name == "echo");
static_assert(mcp::register_tagged_tools<^^groups, ^^mcp::tool_group, mcp::simdjson_parser>()[1].name == "add");
static_assert(mcp::register_tagged_tools<^^groups, ^^mcp::tool_group, mcp::simdjson_parser>()[2].name == "project_info");
static_assert(mcp::register_tagged_tools<^^groups, ^^mcp::tool_group, mcp::simdjson_parser>()[3].async);
static_assert(mcp::register_tagged_tools<^^groups, ^^mcp::tool_group, mcp::simdjson_parser>()[0].description == "Returns the message back verbatim. Use for connectivity sanity checks.");
// The last tool is admin-guarded; its `authorized` must be wired.
static_assert(mcp::register_tagged_tools<^^groups, ^^mcp::tool_group, mcp::simdjson_parser>()[5].name == "raw_project_info");
static_assert(mcp::register_tagged_tools<^^groups, ^^mcp::tool_group, mcp::simdjson_parser>()[5].authorized != nullptr);
static_assert(mcp::register_tagged_tools<^^groups, ^^mcp::tool_group, mcp::simdjson_parser>()[6].name == "banner");
// Compile-time proof that the banner tool's serializer came from the with<>
// alias (plain_serializer, not the default json_serializer).
using formatted_config = std::remove_cv_t<
    typename [: std::meta::type_of(mcp::find_meta(^^groups::formatted::banner)) :]>;
static_assert(std::is_same_v<mcp::tool_serializer<formatted_config>::template type<groups::banner_result>,
                             groups::plain_serializer<groups::banner_result>>);
static_assert(mcp::register_tagged_prompts<^^groups, ^^mcp::prompt_group, mcp::simdjson_parser>().size() == 1);
// The completion hook is discovered by the default naming convention.
static_assert(mcp::register_tagged_prompts<^^groups, ^^mcp::prompt_group, mcp::simdjson_parser>()[0].complete != nullptr);

// Override probe: `complete_for` binds a hook whose name does NOT follow the
// convention, so it must be wired through the explicit reference.
struct override_probe {
    [[= mcp::tool{ .description = mcp::str("x") } ]]
    static std::string p(std::string a) { return a; }
    [[= mcp::complete_for{ .prompt = ^^override_probe::p } ]]
    static std::string oddly_named_hook(std::string_view, std::string_view) { return "[]"; }
};
static_assert(mcp::register_prompts<^^override_probe, mcp::simdjson_parser>()[0].complete != nullptr);

std::span<const mcp::Action> actions() {
    return mcp::register_tagged_tools<^^groups, ^^mcp::tool_group, mcp::simdjson_parser>();
}

std::span<const mcp::Resource> resources() {
    static constexpr mcp::Resource value[] = {
        {
            .uri = "server://info",
            .name = "Server info",
            .description = "Runtime metadata about this MCP server",
            .mime_type = "application/json",
            .text = []() {
                char cwd_buf[4096];
                const char* cwd = ::getcwd(cwd_buf, sizeof cwd_buf) ? cwd_buf : "?";
                return mcp::json_object({
                    {"name", mcp::json_string("mcp-demo")},
                    {"implementation", mcp::json_string("cpp-refl")},
                    {"version", mcp::json_string("0.1.0")},
                    {"transports", R"(["stdio","unix-socket"])"},
                    {"cwd", mcp::json_string(cwd)},
                    {"pid", std::to_string(::getpid())},
                });
            },
        },
        {
            .uri = "project://status",
            .name = "Project status",
            .description = "Git status (branch, HEAD, dirty files) of the repository containing the server's working directory",
            .mime_type = "text/plain",
            .text = []() { return project_status_text(); },
        },
    };
    return value;
}

std::span<const mcp::Prompt> prompts() {
    return mcp::register_tagged_prompts<^^groups, ^^mcp::prompt_group, mcp::simdjson_parser>();
}

std::span<const mcp::ResourceTemplate> resource_templates() {
    static constexpr mcp::ResourceTemplate value[] = {
        {
            .uri_template = "git://commit/{hash}",
            .name = "Git commit",
            .description = "A single git commit, looked up by hash",
            .mime_type = "text/plain",
        },
    };
    return value;
}

mcp::Registry registry() {
    return mcp::Registry{ actions(), resources(), prompts(), resource_templates() };
}

// A simple JSON substring check: the credential is `{"auth":"admin"}`. We
// avoid a full parse here because the line may equally well be the start of a
// real JSON-RPC exchange that we must leave untouched.
mcp::AuthResult authenticate(std::string_view first_line) {
    static constexpr std::string_view marker = R"("auth":"admin")";
    if (first_line.find(marker) != std::string_view::npos) {
        static constexpr AuthContext admin_ctx{ .admin = true };
        return { .ctx = &admin_ctx, .consumed = true };
    }
    return {};   // not a handshake; default policies apply
}

} // namespace app
