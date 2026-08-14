# Cap'n Proto schema for the binary-transport proof of concept.
#
# This models the *envelope* of an MCP tools/call request (the part that the
# mcpp reflection layer reads through its `Parser` seam). The schema lives here,
# outside the data: both sides compile it in, and the wire carries only the
# packed struct — no keys, no escaping, no per-message schema. Field ordinals
# below ARE the wire layout.
#
# `CallArguments` is a flat struct whose fields are named exactly like the
# reflected tool parameters this demo exposes (handle + direction / radius /
# payload). A real binary transport would codegen one struct per tool; this
# PoC shows a representative one. Keeping it flat (no unions) means the reader
# never touches an inactive member, so it is safe under -fno-exceptions.
#
# The tool result is still the standard MCP text envelope on the way back; only
# the request direction is binary here.

@0xa5c1f0d0e1b2c3d4;

struct CallRequest {
  params @0 :CallParams;

  struct CallParams {
    name @0 :Text;                       # tool name, e.g. "move"
    arguments @1 :CallArguments;
  }

  struct CallArguments {
    handle @0 :UInt64;                   # object handle for non-static tools
    direction @1 :Text;                  # north/south/east/west
    radius @2 :Int32;                    # look_around radius
    payload @3 :Text;                    # generic string arg
  }
}
