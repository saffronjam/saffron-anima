# Phase 5 — Completeness gate, contract-test refactor, schema retirement, docs

**Status:** COMPLETED

**Depends on:** phase 4

## Goal

Prove parity is complete, shrink the live contract test to a tripwire that iterates the
generated manifest, retire the hand-authored payload schemas, and rewrite the docs and AGENTS
guidance for the DTO-first / typed-handler flow. After this phase the generated manifest is
the contract of record and the schemas are gone except the envelope.

## Implementation checkpoint

- The DTO generator now emits manifest fixture and skip metadata for every typed command, and
  fails generation if a command has neither.
- `tools/check-control-schema/check.ts` is manifest-driven: it compares live `help` with
  `command-manifest.generated.json`, validates non-skipped command results against
  `openrpc.generated.json`, keeps the envelope negative case, and keeps the raw quoted-u64
  tripwire.
- Hand-authored payload schemas were retired from `schemas/control/`; only
  `envelope.schema.json`, generated OpenRPC, generated command manifest, and local guidance remain.
- The editor no longer depends on `json-schema-to-typescript`.
- DTO-first guidance was updated in `schemas/control/AGENTS.md`,
  `engine/source/saffron/control/AGENTS.md`, and the tooling/control docs.
- `tools/ci/check.sh` and `make e2e` passed in the `saffron-build` toolbox.

## The completeness gate (both directions)

1. **Live help ≡ generated manifest.** The contract test calls the engine's `help`
   (`control_commands_render.cpp`, emits `{name,help}` per registered command) and asserts:
   every name in `help` appears in the generated manifest, and every manifest entry appears
   in `help`. No command can exist in one and not the other. The full registered set is 62
   commands (18 render + 28 scene + 16 asset `registerCommand` calls, the
   `registerBuiltinCommands` order, `control_server.cpp:140`); the gate covers all of them.
   (If `plans/scene-hierarchy/` phase 4 has landed its `set-parent` command first, the count
   is **63** and the manifest must include `set-parent` — the gate's both-directions check
   fails otherwise. See the README's Cross-plan coordination section.)

2. **Skip-with-reason lives in the manifest source, not the test.** Commands that genuinely
   cannot be exercised live — `quit` (kills the host), `screenshot` (writes a file),
   `load-scene`/`save-scene` (filesystem), `attach-/resize-native-viewport` (X11 side
   effect), the lifecycle/no-result ones (`select`, `deselect`, `destroy-entity`,
   `gizmo-pointer`, `pick` over empty space), and the reflective carve-outs (`help`,
   `dump-schema`) — carry an explicit `skip: "<reason>"` annotation **in the DTO/manifest
   source** (a comment annotation the generator reads into the manifest). The test reads the
   skip reason from the manifest; it is not buried in test code. `set-light` (no editor
   wrapper) is marked `se`-CLI-only, not skipped, so the gate still validates its shape.

3. **Classify by result kind.** The gate must not demand a result schema for every command:
   commands are `result-typed` (validate the result DTO), `echo`/`no-result` (assert
   `ok:true`, empty/ack payload), or `skip` (with reason). `set-light`, `quit`, `select`,
   etc. are not result-typed and the gate treats them by class — it does not fail for the
   absence of a result schema (EDITOR-map risk).

## Contract-test refactor (shrink to a tripwire, keep it)

The live test (`tools/check-control-schema/check.ts`) today hardcodes ~20 command↔schema
pairs and a fixed call sequence (`check.ts:155-178`). Refactor it to:

4. **Drive per-command live validation from the generated manifest** — a loop over manifest
   entries `{cmd, paramsFixture, resultDto-or-class, skip}`. Keep the test's value, drop its
   hand-maintenance.

5. **Preserve the data-dependent ordering.** Several cases thread ids: `add-entity {cube}`
   seeds an entity whose id feeds `inspect`/`rename`/`copy`; `get-thumbnail` needs a mesh id
   scraped from `list-assets`; project cases use a per-pid name (`check.ts` fixtures). A flat
   per-command fixture table cannot express "use the id from the previous call" — keep the
   seeding/threading logic **outside** the manifest loop (a small ordered preamble that
   produces the ids the loop's fixtures reference), and let the loop validate shapes.

6. **Keep the four tripwire assertions:** envelope shape (`envelope.schema.json` stays),
   id pattern `^[0-9]+$` (the `assertRawU64` raw-byte check — u64-as-quoted-decimal-string),
   completeness (step 1), and per-command live validation against the generated result DTO.
   The negative case (unknown command → `ok:false`) stays.

7. **Resolve the two-identifier issue.** `check.ts` validates against schema **files** by
   name; `gen-protocol` bundled by **title**. With schemas retiring, the manifest carries the
   DTO title as the single identifier and the test validates against the generated
   `$defs`/OpenRPC schema (not a sibling `.schema.json` file). The embedded validator in
   `check.ts` is repointed at the generated schema bundle.

## Schema retirement

8. **Delete the hand-authored payload schemas** in `schemas/control/` once the manifest-driven
   test proves parity for every result-typed command. `schemas/control/` holds **25
   `*.schema.json` files** (the directory's `AGENTS.md` / `CLAUDE.md` are not schemas). The
   split: **keep exactly one — `envelope.schema.json`** (the `{ok,error,result,id}` wrapper,
   not derived from a command DTO; the contract test still validates against it). **Retire the
   other 24**, including `uuid.schema.json` — its `^[0-9]+$` id-string invariant survives not
   as a file but as the `assertRawU64` raw-byte tripwire (step 6). The retired set is every
   component / result / shared-primitive schema: `asset-entry`, `asset-list`, `camera`,
   `components`, `directional-light`, `editor-camera`, `entity-list`, `entity-ref`,
   `environment`, `gizmo-state`, `inspect-result`, `material`, `mesh`, `name`, `point-light`,
   `project-info`, `render-stats`, `selection`, `spot-light`, `thumbnail`, `transform`,
   `uuid`, `vec3`, `vec4` — all superseded by the DTOs.

9. **`schemas/control/AGENTS.md` inverts.** Rewrite it: the schemas are no longer the source
   of truth; the C++ DTOs are, the generator emits the wire types, and the only hand-authored
   schema is the envelope. Or fold the directory into the generator's output location if it is
   left empty save the envelope.

## Docs and AGENTS rewrite (keep-current obligation)

10. **`docs/content/explanations/tooling-and-control/shared-types.md`** — its thesis
    ("schema authored first, C++ is a validated consumer, no named C++ DTO structs", lines
    20–26/42–48) inverts to "C++ DTOs authored first, schema/TS/OpenRPC generated". Update the
    `What | File | Symbols` pointer table to the `:Dto` partition + generator. Preserve the
    wire-invariant section (camelCase, u64-as-string, Euler radians, SpotLight degrees,
    Camera `near`/`far` key rename) — those invariants now live in the DTO field encodings,
    not the hand-written lambdas.
11. **`engine/source/saffron/control/AGENTS.md`** — rewrite the "Adding a command" section for
    the typed flow: declare a `Params`/`Result` DTO in the `:Dto` partition, use
    `registerCommand<Params, Result>`, regenerate, done. Replace the "Schema-first contract"
    bullet with "DTO-first contract; the generator emits schema/TS/OpenRPC; the contract test
    iterates the manifest." Drop the "hand-authored schema in `schemas/control/`" instruction.
12. **`docs/content/explanations/tooling-and-control/scene-commands.md` /
    `reference/control-commands.md`** — the `dump-schema`-as-codegen-seam description is
    obsolete; point at the generator instead.
13. **`docs/content/explanations/tooling-and-control/_index.md`** — update the hub's "Pages"
    table rows: the `shared-types` row (`:20`) still describes "schema-first wire contract:
    schemas → TS codegen + C++ contract test, wire invariants" with code pointers
    `schemas/control/*` + `tools/check-control-schema` — invert it to the DTO-first flow
    (the `:Dto` partition + `tools/gen-control-dto`). The `control-plane-architecture` row
    (`:14`) lists `registerCommand` / the command itable — extend it to mention the typed
    `registerCommand<Params, Result>` overload. This is the keep-current hub-row obligation
    (root `AGENTS.md`: an altered concept updates its explanation page **and its hub row**).
14. **`docs/content/explanations/tooling-and-control/control-plane-architecture.md`** — its
    "A command is data plus a closure" section describes the single erased
    `Result<json>(EngineContext&, const json&)` handler and `registerCommand` (the page leads
    with `CommandTraits` and the lone register path). Add the typed
    `registerCommand<Params, Result>` overload: the erasure thunk that parses `Params`, calls
    the handler, and serializes the `Result` DTO down to the same erased row, so the typed and
    raw handlers coexist. Keep the existing socket/drain/`Result<T>` narrative intact.
15. **Fix stale references in passing:** docs still cite `EditorContext` / `editor_app.cppm` /
    `EditorLayer` (now `SceneEditContext` / `scene_edit_context.cppm`, host registers
    components at `host.cppm`) — correct them where this phase touches the same pages.

## Validation

- `check.sh` green: build, the regenerate-and-diff gate, the shrunk manifest-driven contract
  test (completeness both directions + per-command validation + envelope + id pattern), the
  frontend build.
- Deleting a schema file no longer breaks anything (no consumer reads
  `schemas/control/*.schema.json` except the retired `gen-protocol` and the now-repointed
  contract test).
- Adding a throwaway command with a DTO and **forgetting** to regenerate makes the
  diff gate fail; adding it to the engine but **not** the manifest makes the completeness gate
  fail — both directions of the gate demonstrably bite.
- `make e2e` green; `se help` and `se <cmd>` outputs unchanged.

## Risks

- **Skip-list erosion.** If skip-with-reason is too liberal the completeness gate becomes
  theatre. Keep the skip set to commands that truly cannot run live (destructive/filesystem/
  X11/reflective) and validate everything else; review the skip list as part of this phase's
  done criteria.
- **se printer drift.** `tools/se` printers read result fields by string key
  (`tools/se/source/main.cpp`); with schemas gone there is no compile-time link from a DTO
  rename to a printer. Audit the printers against the manifest's result DTOs here, since this
  is the phase that removes the schema cross-check the contract test used to provide.
- **Two-identifier validator change.** Repointing `check.ts`'s embedded validator from
  sibling `.schema.json` files to the generated bundle is the riskiest test change; do it with
  the schemas still present (validate against both, assert agreement) before deleting the
  files, so the cutover is proven, not assumed.
- **Component result shapes still opaque.** `inspect` / `get-environment` /
  `set-environment` carry component/environment bodies that are still opaque json until phase
  6; the contract test validates their envelope and id discipline but not the inner component
  fields. Note this coverage boundary so phase 6 closes it.
