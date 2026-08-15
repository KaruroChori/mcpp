set_project("mcpp")
set_version("0.2.1")
set_xmakever("3.0.9") --not sure it must be this high, but I cannot be bother checking feature lists.

set_languages("c++26")
set_policy("generator.compile_commands", true)

add_requires("simdjson", {system = false, configs = {noexceptions = not has_config("exceptions")}})
add_requires("fmt", {system = false, configs = {header_only = true}})

-- Legacy 2025-11-25 protocol path, on by default for legacy clients.
-- Default it to false once we have substantial migration to 2026-07-28
option("legacy2025")
    set_default(true)
    set_showmenu(true)
    set_category("Build Options")
    set_description("Enable the legacy 2025-11-25 protocol path (dual-era)")
option_end()

-- Build the library with exceptions enabled, for exceptions-enabled consumers
-- (e.g. ones that use fmt or simdjson with exceptions themselves). Off by
-- default: the library stays exception-free.
option("exceptions")
    set_default(false)
    set_showmenu(true)
    set_category("Build Options")
    set_description("Build with exceptions enabled (match an exceptions-enabled consumer)")
option_end()

local common = function ()
    add_includedirs("include", {public = true})
    set_targetdir("$(projectdir)/build")
    set_optimize("fastest")
    add_cxflags("-pthread", "-fno-rtti", "-freflection", {force = true})
    if has_config("exceptions") then
        add_defines("SIMDJSON_EXCEPTIONS=1", "FMT_EXCEPTIONS=1")
    else
        add_cxflags("-fno-exceptions", {force = true})
        add_defines("SIMDJSON_EXCEPTIONS=0", "FMT_EXCEPTIONS=0")
    end
    if has_config("legacy2025") then
        add_defines("MCPP_LEGACY_2025")
    end
    add_ldflags("-pthread")
end

-- The core library: parser-free, header-only. 
-- This is the reusable part that an embedded target can use with its own JSON parser
-- (see examples/mini, which drives it with the mini parser backend).
target("mcp")
    set_kind("headeronly")
    common()
    add_headerfiles("include/mcpp/(**.hpp)", {prefixdir = "mcpp"})
    add_installfiles("assets/.opencode/skills/(**)", {prefixdir = "share/opencode/skills"})
    add_installfiles("examples/(**)", {prefixdir = "share/mcpp/examples"})
    add_installfiles("assets/specs/(**)", {prefixdir = "share/mcpp/specs"})
    add_installfiles("tools/mcpp-skill/mcpp-skill", {prefixdir = "bin"})
    add_installfiles("LICENSE", {prefixdir = "share/licenses/mcpp"})

-- The extended library: the simdjson + thread-pool server on top of the core.
target("mcpp-server")
    set_kind("static")
    common()
    add_deps("mcp")
    add_packages("simdjson", "fmt")
    add_headerfiles("include/mcpp-server/(**.hpp)", {prefixdir = "mcpp-server"})
    add_files("lib/mcpp-server/*.cpp")

-- The bridge: stdio <-> unix socket byte relay with slot authentication. 
-- It allowas stdio-only clients to reach an already-running app that hosts MCP slots on unix sockets. 
-- Standalone POSIX tool, no special library deps.
target("mcpp-bridge")
    set_kind("binary")
    set_targetdir("$(projectdir)/build")
    set_optimize("fastest")
    add_cxflags("-pthread", "-fno-exceptions", "-fno-rtti", {force = true})
    add_ldflags("-pthread")
    add_files("tools/mcpp-bridge/*.cpp")

-- `xmake test`: build with demos enabled and run the self-checking examples
-- (see tools/check/check-demos.sh). Requires a C++26 reflection toolchain.
task("test")
    set_category("test")
    set_menu {
        usage = "xmake test",
        description = "Build the demos and run the self-checking examples",
    }
    on_run(function ()
        os.exec("sh tools/check/check-demos.sh")
    end)
task_end()

-- Examples are opt-in, like kd3: `xmake f --with_demo=y`. The library happily builds without. 
option("with_demo")
    set_default(false)
    set_showmenu(true)
    set_category("Build Options")
    set_description("Build the example binaries (todo/hello-world, mini, object, async, binary, app)")
option_end()

if has_config("with_demo") then

    add_requires("capnproto")

local skip_demo_install = function (target)
    cprint("${dim}skip installing example %s (examples are never installed)", target:name())
end

target("mcp-example-hello")
    on_install(skip_demo_install)
    set_kind("binary")
    common()
    add_deps("mcpp-server")
    add_packages("simdjson", "fmt")
    add_files("examples/hello-world/*.cpp")

-- The full example app: stdio + unix-socket (SocketHost) + auth handshake,
-- prompts, resources, resource templates, admin-guarded tools, and an
-- async/progress/cancellation tool. The richest demo of the surface.
target("mcp-example-app")
    set_kind("binary")
    common()
    on_install(skip_demo_install)
    add_deps("mcpp-server")
    add_packages("simdjson", "fmt")
    add_files("examples/app/main.cpp", "examples/app/tools.cpp")

target("mcp-example-mini")
    set_kind("binary")
    common()
    on_install(skip_demo_install)
    add_deps("mcp")
    add_files("examples/mini/main.cpp", "examples/mini/tools.cpp", "examples/mini/mini-json.cpp")

target("mcp-example-object")
    set_kind("binary")
    common()
    on_install(skip_demo_install)
    add_deps("mcp")
    add_files("examples/mini/object_demo.cpp", "examples/mini/mini-json.cpp")

target("mcp-example-async")
    set_kind("binary")
    common()
    on_install(skip_demo_install)
    add_deps("mcpp-server")
    add_packages("simdjson", "fmt")
    add_files("examples/mini/async_demo.cpp")

-- Binary-transport proof of concept.
target("mcp-example-binary")
    set_kind("binary")
    common()
    on_install(skip_demo_install)
    add_deps("mcp")
    add_includedirs("examples/binary", "examples/binary/gen", {public = true})
    add_packages("capnproto")
    add_files("examples/binary/binary_demo.cpp", "examples/binary/gen/tools.capnp.c++")

end
