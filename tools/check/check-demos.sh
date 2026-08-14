#!/bin/sh
#
# check-demos.sh - build the library with the demos enabled and run the
# self-checking example binaries. Any failure exits non-zero.
#
# The self-checking demos (mcp-example-object, -async, -binary) exercise the
# object-handle model, the async path, and the Cap'n Proto binary transport;
# they print their own ok/FAILED lines and exit non-zero on internal failure.
# This script is the repo's regression gate: run it locally, or wire it into
# any CI that has a C++26 reflection toolchain (gcc 16+ with -freflection).
#
# Usage:
#   ./tools/check/check-demos.sh
#
set -eu

# --- locate the repo root (this script lives in <repo>/tools/check/) ---------
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
cd "$REPO"

# --- toolchain gate (mirrors the skill's §3) -----------------------------------
GCC_LINE=$(gcc --version 2>/dev/null | head -n1)
GCC_MAJOR=$(printf '%s' "$GCC_LINE" | sed -n 's/.* \([0-9][0-9]*\)\..*/\1/p')
if [ "${GCC_MAJOR:-0}" -lt 16 ] 2>/dev/null; then
    echo "check-demos: requires gcc 16+ with -freflection; found: ${GCC_LINE:-no gcc on PATH}" >&2
    exit 1
fi
if ! printf 'int main(){}' | gcc -std=c++26 -freflection -x c++ - -fsyntax-only 2>/dev/null; then
    echo "check-demos: gcc ${GCC_MAJOR} does not support -freflection." >&2
    exit 1
fi

# --- build with demos ----------------------------------------------------------
xmake f --with_demo=y
xmake

# --- run the self-checking demos -----------------------------------------------
fail=0
for demo in mcp-example-object mcp-example-async mcp-example-binary; do
    echo "check-demos: running $demo ..."
    if ! ./build/$demo; then
        fail=1
    fi
done

if [ "$fail" -eq 0 ]; then
    echo "check-demos: all self-checks passed"
else
    echo "check-demos: some self-checks FAILED" >&2
    exit 1
fi
