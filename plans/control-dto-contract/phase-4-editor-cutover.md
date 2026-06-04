# Phase 4 — Editor cutover

**Status:** NOT STARTED

**Depends on:** phase 2, phase 3

## Goal

Make the generated `se-types.ts` the editor's only protocol source, give `call<C>(cmd,
params)` types in **both** directions, and delete `callRaw` / `.raw()` along with every
manual `as` cast at the call sites. Needs phases 2 and 3 because deleting `callRaw` requires
every command the editor calls to have a typed params **and** result DTO.

## Steps

### Replace the protocol artifact

1. **Generate `editor/src/protocol/se-types.ts`** from the DTOs (the generator's TS output,
   phase 1 step 5) carrying the full `CommandParamsMap` **and** `CommandResultMap` — both
   derived from the DTOs, not hand-kept. This replaces the schema-derived
   `editor/src/protocol/index.ts` and **deletes the hand-written `CommandResultMap` block**
   in `editor/scripts/gen-protocol.ts:65-95`. Point the `@saffron/protocol` alias
   (`editor/tsconfig.json`, and `tests/e2e/tsconfig.json:12` which path-maps to
   `editor/src/protocol/index.ts`) at the new file, or keep the filename `index.ts` so the
   alias is unchanged — prefer keeping the path so the e2e tsconfig and editor tsconfig need
   no edit.

   **Compatibility requirement — preserve the existing named interface exports.** The
   generated module MUST keep exporting the same named TypeScript types the current
   `index.ts` does, from the same `@saffron/protocol` module path, because both `client.ts`
   and the e2e suite import them by name:
   - `client.ts:10-26` imports `AssetList, CommandResultMap, EditorCamera, EntityList,
     EntityRef, Environment, GizmoState, InspectResult, Material, RenderStats, Selection,
     Thumbnail, Transform, Vec3, ProjectInfo`.
   - `tests/e2e/*.test.ts` import (type-only) `EntityList, EntityRef, InspectResult,
     RenderStats, Selection` (`control.test.ts:12`, `rendering.test.ts:8`, `scene.test.ts:8`,
     `toggles.test.ts:9`).
   - `index.ts` today also exports `Uuid` (a `string` alias, `index.ts:14`) and the full set
     of value/component interfaces (`Vec3`, `Vec4`, `Transform`, `Material`, `Mesh`,
     `Camera`, `DirectionalLight`, `PointLight`, `SpotLight`, `Name`, `Components`,
     `Envelope`, `AssetEntry`, `ProjectInfo`, …).
   Every type a consumer imports must survive the swap under the **same exported name and the
   same module path** (`@saffron/protocol` → `editor/src/protocol/index.ts`); a DTO-derived
   type whose generated name differs from the schema-derived `title` must be aliased back, or
   the importing call site migrated in the same change. Renaming any of these — or moving the
   module — is a breaking change and must be done as a stated, applied migration, not a silent
   drop.

2. **Decommission `gen-protocol.ts`.** Once `se-types.ts` is the source, the
   schema→TS path (`json-schema-to-typescript`, the `bun run gen:protocol` step in
   `editor/package.json`) is replaced by the DTO generator. Update the `check` / `build`
   scripts to run the DTO generator instead. (Schema files themselves retire in phase 5, not
   here — phase 4 only swaps the typegen source.)

### Type `call` both directions

3. **Widen `call<C>` to typed params.** Today `call<C extends keyof CommandResultMap>(cmd,
   params?: object)` takes **untyped** params (`client.ts:77-82`). Change to
   `call<C extends keyof CommandParamsMap>(cmd: C, params: CommandParamsMap[C]):
   Promise<CommandResultMap[C]>`. Optional-field DTOs (e.g. `new-project` `root?`,
   the `set-*` merge commands) become `Partial`-shaped params so conditional builds still
   typecheck.

4. **Delete `callRaw` and `client.raw`.** Remove `callRaw` (`client.ts:84-88`) and the
   `raw(cmd, params)` escape hatch (`client.ts:342-344`). Migrate every call site off them
   (worklist below).

### Migrate the call sites (the EDITOR map's worklist)

5. **Re-point the typed wrappers that wrongly use `callRaw`.** `get-project` / `new-project`
   / `open-project` / `save-project` / `load-project` are in `CommandResultMap` yet routed
   through `callRaw` with `as Promise<ProjectInfo>` casts — switch them to typed `call` and
   drop the casts (`new-project`'s conditional `root` needs the `Partial` params shape).

6. **Migrate the genuinely-raw call sites** (each with its params shape, from the editor map):
   `select {entity}`, `destroy-entity {entity}`, `deselect {}`, `set-transform
   {entity,...Partial<Transform>}`, `set-material {entity,...Partial<Material>}`,
   `add-component {entity,component}`, `remove-component {entity,component}`, `set-component
   {entity,component,json}`, `set-component-field {entity,component,field,value}`,
   `pick {u,v}→PickResult`, `gizmo-pointer {phase,x,y}`, `rename-asset {id,newName}→{id,name}`,
   `assign-asset {entity,slot,asset}`, `import-model {path}→EntityRef&{mesh,albedoTexture}`,
   `import-texture {path}→{texture}`, `set-aa {mode}→{aa}`, the toggles
   `set-clustered/ibl/ssao/contact-shadows/ssgi/shadows/depth-prepass/rt-shadows/restir
   {enabled}→` each its own per-command field (`{clustered}` / `{ibl}` / `{ssao}` /
   `{contactShadows}` / `{ssgi}` / `{shadows}` / `{depthPrepass}` / `{rtShadows}` / `{restir}`
   — the result key is per-command, not a uniform `flag`), `set-gi {mode}→{ddgi}`,
   `set-exposure {ev}→{exposureEv}`,
   `save-scene/load-scene {path}→{path}`, `screenshot {target,path}→{target,path,pending}`.
   Remove `pick`'s manual cast specifically (the editor map flags it).

   - **`add-entity` preset mismatch.** The editor's local `EntityPreset` union
     (`client.ts:68-74`) is `empty | cube | point-light | spot-light | directional-light |
     camera` — it is **missing `model`**, which the engine accepts on the wire (`add-entity`
     handler, `control_commands_scene.cpp:484,494` treat `cube` and `model` alike). The
     generated `CommandParamsMap['add-entity']` preset enum (from the phase-2 `Preset` DTO)
     **will** include `model`, so the editor's narrower union stays assignable and typechecks,
     but the editor can never send `model`. Either drop the hand-written `EntityPreset` in
     favour of the generated param type (so `model` becomes reachable) or add `model` to the
     local union — and note that `cube`/`model` are engine-synonyms, so a separate menu item
     may be redundant. Pick one in this phase; do not leave the local union silently diverged
     from the wire enum.

7. **The one `client.raw('viewport-native-info')` liveness probe**
   (`editor/src/panels/ViewportPanel.tsx:155`, awaited, result ignored) becomes a typed
   `call('viewport-native-info', {})`. (If viewport-presentation has already changed this
   command, follow that plan's shape.)

8. **The toggle boolean↔`{enabled}` seam.** The editor wrappers expose a boolean but the
   wire took `{enabled:0|1}`. With a typed `bool enabled` DTO param (phase 3) the wrapper
   passes the boolean straight through — confirm the generated TS param type is `boolean`,
   not `0|1`, so no wrapper coercion remains.

9. **Audit the Rust bridge against the generated param DTOs.** `editor/src-tauri/src/lib.rs`
   is a second hand-built wire consumer the typed `call<C>` does not cover: it builds JSON
   params for three control commands itself, outside the generated shapes. Confirm each still
   matches its phase-3 param DTO field-for-field:
   - `attach_native_viewport` (`lib.rs:257-264`) sends
     `{ parentXid: <u64-as-string>, x, y, width, height }` — must match the `attach-native-viewport`
     DTO (`WireUuid parentXid` + named-only `i32 x/y/width/height`); the bridge already
     stringifies the XID, matching `WireUuid`'s decimal-string wire form.
   - `resize_native_viewport` (`lib.rs:271-275`) sends `{ x, y, width, height }` — must match
     the `resize-native-viewport` named-only-geometry DTO.
   - the `auto_start` readiness probe calls `viewport-native-info` (`lib.rs:341`, no params)
     and `teardown` calls `quit` (`lib.rs:210`, no params) — both paramless, so the DTO param
     struct is empty; nothing to align beyond confirming the command names are unchanged.
   These five commands (`start_engine`, `attach`/`resize` viewport, `quit`,
   `viewport-native-info`/`engine_alive`) reach the engine through **dedicated Tauri commands**
   (`lib.rs:424-434` `invoke_handler`), not the generic `control(cmd, params)` passthrough the
   typed `client.call` rides — by design, because they bridge native window-handle / process
   lifecycle that React cannot express (see `editor/AGENTS.md` "control client is one generic
   passthrough" and `client.ts:1-5,346-361`). They are **explicitly carved out of the typed
   `client.call` surface**: the DTO contract still governs their *wire params* (the DTOs in
   phase 3), but their Rust-side construction is hand-checked here rather than generated,
   since Rust has no `@saffron/protocol` consumer. If `plans/viewport-presentation/` has
   already repurposed the `*-native-viewport` commands, follow that plan's param shapes and
   note the overlap in both plans.

## Validation

- `cd editor && bun run check` green: the generated `se-types.ts` typechecks and every call
  site compiles against `CommandParamsMap` / `CommandResultMap` with no `as` casts and no
  `callRaw` reference (grep `callRaw` / `.raw(` returns nothing in `editor/src`).
- `bun run build` (the `check.sh` frontend stage) green.
- Full editor smoke: select / inspect / set-transform / set-material / pick / gizmo / asset
  assign / import / project open-save / toggles all work through typed `call`.
- `tests/e2e` (`make e2e`) still green — its imports are `import type` only and the harness
  `call<T>` is loosely typed, so it is decoupled from `CommandResultMap`; confirm the
  `@saffron/protocol` alias still resolves after the artifact swap.
- `check.sh` green end to end (the regenerate-and-diff gate now covers `se-types.ts` too).

## Risks

- **Five commands silently on `callRaw`.** `get-project` and friends are in the map but cast
  through `callRaw`; deleting `callRaw` without re-pointing them is a compile error caught by
  `bun run check`, not a silent break — but they must be migrated, not just left.
- **`@saffron/protocol` single point of failure.** The e2e alias path-maps to
  `editor/src/protocol/index.ts` (`tests/e2e/tsconfig.json:12`); if the artifact moves or
  splits, both that tsconfig and the editor tsconfig need updating. Keep the filename to
  avoid it. e2e imports are type-only and `make e2e` has no typecheck step — type drift there
  is caught only by the editor's `bun run check`, so verify the editor check after the swap.
- **`Partial` params vs. required fields.** The merge `set-*` commands and `new-project`'s
  optional `root` need the generated params type to mark those fields optional, or conditional
  param construction in the editor stops typechecking. The DTO `std::optional` fields must map
  to `?`-optional TS fields.
- **se CLI untouched but field-name-fragile.** `tools/se` does not consume the protocol; its
  per-command text printers read result fields by string key with `value(key, default)`
  fallbacks (`tools/se/source/main.cpp`). Any field rename a DTO introduces would make a
  printer silently fall back — phase 5 audits printers against the manifest; phase 4 must not
  rename any result field relative to the pre-migration wire (the byte-identical checks in
  phases 2/3 already guarantee this).
