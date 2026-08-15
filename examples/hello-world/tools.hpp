#pragma once

#include <span>

#include <mcpp/mcpp.hpp>

namespace todo {

/**
 * The simple example: a shared, mutex-guarded todo list exposed over MCP.
 * No resources, no prompts, no custom serializers: just reflection turning a
 * few member functions of a tool group into tools.
 */
mcp::Registry registry();

} // namespace todo
