#pragma once

#include <span>

#include <mcpp/mcpp.hpp>

namespace mini {

/**
 * A reflected capability set driving mcp::Protocol directly — no simdjson, no
 * threads. Tools and prompts are reflected `tool_group`/`prompt_group` members
 * compiled against the mini JSON parser backend (see tools.cpp). This proves
 * the reflection layer and protocol core are reusable with any JSON backend.
 */
mcp::Registry registry();

} // namespace mini
