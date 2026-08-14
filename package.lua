package("mcpp")
    set_homepage("https://github.com/karurochori/mcpp")
    set_description("A C++26 reflection-based Model Context Protocol (MCP) server library")
    set_license("LGPL-3.0")

    -- The reflection flags are mandatory; do not let xmake probe-and-drop them.
    set_policy("check.auto_ignore_flags", false)

    add_urls("https://github.com/karurochori/mcpp.git")

    on_load(function (package)
        -- mcpp-server links simdjson and fmt; consumers need them transitively.
        package:add("deps", "simdjson", "fmt")
        -- Consumers compile the headers, so they need the flags the library is built with.
        package:add("cxflags", "-fno-exceptions", "-fno-rtti", "-freflection", {force = true})
        package:add("defines", "SIMDJSON_EXCEPTIONS=0", "FMT_EXCEPTIONS=0")
    end)

    on_install(function (package)
        import("package.tools.xmake").install(package)
    end)

    on_test(function (package)
        assert(package:check_cxxsnippets({test = [==[
            #include <mcpp/mcpp.hpp>
            #include <mcpp-server/mcpp-server.hpp>
            struct [[= mcp::tool_group{} ]] ops {
                [[= mcp::tool{ .description = mcp::str("Adds two integers.") } ]]
                static auto add(std::int64_t a, std::int64_t b) {
                    return (struct{ std::int64_t sum; }){ a + b };
                }
            };
            void test() {
                auto tools = mcp::register_tools<^^ops, mcp::simdjson_parser>();
                (void)tools;
            }
        ]==]}, {configs = {languages = "c++26"}}))
    end)
package_end()
