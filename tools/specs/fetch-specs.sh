#!/bin/sh
#
# fetch-specs.sh - vendor MCP specification artifacts into assets/specs/.
#
# Reproducible: re-run it (after upstream changes or to refresh a revision) to
# regenerate the tree. Uses only curl; nothing else is required. The layout:
#
#   assets/specs/schema/<rev>/schema.json     canonical JSON Schema (wire contract)
#   assets/specs/changelog/<rev>.mdx          "changes since previous revision"
#   assets/specs/seps/*.md                    design rationales (2567/2575/2322)
#   assets/specs/docs/<rev>/...               key doc pages, verbatim from the
#                                             specification repo (docs/specification)
#
# Sources:
#   specification repo:    github.com/modelcontextprotocol/specification
#   SEPs (modelcontextprotocol repo): github.com/modelcontextprotocol/modelcontextprotocol
#
set -eu

SPEC_REPO=https://raw.githubusercontent.com/modelcontextprotocol/specification/main
MCP_REPO=https://raw.githubusercontent.com/modelcontextprotocol/modelcontextprotocol/main

# Resolve the repo root (this script lives at <repo>/tools/specs/).
SCRIPT=$0
while [ -L "$SCRIPT" ]; do
    LINK=$(readlink "$SCRIPT") 2>/dev/null || true
    if [ -z "$LINK" ]; then break; fi
    case "$LINK" in
        /*) SCRIPT=$LINK ;;
        *)  SCRIPT=$(dirname "$SCRIPT")/$LINK ;;
    esac
done
REPO=$(CDPATH= cd -- "$(dirname "$SCRIPT")/../.." && pwd)
SPECS=$REPO/assets/specs

fetch() {   # fetch <url> <dest>
    mkdir -p "$(dirname "$2")"
    echo "fetch-specs: ${2#"$REPO"/}"
    curl -fsSL "$1" -o "$2"
}

# --- canonical JSON Schema per revision (the wire contract) ------------------
for rev in 2025-11-25 2026-07-28; do
    fetch "$SPEC_REPO/schema/$rev/schema.json" "$SPECS/schema/$rev/schema.json"
done

# --- changelogs ("changes since previous revision") ---------------------------
for rev in 2025-11-25 2026-07-28; do
    fetch "$SPEC_REPO/docs/specification/$rev/changelog.mdx" "$SPECS/changelog/$rev.mdx"
done

# --- SEPs referenced by the 2026-07-28 port -----------------------------------
fetch "$MCP_REPO/seps/2567-sessionless-mcp.md" "$SPECS/seps/2567-sessionless-mcp.md"
fetch "$MCP_REPO/seps/2575-stateless-mcp.md"   "$SPECS/seps/2575-stateless-mcp.md"
fetch "$MCP_REPO/seps/2322-MRTR.md"            "$SPECS/seps/2322-MRTR.md"

# --- key doc pages, verbatim, per revision -------------------------------------
# 2025-11-25 (the revision currently implemented, transitionally).
for p in \
    basic/lifecycle.mdx \
    basic/transports.mdx \
    server/tools.mdx \
    server/resources.mdx \
    server/prompts.mdx \
    server/utilities/completion.mdx \
    server/utilities/pagination.mdx
do
    fetch "$SPEC_REPO/docs/specification/2025-11-25/$p" "$SPECS/docs/2025-11-25/$p"
done

# 2026-07-28 (the port target: sessionless, server/discover, MRTR, ...).
for p in \
    basic/index.mdx \
    basic/versioning.mdx \
    basic/transports/index.mdx \
    basic/patterns/index.mdx \
    basic/patterns/mrtr.mdx \
    basic/patterns/subscriptions.mdx \
    basic/patterns/cancellation.mdx \
    basic/patterns/progress.mdx \
    server/index.mdx \
    server/discover.mdx \
    server/tools.mdx \
    server/resources.mdx \
    server/prompts.mdx \
    server/utilities/completion.mdx \
    server/utilities/pagination.mdx \
    server/utilities/logging.mdx \
    server/utilities/caching.mdx
do
    fetch "$SPEC_REPO/docs/specification/2026-07-28/$p" "$SPECS/docs/2026-07-28/$p"
done

echo "fetch-specs: done"
