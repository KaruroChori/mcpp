#pragma once

/// @file mcpp.hpp
/// @brief Umbrella header for the core library.
/// Umbrella header for the mcpp core library. Include this to use the
/// header-only, parser-free MCP core: the reflection layer, protocol
/// dispatch, JsonView, serializers, and the stream abstraction.
///
/// Every function here is inline, and the only external dependency is the C++
/// standard library (plus `<meta>` for reflection). It has no concrete JSON
/// parser: parsing is delegated to a `Parser` backend. Bring your own
/// (simdjson, the mini parser in examples/mini, ...) and pass it to
/// `register_tools<Group, Parser>`.
/// @see json-view.hpp
///
/// Platform note: the stream abstraction (stream.hpp) requires a POSIX
/// platform: stdio reads via `read()` and unix domain sockets, so the core
/// compiles on Linux, macOS and the BSDs, and explicitly not on Windows
/// (which is not a target: MSVC has no `-freflection`). Everything else in
/// the core is portable C++26.
///
/// The extended, opinionated server (simdjson + threads + SocketHost) lives
/// in the separate mcpp-server library.
/// @see mcpp-server/mcpp-server.hpp
/// SPDX-License-Identifier: LGPL-3.0

#include "action.hpp"
#include "json.hpp"
#include "json-view.hpp"
#include "meta.hpp"
#include "protocol.hpp"
#include "read-arg.hpp"
#include "serializer.hpp"
#include "stream.hpp"
