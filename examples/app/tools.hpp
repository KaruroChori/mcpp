#pragma once

#include <span>

#include <mcpp/mcpp.hpp>

namespace app {

/**
 * The session's opaque authorization context: the "permission level" this MCP
 * slot was granted. The library treats it as an opaque pointer; the app
 * decides what it carries and the policies interpret it.
 */
struct AuthContext {
    bool admin = false;
};

/**
 * Group/tool policy: requires an admin-granted session. Unguarded capabilities
 * stay visible to everyone; only `with<admin>`-tagged ones are gated.
 */
struct admin_policy {
    static bool authorized(const void* ctx) {
        const auto* c = static_cast<const AuthContext*>(ctx);
        return c && c->admin;
    }
};

/**
 * Tolerant first-line authenticator: consumes `{"auth":"admin"}` (granting the
 * admin context); any other first line is left to be processed as a normal
 * JSON-RPC message with default policies.
 */
mcp::AuthResult authenticate(std::string_view first_line);

/** Assembles the project's capabilities into a mcp::Registry. */
mcp::Registry registry();

} // namespace app
