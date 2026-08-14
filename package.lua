package("mcpp")
    set_homepage("https://github.com/karurochori/mcpp")
    set_description("A C++26 reflection-based Model Context Protocol (MCP) server library")
    set_license("LGPL-3.0")

    -- The reflection flags are mandatory; do not let xmake probe-and-drop them.
    set_policy("check.auto_ignore_flags", false)

    add_urls("https://github.com/karurochori/mcpp.git")

    -- Build and link the library with exceptions enabled, for exceptions-enabled
    -- consumers (e.g. ones that use fmt or simdjson with exceptions themselves).
    -- Off by default: the library is built exception-free.
    add_configs("exceptions", {description = "Build with exceptions enabled", default = false, type = "boolean"})

    on_load(function (package)
        local exceptions = package:config("exceptions")
        -- mcpp-server links simdjson and fmt; consumers need them transitively.
        -- simdjson must match the mode the library was built with.
        package:add("deps", "simdjson", {configs = {noexceptions = not exceptions}})
        package:add("deps", "fmt", {configs = {header_only = true}})
        -- Reflection splices live in the headers, so consumers must compile with
        -- -freflection. The library is built exception-free by default, but that
        -- is its own profile; consumers keep their own exception/RTTI settings.
        -- Only SIMDJSON_EXCEPTIONS=0 is forced in the default mode so simdjson
        -- headers match the prebuilt library. fmt is not in the public headers,
        -- so a consumer's own fmt usage is never touched.
        package:add("cxflags", "-freflection", {force = true})
        if not exceptions then
            package:add("defines", "SIMDJSON_EXCEPTIONS=0")
        end
    end)

    on_install(function (package)
        import("package.tools.xmake").install(package, {exceptions = package:config("exceptions")})
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
