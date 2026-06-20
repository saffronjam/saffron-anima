# Phase 6 — The shared Luau typegen skeleton + the component-snapshot `.luau` emitter

**Status:** COMPLETED

**Depends on:** 10-protocol-codegen:phase-3-component-registry-macro, 10-protocol-codegen:phase-5-xtask-emitters-and-editor-repoint

## Goal

Build the shared `Rust-type → Luau` mapping module and the component-snapshot emitter — the `.luau`
replacement for `script_component_defs.generated.hpp` — so the typed `:get_component(name)` surface is
generated from the same DTO component shapes the registry knows. This is the **skeleton area 12 reuses**
to emit the `sa.*` binding API defs from the script-binding source; this phase delivers the mapping helper
and the component-snapshot half, leaving the API-surface half to area 12.

## Why this shape (NO LEGACY)

- **The same single-source discipline as `@saffron/protocol`, reused for Luau.** The locked ground rule
  (pre-plan §0): the Lua-facing type surface is a generated artifact from the Rust binding source, the same
  pattern as the DTOs. PP-7 owns the *typegen skeleton*; this phase provides it. The component-snapshot
  defs are emitted from the **DTO component shapes** the phase-3 registry registers — exactly as
  `emitScriptComponentDefs` reads the generated TS interfaces + the `scene_edit_components.cpp`
  registered-name set (`gen.ts:3361`), but reading the Rust DTOs + the macro's registered set instead.
- **A `.luau` defs file, not a C++ `string_view` blob.** `gen.ts` emits `SaComponentDefs` as a
  `constexpr std::string_view` in a `#pragma once` header appended to `library/sa.lua` at runtime
  (`gen.ts:3450`). NO LEGACY: there is no `library/sa.lua` overlay in the Rust engine (the hand-written
  overlay + its `check-script-defs` drift tripwire are deleted), so the emitter writes a plain `.luau` defs
  file into the project scaffold / script type root that the Luau type checker reads directly. No header
  wrapper, no append step.
- **One mapping helper, two emitters.** The `tsToLua` function (`gen.ts:3394`) maps a wire type to a Luau
  type annotation: `number`/`boolean`/`string` passthrough, `WireUuid → string`, `Vec3 → {x:number,
  y:number, z:number}`, `Vec4 → {…,w:number}`, nested DTO → `sa.<Name>`, vectors → `T[]`, unions
  passthrough. This phase implements it once over the Rust DTO types (a `Rust-type → Luau` mapper); the
  component-snapshot emitter and area 12's `sa.*` API emitter both call it, so the mapping has one owner.
- **The transitive class set, ported faithfully.** `emitScriptComponentDefs` grows the emitted
  `---@class` set transitively from the registered components (nested DTOs like `BVec3`/`PhysicsMaterial`/
  `Material` are emitted, unrelated DTOs are not — `gen.ts:3406`–3424). The Rust emitter reproduces this
  reachability walk over the DTO field graph rooted at the registered component set, and emits the
  `---@overload fun(self: sa.Entity, name: "<Comp>"): sa.<Comp>?` lines for `:get_component` (`gen.ts:3437`)
  plus the `---@class sa.<Comp>` blocks (`gen.ts:3430`), sorted by name to match.
- **The two synthetic shapes (`AnimationPlayer`, `MaterialAsset`) stay synthetic.** `gen.ts` injects these
  two component shapes by hand because they have no TS catalog entry (`gen.ts:3379`–3392); the Rust emitter
  keeps the same two literal field lists so the emitted defs match.
- **Area 12 boundary.** This phase stops at the component-snapshot defs + the mapping helper. The `sa.*`
  function/namespace/global defs (the binding API surface) are emitted by area 12 using this mapping
  helper, from the script-binding registration source — that is area 12's deliverable, hooked here. The
  shared module is the contract between the two areas.

## Grounding (real files / symbols)

- `tools/gen-control-dto/gen.ts`: `emitScriptComponentDefs` (3361, the emitter being ported),
  `tsToLua` (3394, the type mapper this phase owns), the transitive `reach` walk (3406–3424), the
  synthetic `AnimationPlayer`/`MaterialAsset` shapes (3379–3392), the `---@class`/`---@overload` output
  (3430–3448), the `componentDefsOut` write (`main`, 3492).
- `engine-old/source/saffron/sceneedit/scene_edit_components.cpp`: the `registerComponent<C>(reg, "Name")`
  calls (the registered-name set the emitter roots its reachability walk in — now read from the phase-3
  macro list).
- `engine-old/source/saffron/assets/script_component_defs.generated.hpp`: the committed C++ blob (the
  shape the `.luau` output reproduces, minus the header wrapper).
- Pre-plan §0 + PP-8 charter: "the typed `sa.*` surface is generated from the Rust binding definitions ...
  the same single-source-of-truth pattern as `@saffron/protocol`"; this phase is the shared skeleton.

## Acceptance gate

- `cargo build --workspace` succeeds; clippy + fmt clean; `#![deny(unsafe_code)]` holds.
- `xtask gen-protocol` (or a dedicated `gen-luau` subcommand the gate runs) writes the component-snapshot
  `.luau` defs; a `#[test]` asserts the emitted defs carry a `---@class sa.<Comp>` block for every
  registered component reachable in the wire-shape graph and the `:get_component` overloads, sorted by
  name, with field annotations from the shared mapper.
- A `#[test]` covers the mapper: `WireUuid → string`, `Vec3 → { x: number, y: number, z: number }`, a
  nested DTO → `sa.<Name>`, a `Vec<T>` → `T[]`, matching `tsToLua`.
- The two synthetic shapes (`AnimationPlayer`, `MaterialAsset`) are present with their literal field lists.
- The emitted `.luau` defs are byte-stable across re-runs (the freshness gate); no `library/sa.lua` overlay
  and no `check-script-defs` step exist anywhere in the tree.
