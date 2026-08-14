# Vendored MCP specification assets

Local copies of the MCP specification material this project consults while
implementing and porting the protocol. The library transitionally supports
**2025-11-25** and targets **2026-07-28** (legacy revisions are dropped), so
only those two revisions are vendored.

## Layout

```
schema/<rev>/schema.json     canonical JSON Schema for the revision (the wire contract)
changelog/<rev>.mdx          "changes since previous revision" (from the spec repo)
seps/*.md                    SEP-2567 / SEP-2575 / SEP-2322-MRTR (design rationales)
docs/<rev>/...               key doc pages, verbatim from the spec repo (docs/specification)
```

## Provenance

All files are fetched verbatim from upstream, no edits:

- `modelcontextprotocol/specification` (raw, branch `main`):
  - `schema/<rev>/schema.json`
  - `docs/specification/<rev>/<page>.mdx`
- `modelcontextprotocol/modelcontextprotocol` (raw, branch `main`):
  - `seps/<file>.md`

License: the specification repo is MIT-licensed; these are unmodified copies
for reference.

## Refresh

```sh
./tools/specs/fetch-specs.sh
```

Re-run to refresh an existing revision or pick up new ones; the script is the
single source of truth for the layout (add a revision/page there, run it, and
the tree regenerates).

## Not vendored (yet)

- Full per-revision doc trees (only the pages we touch are included). Add pages
  to `fetch-specs.sh` as needed, e.g. for the 2026-07-28 port.
- Older revisions (`2024-10-07` .. `2025-06-18`) — dropped from support; their
  changelogs are still online if ever needed.
