#include "tools.hpp"

#include <cctype>
#include <type_traits>

#include <mcpp/mcpp.hpp>
#include <mcpp-server/simdjson-ops.hpp>

namespace hello {

struct greet_result {
    std::string greeting;
    bool shouted;
};

// A custom serializer: the whole point of mcp::with<...>::tool. This one is
// deliberately tiny and independent of the reflection machinery — it only knows
// how to emit the single result type of this project's tools, so it never pulls
// in <variant>/<optional>/<map> or the reflection-based json_serializer.
template <typename T>
struct plain_serializer {
    static std::string to_json(const T& v) {
        static_assert(std::is_same_v<T, greet_result>,
                      "hello::plain_serializer only knows how to serialize greet_result");
        return mcp::json_object({
            {"greeting", mcp::json_string(v.greeting)},
            {"shouted", v.shouted ? "true" : "false"},
        });
    }
};

struct chat {
    // Custom serializer, selected via mcp::with<...>::tool. The alias binds
    // the serializer while CTAD still deduces the string lengths from the
    // literals, so the annotation reads exactly like the plain form.
    [[= mcp::with<mcp::no_auth, plain_serializer>::tool{ .description = mcp::str("Greets the user by name, optionally SHOUTING.") } ]]
    static greet_result greet(std::string name) {
        bool shouted = false;
        if (!name.empty() && name.back() == '!') {
            shouted = true;
            name.pop_back();
        }
        for (auto& c : name) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return greet_result{ "Hello, " + name + "!", shouted };
    }
};

static_assert(mcp::register_tools<^^chat, mcp::simdjson_parser>().size() == 1);
static_assert(mcp::register_tools<^^chat, mcp::simdjson_parser>()[0].name == "greet");
// Compile-time proof that the tool's serializer came from the with<> alias
// (plain_serializer, not the default json_serializer).
using hello_config = std::remove_cv_t<
    typename [: std::meta::type_of(mcp::find_meta(^^chat::greet)) :]>;
static_assert(std::is_same_v<mcp::tool_serializer<hello_config>::template type<greet_result>,
                             plain_serializer<greet_result>>);

std::span<const mcp::Action> actions() {
    return mcp::register_tools<^^chat, mcp::simdjson_parser>();
}

std::span<const mcp::ResourceTemplate> resource_templates() {
    static constexpr mcp::ResourceTemplate value[] = {
        {
            .uri_template = "users://{id}",
            .name = "User profile",
            .description = "Profile for a user, looked up by id",
            .mime_type = "application/json",
        },
    };
    return value;
}

mcp::Registry registry() {
    return mcp::Registry{ actions(), {}, {}, resource_templates() };
}

} // namespace hello
