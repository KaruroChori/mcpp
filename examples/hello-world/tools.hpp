#pragma once

#include <span>

#include <mcpp/mcpp.hpp>

namespace hello {

/**
 * A different project reusing the same mcp library: one tool group with a
 * single `greet` tool, no resources, no prompts. The library is header-only
 * and decoupled from which capabilities are served (see mcp::Registry).
 */
mcp::Registry registry();

} // namespace hello
