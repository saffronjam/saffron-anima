# Phase 5: Hierarchy panel + Create menu

**Status:** NOT STARTED

<!-- Flip to COMPLETED when the "Done when" checklist passes, validation-clean. Delete this file only after COMPLETED + merged. -->

## Goal

Build the first real data panel of the TypeScript/Tauri editor: a **Hierarchy** list (list / select / copy / delete) and a **Create** menu (every preset), driven entirely by the typed control client from phase-3 and the new editor commands from phase-2. Wire **bidirectional selection** so a hierarchy-row click selects the entity in-engine and a viewport ray-pick / billboard-pick (phase-4) highlights the matching row. This is the React analog of the C++ `hierarchyPanel` (`editor_panels.cpp:19-109`) and the Create paths in `editor_panels.cpp` + the menu bar — no engine behavior changes here; this phase consumes the surface phases 1-4 built.

## Current state (verified)

The C++ ImGui Hierarchy + Create paths this phase replaces (all on `main`, in `editor-old/` after phase-1's move):

- **Hierarchy panel** — `engine/source/saffron/editor/editor_panels.cpp:19-109` (`hierarchyPanel`):
  - Lists entities via `forEach<IdComponent, NameComponent>(ctx.scene, …)` (`editor_panels.cpp:66-82`); `forEach` is templated at `scene.cppm:288`.
  - Row is `ImGui::Selectable(name.name.c_str(), isSelected)` keyed by `id.id.value` (`editor_panels.cpp:69-74`); selection compares `entity.handle == ctx.selected.handle` (`:70`) and calls `setSelection(ctx, entity)` (`:73`).
  - Right-click context item gives **Copy** and **Delete** (`editor_panels.cpp:75-80`); structural changes are deferred to after the iteration (`:84-106`).
  - **Copy** is a deep-copy through the `ComponentRegistry`: it `createEntity(…, name + " (copy)")` then for each `ctx.registry.rows` row that `has` the source, `addDefault` + `deserialize(serialize(...))` (`editor_panels.cpp:85-98`). This is exactly the semantics phase-2's `copy-entity` command mirrors.
  - **Delete** clears selection if the deleted entity was selected, then `destroyEntity(ctx.scene, toDelete)` (`editor_panels.cpp:99-106`).
  - **No inline rename** — there is no name edit in the hierarchy; rename is the Inspector's Name field (phase-6). Match this.
- **Create / "Add +" presets** — `editor_panels.cpp:23-62` (the `"AddEntityPreset"` popup): **Empty** (`createEntity` "Entity"), **Model** (`ctx.onCreateCube()`), **Point Light** (`createEntity` + `addComponent<PointLightComponent>` + translation `{0,2,0}`), **Spot Light** (+ `SpotLightComponent`, translation `{0,4,0}`), **Directional Light** (+ `DirectionalLightComponent`), **Camera** (+ `CameraComponent`). The menu-bar **Create** menu (`editor_panels.cpp:392`) adds a **Cube** entry calling the same `ctx.onCreateCube`.
- **`onCreateCube`** (`editor_context.cppm:58`, set in `editor_app.cppm:208-218`) imports the bundled cube (`importModel(... assetPath("models/cube.gltf"))`) into the catalog then `spawnModel(ctx.scene, "Cube", *cube)` (`assets.cppm:387`) and selects it. This is the in-process spawn that phase-2's `add-entity{preset:model|cube}` reuses.
- **Selection model** — `setSelection(ctx, entity)` sets `ctx.selected` and publishes `ctx.onSelectionChanged` (a `SubscriberList<Entity>`, `editor_context.cppm:44`; impl `editor_context.cpp:16-20`). `ctx.selected.handle == entt::null` ⇒ nothing selected. `newEditorContext` seeds Camera + Sun and selects Camera (`editor_context.cpp:30-40`).

Control commands this phase relies on (the **command surface is provided by earlier phases — this phase adds NO new commands**):

- **`list-entities`** (`control_commands_scene.cpp:23-33`): `forEach<IdComponent, NameComponent>` → `{ entities: [ { id: <u64>, name }, … ] }`. **HAZARD:** `id` is the raw `IdComponent.id.value` `u64` (`core.cppm:51-53`), emitted as an unsigned integer that exceeds `Number.MAX_SAFE_INTEGER` — it MUST be a **string** end-to-end in TS (see `@saffron/protocol` from phase-2/3; never a JS `number`).
- **`select`** (`control_commands_scene.cpp:292-302`): `resolveEntity` (accepts uuid or name, `control_server.cpp:72-132`) → `setSelection(ctx.editor, *entity)` → returns `entityRef` (`{id,name}`, `control_server.cpp:134-138`).
- **`destroy-entity`** (`control_commands_scene.cpp:54-69`): resolves, clears selection if selected, `destroyEntity` → `{ destroyed: <u64> }`.
- **`add-entity{preset}`** (NEW in phase-2) → `entityRef`; presets `empty|cube|model|point-light|spot-light|directional-light|camera`, reusing the in-process spawn incl. the bundled-cube import. Replaces the C++ "Add +" / Create menu / `onCreateCube`.
- **`copy-entity{entity}`** (NEW in phase-2) → `entityRef`; deep-copy mirroring `editor_panels.cpp:85-98`.
- **`get-selection`** (NEW in phase-2) → `{ entity: EntityRef|null, selectionVersion: <int> }`; frame-stamped `selectionVersion` bumps on every `setSelection`.
- **`sceneVersion`** (NEW in phase-2) — a counter bumped on entity create/destroy/copy, surfaced in `get-selection` and/or `render-stats`, so the hierarchy poll **diffs** instead of re-fetching the full list every tick.
- **`deselect`** (NEW in phase-2) → clears selection (empty-space pick / Escape).

Frontend baseline (MVP, worktree `editor/src/main.tsx`, branch `explore-ui`):

- The MVP has **no hierarchy** — the sidebar hardcodes one entity (`'Cube'` fallback) with a Transform-only inspector. There is no entity list, no selection model, no polling.
- Load-bearing MVP mechanics to **reuse** (already ported into the phase-3 skeleton): the focus-gated version-stamped **reconcile poll** (`store.ts`), the coalesced-write helper (`coalesce.ts`, ported from `queueTransform` at `main.tsx:104-127`), and the Zustand store fields `entities`, `selectedId`, `sceneVersion`, `selectionVersion`, `engineStatus` (`store.ts` from phase-3).
- Tauri I/O goes through the generic `control(cmd, params)` passthrough (phase-3) and the typed `client.ts`; **never** `invoke()` of a bespoke shim (the 8 MVP shims at `lib.rs:219-388` are gone).

**Depends on:** phase-4 (the viewport-pick → `get-selection` round-trip and the visible billboards that make lights/cameras selectable), which depends on phase-3 (Tauri/React skeleton + typed `client.ts` + Zustand store + reconcile poll) and phase-2 (`add-entity` / `copy-entity` / `get-selection` / `deselect` / `sceneVersion` commands + `@saffron/protocol` types). No engine or CMake work in this phase.

## Implementation

All new code is TypeScript under `editor/src/`. No engine, no CMake, no new control commands, no shaders, no env vars.

### 1. Store: entity list + selection diffing (`editor/src/state/store.ts`)

Extend the phase-3 Zustand store (do not duplicate fields already added there):

- State (confirm/extend): `entities: EntityRef[]`, `selectedId: string | null`, `sceneVersion: number`, `selectionVersion: number`. `EntityRef` is the generated `@saffron/protocol` type `{ id: string; name: string }` — `id` is a **string** (u64-as-string).
- Actions:
  - `setEntities(entities: EntityRef[])` — replace the list.
  - `setSelectedId(id: string | null)` — optimistic local selection (set immediately on a hierarchy click so the UI feels instant).
  - `setSceneVersion(v) / setSelectionVersion(v)` — version bookkeeping for the poll.
- The reconcile poll (already in `store.ts` from phase-3) must, per tick:
  1. call `client.getSelection()` (cheap) → read `selectionVersion` + `entity`. If `selectionVersion` changed, update `selectedId` (engine wins — this is the viewport-pick → row-highlight round-trip from phase-4).
  2. read `sceneVersion` (from `get-selection` and/or `render-stats`). **Only** when `sceneVersion` changed, call `client.listEntities()` and `setEntities(...)`. Do NOT call `listEntities` every tick (`risks` below; matches `doneWhen` "only re-fetches the full list when sceneVersion changed").
- Gate the engine-wins selection update OFF while a hierarchy structural op is locally in-flight is unnecessary here (selection ops are idempotent and cheap); but DO let `selectionVersion` be authoritative so a `se select` from a separate terminal reflects in the UI.

### 2. Typed client methods (`editor/src/control/client.ts`)

Add thin typed wrappers over the phase-3 generic `call<R>(cmd, params)` (all params **named**, never positional; `ok:false` already surfaces as a typed rejection):

- `listEntities(): Promise<EntityRef[]>` → `call<{entities: EntityRef[]}>("list-entities", {})` then `.entities` (the schema/codegen guarantees `id` is a string).
- `select(id: string): Promise<EntityRef>` → `call("select", { entity: id })`.
- `deselect(): Promise<void>` → `call("deselect", {})`.
- `destroyEntity(id: string): Promise<{ destroyed: string }>` → `call("destroy-entity", { entity: id })`.
- `copyEntity(id: string): Promise<EntityRef>` → `call("copy-entity", { entity: id })`.
- `addEntity(preset: AddEntityPreset): Promise<EntityRef>` → `call("add-entity", { preset })`.
- `getSelection(): Promise<Selection>` (if not already added in phase-3's poll) → `call("get-selection", {})`.

`AddEntityPreset` is the generated union type from `@saffron/protocol`: `"empty" | "cube" | "model" | "point-light" | "spot-light" | "directional-light" | "camera"`. Use the exact preset strings the phase-2 `add-entity` command accepts; do not invent new ones.

After any structural op (`addEntity`/`copyEntity`/`destroyEntity`), select the returned entity locally (`store.setSelectedId(ref.id)`) and let the next poll tick reconcile `sceneVersion` → refresh the list. Optionally `store.setEntities` optimistically for snappier UX, but the `sceneVersion`-diff poll is the source of truth.

### 3. HierarchyPanel (`editor/src/panels/HierarchyPanel.tsx`)

- Reads `entities` + `selectedId` from the store (selector subscriptions, not the whole store, to avoid re-render churn).
- Renders one row per `EntityRef`. Row key = `entity.id` (string). Highlight when `entity.id === selectedId`.
- **Left-click** a row → optimistic `store.setSelectedId(entity.id)` + `client.select(entity.id)`. This is the C++ `Selectable` + `setSelection` path (`editor_panels.cpp:71-74`).
- **Right-click** a row → a context menu (a small `<ul>`/popover; see overlay note in Risks — a hierarchy context menu is in the webview, NOT over the X11 viewport, so a normal HTML menu is fine) with:
  - **Copy** → `client.copyEntity(entity.id)` then select the returned dup.
  - **Delete** → `client.destroyEntity(entity.id)`; if the deleted id was `selectedId`, clear it (`store.setSelectedId(null)`). Matches `editor_panels.cpp:99-106`.
- **No inline rename** — match the C++ editor (rename lives in the phase-6 Inspector Name field). Do not add an editable row label.
- Empty state: when `entities.length === 0`, show a muted "No entities" placeholder.
- A header **"+"** button (and/or right-click empty space) opens the Create menu (component below). This mirrors the C++ "Add +" button at `editor_panels.cpp:23`.

### 4. CreateMenu (`editor/src/app/CreateMenu.tsx`)

- A reusable menu component listing every preset, wired to `client.addEntity(preset)`:
  - **Empty** → `"empty"`
  - **Cube** → `"cube"` (or `"model"` — use whichever phase-2 mapped the bundled-cube spawn to; the C++ "Model"/"Cube" entries both call `onCreateCube`, so pick the single canonical preset string phase-2 defined and document it here)
  - **Point Light** → `"point-light"`
  - **Spot Light** → `"spot-light"`
  - **Directional Light** → `"directional-light"`
  - **Camera** → `"camera"`
- Used in two places (single source of truth): the Hierarchy header "+" button (this phase) and the top menu bar **Create** menu (phase-8 `MenuBar.tsx` imports the same component). Keep the preset list defined once (e.g. an exported `const CREATE_PRESETS: { label: string; preset: AddEntityPreset }[]`).
- On click: `await client.addEntity(preset)` → `store.setSelectedId(ref.id)`. The poll's `sceneVersion` diff refreshes the list and the new entity appears. Lights/cameras become **visible + pickable** in the viewport via the phase-4 billboards.

### 5. Bidirectional selection (consume phase-4)

- **Hierarchy → engine:** handled by the row-click `client.select(id)` above.
- **Engine → hierarchy:** the reconcile poll's `getSelection()` (step 1) updates `selectedId` whenever `selectionVersion` changes. So a viewport ray-pick / billboard-pick (phase-4) or an external `se select` flips the highlighted row within the poll interval. No extra wiring beyond honoring `selectionVersion` in the poll.
- **Empty-space deselect:** phase-4's pick path calls `deselect` engine-side on an empty hit; `selectionVersion` bumps and the poll clears `selectedId`. The hierarchy reflects "nothing selected" with no row highlighted.

### 6. Wire panels into the layout

- Mount `HierarchyPanel` in the app shell (the phase-3 `App.tsx` / phase-9 `Layout.tsx` slot — left column, matching the C++ default DockBuilder "Hierarchy left"). For this phase a simple fixed slot is fine; phase-9 makes it dockable/resizable.
- `bun run check` must pass with the generated `@saffron/protocol` types (ids as strings throughout — no `number` for any id).

## Done when

- [ ] The Hierarchy panel lists **all** entities from `list-entities`; each row shows the entity name; the selected row is visibly highlighted.
- [ ] Clicking a hierarchy row selects that entity in-engine (`se inspect` / the phase-6 Inspector follows the selection) and highlights the row.
- [ ] Right-click **Copy** duplicates the entity (a new "… (copy)"-style entity appears, with all components — verify via `se inspect` diff of source vs copy); right-click **Delete** removes it (and clears selection if it was selected).
- [ ] The **Create** menu spawns **every** preset (Empty / Cube / Point Light / Spot Light / Directional Light / Camera); each new entity appears in the hierarchy AND in the embedded viewport (lights/cameras visible via the phase-4 billboards).
- [ ] A viewport **ray-pick** (mesh) or **billboard-pick** (light/camera) highlights the matching hierarchy row within the poll interval; clicking empty space deselects (no row highlighted).
- [ ] An `se select <id>` / `se pick` from a **separate terminal** updates the highlighted hierarchy row within the poll interval (selectionVersion-driven).
- [ ] The hierarchy **only** re-fetches the full entity list when `sceneVersion` changes — confirmed by observing that an idle scene issues no `list-entities` calls per tick (e.g. log/inspect the poll), only the cheap `get-selection` + `render-stats`.
- [ ] All entity `id`s are handled as **strings** end-to-end (no `Number` coercion of a u64 anywhere in the hierarchy/create path); `bun run check` passes against `@saffron/protocol`.
- [ ] No inline rename exists in the hierarchy (parity with the C++ editor; rename is deferred to the phase-6 Inspector Name field).

## Risks / seams

- **Hierarchy poll churn during rapid create/delete** — mitigated by `sceneVersion` diffing: the poll re-fetches the list only on a version change, not per tick. If a burst of creates lands between ticks, the next `sceneVersion` bump catches all of them in one `list-entities` (the list is a full snapshot, so no per-entity diff is needed beyond the version gate).
- **Selection round-trip latency vs the poll interval** — a hierarchy click sets `selectedId` optimistically *and* fires `select`, so the UI never waits on a round-trip; the poll only confirms / corrects. A viewport pick has no optimistic local source, so its row-highlight lags by up to one poll interval (~100-200ms at 5-10Hz) — acceptable, and stated in `doneWhen` ("within the poll interval").
- **Optimistic-vs-authoritative selection conflict** — if a local click and an engine-side selection change race, `selectionVersion` is authoritative: always let a *newer* `selectionVersion` from `get-selection` overwrite the local optimistic `selectedId`. Never let a stale poll response clobber a just-set local selection — compare `selectionVersion` and only apply if it advanced.
- **Context menu placement** — the hierarchy right-click menu lives in the webview DOM (the Hierarchy panel is NOT over the reparented X11 viewport rect), so a normal absolutely-positioned HTML popover is fine here. (The cross-cutting overlay constraint — elements cannot be drawn over the native viewport window — applies only to popovers that would overlap the viewport rect; not relevant to the left-column Hierarchy.)
- **Preset string drift** — the `AddEntityPreset` union and the C++ `add-entity` preset switch (phase-2) must agree exactly. They are kept in sync by the schema-first pipeline: the union comes from `@saffron/protocol` generated from `schemas/control/`, validated against live `se add-entity` output by the phase-2 contract test (`tools/check-control-schema`). If a preset is added engine-side, the schema + generated union update and the Create menu's `CREATE_PRESETS` list is the one hand-maintained surface — keep it minimal.
- **Non-goal (parity-correct):** no scene-graph parenting / nesting in the hierarchy (the C++ editor has none — the list is flat via `forEach`, `editor_panels.cpp:66`). Do not build a tree; a flat list is parity. Parenting/`resolveRefs` is an explicit migration non-goal.
