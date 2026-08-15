#include "tools.hpp"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <mcpp/mcpp.hpp>
#include <mcpp-server/simdjson-ops.hpp>

namespace todo {

namespace {

struct item {
    std::int64_t id;
    std::string text;
    bool done;
};

/// The shared list, guarded by items_mutex(). One list for every client: the
/// simplest possible state model, good enough for a hello-world example.
std::vector<item>& items() {
    static std::vector<item> l;
    return l;
}

std::mutex& items_mutex() {
    static std::mutex m;
    return m;
}

} // namespace

namespace groups {

struct [[= mcp::tool_group{} ]] todo_list {
    [[= mcp::tool{ .description = mcp::str("Adds a todo item, returning its id.") } ]]
    static auto add(std::string text) {
        std::lock_guard<std::mutex> lk(items_mutex());
        auto& l = items();
        const std::int64_t id = l.empty() ? 1 : l.back().id + 1;
        l.push_back(item{ id, std::move(text), false });
        return (struct{ std::int64_t id; bool ok; }){ id, true };
    }

    [[= mcp::tool{ .description = mcp::str("Lists the todo items.") } ]]
    static auto list() {
        std::lock_guard<std::mutex> lk(items_mutex());
        return items();
    }

    [[= mcp::tool{ .description = mcp::str("Marks a todo item as done.") } ]]
    static auto complete(std::int64_t id) {
        std::lock_guard<std::mutex> lk(items_mutex());
        for (auto& it : items()) {
            if (it.id == id) {
                it.done = true;
                return true;
            }
        }
        return false;
    }

    [[= mcp::tool{ .description = mcp::str("Removes a todo item.") } ]]
    static auto remove(std::int64_t id) {
        std::lock_guard<std::mutex> lk(items_mutex());
        auto& l = items();
        for (auto it = l.begin(); it != l.end(); ++it) {
            if (it->id == id) {
                l.erase(it);
                return true;
            }
        }
        return false;
    }
};

} // namespace groups

static_assert(mcp::register_tagged_tools<^^groups, ^^mcp::tool_group, mcp::simdjson_parser>().size() == 4);
static_assert(mcp::register_tagged_tools<^^groups, ^^mcp::tool_group, mcp::simdjson_parser>()[0].name == "add");
static_assert(mcp::register_tagged_tools<^^groups, ^^mcp::tool_group, mcp::simdjson_parser>()[1].name == "list");
static_assert(mcp::register_tagged_tools<^^groups, ^^mcp::tool_group, mcp::simdjson_parser>()[2].name == "complete");
static_assert(mcp::register_tagged_tools<^^groups, ^^mcp::tool_group, mcp::simdjson_parser>()[3].name == "remove");

std::span<const mcp::Action> actions() {
    return mcp::register_tagged_tools<^^groups, ^^mcp::tool_group, mcp::simdjson_parser>();
}

mcp::Registry registry() {
    return mcp::Registry{ actions(), {}, {}, {} };
}

} // namespace todo
