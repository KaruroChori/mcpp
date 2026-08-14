#pragma once

/// @file mcpp-server.hpp
/// @brief Umbrella header for the mcpp-server layer.
///
/// Depends on the core (JsonView/Protocol) and on a concrete JSON parser, here simdjson.
///
/// @see mcp::Server, the concrete simdjson + thread-pool adapter on Protocol.
/// @see mcp::SocketHost, the thread-per-connection multi-session host.
/// @see mcp::simdjson_parser, the simdjson Parser backend, also passed to
///      register_tools<Group, Parser> for reflected tools.
/// SPDX-License-Identifier: LGPL-3.0

#include <mcpp/mcpp.hpp>
#include "server.hpp"
#include "simdjson-ops.hpp"
#include "socket-host.hpp"
