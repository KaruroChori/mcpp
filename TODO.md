# TODO

Open and in-progress items. Completed work is recorded in the git history.

## In progress

- [ ] **Doxygen documentation pass.**
  The library headers (`include/mcpp/*.hpp`, `include/mcpp-server/*.hpp`) and
  implementation files (`lib/mcpp-server/*.cpp`) are converted to Doxygen
  style (`///`, `@param`, `@return`, `@tparam`, `///<`). Remaining: add
  `@file` header blocks presenting the copyright/license to every library
  file (per-file subagents), and the examples + `tools/mcpp-bridge/bridge.cpp`.

- [x] **CITATION.cff.** Citation File Format metadata at the repo root
  (kd3-style), presenting the software for citation.

- [x] **package.lua.** xrepo package descriptor at the repo root (kd3-style):
  builds via `package.tools.xmake`, propagates the reflection flags and the
  simdjson/fmt deps to consumers.

## Open

- [ ] **SPDX header lines.**
  Follow-up from B4: add `SPDX-License-Identifier: LGPL-3.0` and a copyright
  line to each header and source file.

- [x] **Ship the vendored specs with the install.**
  `assets/specs/` rides along with `xmake install` (`share/mcpp/specs/`),
  referenced by the skill.

- [ ] **F7 — Embedded/mini engine as a separate effort (design note).**
  The embedded/low-power story is not an extension of the host marshalling
  (growing `read_arg`/schema machinery). It stays a minimal, parser-free core
  driven by a small custom `Parser`, built as its own thing (see `examples/mini`
  as the seed). Do not fold host-side read/arg or schema breadth into the
  embedded path.

## Decided, not scheduled

- The 2026-07-28 optional features are deliberately not implemented (not useful
  for our application): `extensions`, OTEL trace-context `_meta` keys,
  `instructions`, `HeaderMismatch` (HTTP), `subscriptions/listen` (HTTP),
  per-request `logLevel` consumption, MRTR `input_required`.

## Protocol reference

- Vendored spec: `assets/specs/` (schema, changelog, docs); refresh with
  `tools/specs/fetch-specs.sh`.
- `docs/protocol.md`: implemented revisions (2026-07-28 and the legacy
  2025-11-25 dual-era path), the wire details, binary transports.
