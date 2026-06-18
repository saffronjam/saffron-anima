# Phase 9 — The `sa.*` API `.luau` typegen, and deleting the overlay + tripwire

**Status:** COMPLETED

**Depends on:** 12-scripting:phase-2-value-types-and-binding-table, 10-protocol-codegen:phase-6-luau-typegen-skeleton

## Goal

Emit the `sa.*` API surface as Luau type defs from the phase-2 binding-descriptor table, using area 10's
shared `Rust-type → Luau` mapper, and assemble it with area 10's component-snapshot emitter into one
`.luau` defs file — the single generated replacement for the hand-written `SaLuaDefs` overlay +
`SaComponentDefs`. With this phase, the hand-written `library/sa.lua` overlay and the `check-script-defs`
drift tripwire are gone, replaced by the generated-defs-fresh diff check.

## Why this shape (NO LEGACY)

- **One source, generated defs — the locked ground rule.** Pre-plan §0: the typed `sa.*` surface is
  generated from the Rust binding source, the same single-source pattern as `@saffron/protocol`. The
  phase-2 binding-descriptor table is that source; this phase's emitter walks it to produce the
  `sa.Vec3` value class (fields + `---@operator` overloads + methods), the `sa.Entity` method set, the
  `sa.RayHit`/`sa.RagdollState`/`sa.ScriptSelf` classes, the `sa.*` free-function/global table, and the
  `sa.ComponentName` alias (the registered-name union). It reuses area 10 phase-6's `Rust-type → Luau`
  mapper (the `tsToLua` analogue) — area 12 adds the API half on the same mapper, the explicit reuse the
  area-10 README §8 hooks.
- **A plain `.luau` defs file, no `---@meta` string blob, no header.** C++ emitted the API surface as a
  hand-written `SaLuaDefs` `constexpr std::string_view` `---@meta` raw string (`assets.cppm:1078`–1185)
  and the components as the generated `SaComponentDefs` `#pragma once` header
  (`script_component_defs.generated.hpp`), concatenated and written to `library/sa.lua` at runtime
  (`ensureScriptLibrary`, `assets.cppm:1211`). NO LEGACY: the Rust emitter (an xtask `gen-luau`
  subcommand, or the `gen-protocol` Luau step — co-owned with area 10) writes a single `.luau` defs file;
  no `string_view` blob, no `#pragma once` wrapper, no runtime append. The `.luarc.json` project settings
  (LuaLS pointed at `library/`, `sa` global, sandboxed-out libs disabled — `assets.cppm:1189`) stay as a
  write-when-absent scaffold (owned by 07-assets project I/O), retargeted to the `.luau` defs.
- **`check-script-defs` is deleted with no behavioral replacement.** The tripwire (`tools/
  check-script-defs/check.ts`) existed only to keep the imperative C++ bindings and the hand-written
  `SaLuaDefs` in sync — two copies that could drift. With one source there is no second copy: its two
  checks (every live `.addFunction`/`rawset` name documented; every registered component in the
  `sa.ComponentName` alias) are now *structurally* impossible to fail because both the defs and the
  registration come from the same table / the same registry. Its freshness role folds into the xtask
  generated-artifacts-fresh diff (01-build phase-6 / area 10 phase-5): re-running the generator must
  produce a clean git diff. The `check.ts` file, its `AGENTS.md`, the `SaLuaDefs`/`SaComponentDefs`
  blobs, and `emitScriptComponentDefs`'s C++-header wrapper are all removed.
- **The full API surface is the one the C++ `SaLuaDefs` documented.** As a completeness reference, the
  emitted defs must cover: `sa.Vec3`, `sa.RayHit`, `sa.RagdollState`, `sa.ScriptSelf` (the
  `on_create`/`on_update`/`on_destroy`/`on_trigger_*`/`on_contact` handler shape), `sa.Entity`'s full
  method set, every `sa.*` global (`log`, the input trio + mouse, `get_entity_by_name`/`find_all_by_name`/
  `find_by_uuid`/`primary_camera`/`spawn`, `vec3`/`look_at`/`lerp`, `raycast`/`spherecast`, `broadcast`,
  the scheduler `wait`/`delay`/`spawn_task`), and the `sa.ComponentName` alias — but generated, not
  transcribed (`assets.cppm:1083`–1184 is the *shape* to reproduce, not a source to copy).

## Grounding (real files / symbols)

- `engine-old/source/saffron/assets/assets.cppm`: `SaLuaDefs` (the hand-written API `---@meta` overlay,
  1078–1185), `SaComponentDefs` include + `ensureScriptLibrary` (the concat + write, 1198–1212),
  `LuarcJson` (1189–1196).
- `engine-old/source/saffron/assets/script_component_defs.generated.hpp`: the committed component-defs
  blob (the shape area 10 phase-6 reproduces; this phase assembles the API half alongside it).
- `tools/check-script-defs/check.ts` + `tools/check-script-defs/AGENTS.md`: the drift tripwire being
  deleted (its two checks become structurally impossible).
- area 10 phase-6: the shared `Rust-type → Luau` mapper + the component-snapshot emitter this phase
  builds the API half onto; area 10 README §8 (the area-12 hook).

## Acceptance gate

- `cargo build --workspace` succeeds; `#![deny(unsafe_code)]`; clippy + fmt clean.
- `xtask` (the `gen-luau`/`gen-protocol` Luau step) writes one `.luau` defs file; a `#[test]` asserts it
  carries: `---@class sa.Vec3` with its `---@operator` lines + methods; `sa.Entity`'s full method set;
  `sa.RayHit`/`sa.RagdollState`/`sa.ScriptSelf`; every `sa.*` global from the binding table; the
  `sa.ComponentName` alias listing every registered component; and the component-snapshot
  `---@class`/`:get_component` overloads (from area 10 phase-6) — all from the single source.
- A `#[test]` asserts the emitted defs are byte-stable across re-runs (the freshness gate).
- A repository check asserts no `library/sa.lua` overlay, no `SaLuaDefs`/`SaComponentDefs` blob, and no
  `tools/check-script-defs` exist anywhere in the Rust tree.
- The `tools/ci/check.sh`-equivalent gate (01-build) runs the generator and asserts a clean git diff in
  place of the deleted `check-script-defs` step.
