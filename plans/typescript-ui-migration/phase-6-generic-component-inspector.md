# Phase 6: Generic component inspector (schema-driven, all component types)

**Status:** NOT STARTED

<!-- Flip to COMPLETED when the "Done when" checklist passes, validation-clean. Delete this file only after COMPLETED + merged. -->

## Goal

Port the C++ registry-driven Inspector (`inspectorPanel`, `engine/source/saffron/editor/editor_panels.cpp:111-170`) to React as a **data-driven** panel: render every component's fields from the live `inspect` result keyed by the component schema (the discriminated union from `@saffron/protocol`, authored in phase-2 / generated in phase-3) using typed widgets, and support add/remove component plus partial-update writes. This is the React analog of the C++ `ComponentRegistry` (`scene.cppm:440-489`): the panel must NOT hardcode one block per component — it iterates whatever `inspect` returns and dispatches per-field by type, so a future engine-side `registerComponent` shows up with no React edit beyond a widget hint.

## Current state (verified)

The C++ inspector and the wire surface this phase consumes both exist on main; the React side is a stub.

### C++ inspector + registry (the source of truth being ported)
- `inspectorPanel` (`editor_panels.cpp:111-170`): bails on no/invalid selection (`:116-121`), iterates `ctx.registry.rows` (`:124`), skips rows the entity lacks (`traits.has`, `:126`), draws a collapsing header `propertyGridHeader(traits.name)` (`:131`, defined `ui.cppm:342`), shows a right-click "Remove component" **only if `traits.removable`** (`:132-139`), draws the body via `traits.drawInspector` (`:142`), then an "Add Component" popup listing rows the entity **lacks** (`:153-167`).
- `ComponentRegistry` / `ComponentTraits` (`scene.cppm:426-445`): each row has `name`, `removable`, and closures `has`/`addDefault`/`remove`/`serialize`/`deserialize`/`drawInspector`. `registerComponent<C>` (`scene.cppm:450-489`) synthesizes them; `removable` is the last arg.
- The 8 components and their **exact field widgets + JSON keys** are in `registerBuiltinComponents` (`editor_components.cpp:86-278`). This is the authoritative mapping the React widgets must reproduce:
  - **Name** (`:89-100`, **removable=false** `:100`): `name` — `InputText` (text).
  - **Transform** (`:102-127`, **removable=false** `:127`): `translation` (`vec3Control`, `:106`), `rotation` (**edited in DEGREES via `glm::degrees`/`glm::radians`, stored + serialized in RADIANS**, `:107-111`), `scale` (`vec3Control` reset 1.0, `:112`). JSON keys `translation`/`scale`/`rotation` each `{x,y,z}` (`:114-118`).
  - **Mesh** (`:129-141`, removable): `mesh` — `drawAssetPicker(AssetType::Mesh, ...)` (`:133`). JSON key `mesh` = bare `u64` (`:135`).
  - **Camera** (`:143-165`, removable): `fov` (`DragFloat` 1..179, `:147`), `near` (`:148`), `far` (`:149`), `primary` (`Checkbox`, `:150`). JSON keys `fov`/`near`/`far`/`primary` (`:154-155`).
  - **Material** (`:167-200`, removable): `baseColor` (`ColorEdit4`, vec4, `:171`), `albedoTexture` (`drawAssetPicker(AssetType::Texture, ...)`, `:172`), `metallic` (`SliderFloat 0..1`, `:173`), `roughness` (`SliderFloat 0..1`, `:174`), `emissive` (`ColorEdit3`, vec3, `:175`), `emissiveStrength` (`DragFloat 0..100`, `:176`), `unlit` (`Checkbox`, `:177`). JSON keys `baseColor` (`{x,y,z,w}`), `albedoTexture` (bare `u64`), `metallic`, `roughness`, `emissive` (`{x,y,z}`), `emissiveStrength`, `unlit` (`:181-187`).
  - **DirectionalLight** (`:202-225`, removable): `direction` (`DragFloat3`, `:206`), `color` (`ColorEdit3`, `:207`), `intensity` (`DragFloat 0..50`, `:208`), `ambient` (`DragFloat 0..1`, `:209`). JSON keys `direction`/`color` (`{x,y,z}`), `intensity`, `ambient` (`:213-215`).
  - **PointLight** (`:227-247`, removable): `color` (`ColorEdit3`, `:231`), `intensity` (`DragFloat 0..100`, `:232`), `range` (`DragFloat 0..200`, `:233`). JSON keys `color` (`{x,y,z}`), `intensity`, `range` (`:237-238`).
  - **SpotLight** (`:249-277`, removable): `direction` (`DragFloat3`, `:253`), `color` (`ColorEdit3`, `:254`), `intensity` (`DragFloat 0..100`, `:255`), `range` (`DragFloat 0..200`, `:256`), `innerAngle` (`DragFloat 0..89` **DEGREES**, `:257`), `outerAngle` (`DragFloat 0..89` **DEGREES**, `:258`). JSON keys `direction`/`color` (`{x,y,z}`), `intensity`, `range`, `innerAngle`, `outerAngle` (`:261-265`).
- `drawAssetPicker` (`editor_components.cpp:21-84`): a `BeginCombo` of catalog entries filtered by `AssetType`, with a `(none)`=`Uuid{0}` option and an `SE_ASSET` drag-drop target. The Mesh/albedo-texture combos in React come from phase-7's `AssetPicker`; phase-6 only needs the **field hook** so phase-7 can plug in.

### Component struct field types + defaults (`scene.cppm:28-103`)
- `NameComponent.name` (`:30`); `TransformComponent` `translation{0}`/`scale{1}`/`rotation{0}` (Euler XYZ **radians**, `:38-43`); `MeshComponent.mesh` Uuid (`:46-49`); `MaterialComponent` `baseColor{1}`/`albedoTexture`/`metallic=0`/`roughness=1`/`emissive{0}`/`emissiveStrength=1`/`unlit=false` (`:54-63`); `CameraComponent` `fov=45`/`nearPlane=0.1`/`farPlane=100`/`primary=true` (`:66-72`); `DirectionalLightComponent` `direction{-0.5,-1,-0.3}`/`color{1}`/`intensity=1`/`ambient=0.15` (`:76-82`); `PointLightComponent` `color{1}`/`intensity=5`/`range=10` (`:86-91`); `SpotLightComponent` `direction{0,-1,0}`/`color{1}`/`intensity=5`/`range=10`/`innerAngle=20`/`outerAngle=30` **degrees** (`:95-103`). Defaults matter: the schema/widget hints should carry `metallic` min 0 / max 1 etc. matching these `SliderFloat`/`DragFloat` bounds.

### Wire surface (commands this phase calls — all already on main, no new commands)
- **`inspect {entity}`** (`control_commands_scene.cpp:326-345`): returns `{ id, name, components: { [Name]: DTO } }` — a discriminated union keyed on the registry name; `entityRef` adds `id`/`name` (`:342`). This is the **per-component-per-entity** read the panel renders.
- **`set-component {entity, component, json}`** (`:117-138`): routes the body through `row->deserialize`, so the wire shape is **identical to scene files**. NOTE: `set-component` does **NOT merge** — `deserialize` rewrites the whole component from `body` (each `fromJson` reads every key with a default, e.g. `editor_components.cpp:120-126`). For a true partial update the client must **read-modify-write** (fetch current from `inspect`/store, overlay the changed field, send the full DTO). `set-transform` and `set-material` are the only server-side merge helpers.
- **`set-transform {entity, translation?, rotation?, scale?}`** (`:142-171`): server-side **merge** over the current transform (`:161-164`); `rotation` is `{x,y,z}` Euler **radians** on the wire. Returns `entityRef`.
- **`set-material`** (`:175-242`): server-side merge of `baseColor?{x,y,z,w}` / `albedoTexture?` (accepts a string uuid, coerces to u64 `:200-207`) / `metallic?` / `roughness?` / `emissive?{x,y,z}` / `emissiveStrength?` / `unlit?` (`:195-235`).
- **`set-light`** writes **DirectionalLight fields only** (`control_commands_scene.cpp:246`, per the digest); point/spot `range`/`innerAngle`/`outerAngle` and any light field must go through `set-component` (read-modify-write), **not** `set-light`. Spot angles are **degrees** on the wire (the struct stores degrees, `scene.cppm:101-102`).
- **`add-component {entity, component}`** (`:71-91`) and **`remove-component {entity, component}`** (`:93-113`, rejects non-removable with an error `:107-110`). The schema's `removable` flag (from phase-2's `dump-schema`/component schema) gates the React Remove button before the call.
- **`set-component-field {entity, component, field, assetId}`** (phase-2): the Uuid-field assign used by phase-7 drag-drop; phase-6 wires the field renderer to call it for asset Uuid fields when phase-7 lands.
- JSON vec helpers fix the exact shapes: `vec3ToJson`→`{x,y,z}` (`scene.cppm:339-347`), `vec4ToJson`→`{x,y,z,w}` (`:350-358`).
- **HAZARD (digest-confirmed):** every `Uuid` is a `u64` (`core.cppm:51-53`) emitted as a raw number > 2^53 (`Mesh.mesh`/`Material.albedoTexture` are bare `u64`). It is **string** end-to-end in `@saffron/protocol` and must never be parsed as a JS `number`. The inspector's Uuid fields read/write strings.

### React side (what exists / is assumed from earlier phases)
- MVP `editor/src/main.tsx` (worktree `wt:editor/src/main.tsx`) hardcodes a **Transform-only** inspector for a single `Cube` entity. The reusable primitives to lift:
  - `VectorEditor` (`wt:main.tsx:374-444`): pointer-capture drag-scrub on the axis label (`clientX` delta × `step`, `:392-414`), numeric `<input>` with `stopPropagation` on pointer-down (`:437`), `formatNumber` 3-dp (`:446-451`). This is the Vec3 widget.
  - `queueTransform` (`wt:main.tsx:104-127`): buffers the latest partial transform, throttles to ≥4ms/invoke, tracks sent/completed/in-flight counters. This is the write-coalescing primitive — but in the MVP it calls the bespoke `set_cube_transform` shim; phase-6 uses the **generic coalesced-write helper** `editor/src/control/coalesce.ts` (built in phase-3) over the typed client.
- Phase-3 deliverables this phase **depends on** and reuses: `editor/src/control/client.ts` (typed `call<R>` + per-command methods, ids as strings), `editor/src/control/coalesce.ts` (generic coalesced write), the Zustand store `editor/src/state/store.ts` (`selectedId`, `sceneVersion`, `selectionVersion`, `componentsBySelected`), the version-stamped reconcile poll (re-`inspect`s the selection only when `selectionVersion`/`sceneVersion` changed and is gated OFF during an active drag), and `editor/src/protocol/` (generated from `schemas/control`, incl. the component discriminated union + per-component `removable`/field metadata).
- Phase-5 deliverable this phase depends on: `HierarchyPanel` + selection round-trip already set `store.selectedId`. Phase-6 reads it; it does not own selection.

**Depends on:** phase-5 (Hierarchy + selection round-trip). Transitively requires phase-2 (`dump-schema` + the component schema with `removable` + field metadata) and phase-3 (typed client, `coalesce.ts`, store, reconcile poll, generated `@saffron/protocol`).

## Implementation

All paths under `/var/home/saffronjam/repos/SaffronEngine`. No engine/C++ changes and **no new control commands** — this phase is React + the typed client only.

### 1. Field metadata: schema-driven widget selection (`editor/src/components/fieldRenderer.tsx`)
Build the dispatcher that maps a `(component, field, value)` to a widget. Drive it primarily from the generated component schema (`@saffron/protocol`), falling back to value-shape inference so an unknown component still renders.

- Define a `FieldKind` union: `vec3 | vec4 | color3 | color4 | number | slider | bool | text | uuid`.
- Resolve `FieldKind` in priority order:
  1. A per-`(component,field)` **override table** `FIELD_HINTS` in `fieldRenderer.tsx` that mirrors the C++ widgets exactly (the table IS the parity contract — keep it explicit, not inferred):
     - `Transform.rotation` → `vec3` **with `unit: "deg"`** (UI degrees, wire radians); `Transform.translation`/`scale` → `vec3`.
     - `Material.baseColor` → `color4`; `Material.emissive` → `color3`; `Material.metallic`/`roughness` → `slider` `{min:0,max:1,step:0.01}`; `Material.emissiveStrength` → `number` `{min:0,max:100,step:0.05}`; `Material.unlit` → `bool`; `Material.albedoTexture` → `uuid` (`AssetType.Texture`).
     - `Mesh.mesh` → `uuid` (`AssetType.Mesh`).
     - `Camera.fov` → `number` `{min:1,max:179,step:0.5}`; `Camera.near`/`far` → `number`; `Camera.primary` → `bool`.
     - `*Light.color` → `color3`; `*Light.direction` → `vec3`; `*Light.intensity`/`range`/`ambient` → `number` (bounds per the C++ DragFloat ranges above); `SpotLight.innerAngle`/`outerAngle` → `number` `{min:0,max:89,step:0.1}` **with `unit: "deg"`** (already degrees on the wire — `unit:"deg"` here means "no conversion, just a degree label/clamp", distinct from Transform.rotation which converts).
     - `Name.name` → `text`.
  2. Otherwise consult the generated schema's field type (`number`→`number`, `boolean`→`bool`, `string`→`text`, an object with `{x,y,z}`→`vec3`, `{x,y,z,w}`→`vec4`).
  3. Otherwise default `text` (render as a read-only JSON string so an unmapped field is visible, not lost).
- **Units rule (the 57× bug guard):** ONLY `Transform.rotation` converts (UI `glm.degrees`, wire `glm.radians`). The `FieldHint.unit:"deg"` flag drives a `radToDeg`/`degToRad` pair at the field boundary. `SpotLight` angles do NOT convert (they are degrees on both sides). Encode this so it can't be confused: a `convertRadians: boolean` on the hint, true only for `Transform.rotation`.
- Export `renderField(component, field, value, ctx)` returning the right widget element wired to an `onChange(newValue)` that funnels into the panel's write path (section 4).

### 2. Widgets (`editor/src/components/`)
Port/parameterize the MVP primitives. Keep them dumb (value + onChange); the panel owns coalescing.
- `VectorEditor.tsx`: generalize the MVP `VectorEditor` (`wt:main.tsx:374-444`) to N axes (3 or 4) with `axes: readonly string[]`, `step`, optional per-axis label, and the existing pointer-capture drag-scrub + numeric input + `formatNumber`. Used for `vec3`/`vec4`. Add an optional `displayTransform`/`storeTransform` pair so `Transform.rotation` shows degrees while the value flows in radians (or convert in `fieldRenderer` before/after — pick one place; prefer `fieldRenderer` so `VectorEditor` stays unit-agnostic).
- `NumberDrag.tsx`: a single-axis drag-scrub number with `min`/`max`/`step`, clamped, used for `number` and (with a track) `slider`. Reuse the `VectorEditor` drag math for one axis.
- `ColorField.tsx`: a color swatch + hex/RGB input mapping to `{x,y,z}` (color3) or `{x,y,z,w}` (color4) floats in 0..1 (matching `ColorEdit3`/`ColorEdit4`). Use `<input type="color">` for the RGB picker; expose alpha separately for color4 (baseColor). All channels are linear floats on the wire (no gamma conversion — match ImGui's default `ColorEdit` linear behavior used by the C++ panel).
- `ComboField.tsx`: a generic enum/string combo (used for any future enum field; not strictly needed by the 8 components but required by the "extensible without per-component code" goal). For `uuid` fields render a placeholder slot that phase-7's `AssetPicker.tsx` fills (a thumbnail combo + drag-drop). Until phase-7 lands, render the raw Uuid string + a `(none)` clear button so the field is editable.
- All numeric widgets format with the MVP `formatNumber` (3 dp) and `stopPropagation` on the input pointer-down so dragging the label scrubs but typing in the box doesn't.

### 3. Inspector panel (`editor/src/panels/InspectorPanel.tsx`)
The React port of `inspectorPanel` (`editor_panels.cpp:111-170`), fully data-driven.
- Read `selectedId` + `componentsBySelected` from the store. If no selection / invalid → render "No entity selected" (matches `:118`).
- The components come from the store (kept fresh by the phase-3 reconcile poll calling `client.inspect(selectedId)` when `selectionVersion`/`sceneVersion` changes). Do **not** poll inside the panel.
- Iterate the `components` object **in registry order** (the schema carries the canonical order; fall back to insertion order of the `inspect` result). For each present component:
  - A collapsing header showing the component name (port of `propertyGridHeader`, `ui.cppm:342`).
  - A header context/overflow menu with **Remove** shown only when the schema's `removable` is true for that component (`Name`/`Transform` are not removable — gate exactly like `editor_panels.cpp:132`). Remove calls `client.removeComponent(selectedId, name)` then bumps a local refetch (the poll will re-`inspect`).
  - The body: for each field in the component's DTO, call `renderField(name, field, value, ...)` from section 1.
- Below the list: an **Add Component** button → popover listing every registered component the entity **lacks** (port of `:153-167`; the registered set comes from the schema / `list-components`). Selecting one calls `client.addComponent(selectedId, name)`.
- Keep the panel free of any per-component `switch`: adding a component engine-side (a new `registerComponent`) must surface automatically once the schema is regenerated, with a `text` fallback if no `FIELD_HINTS` entry exists yet.

### 4. Write path: read-modify-write + coalescing (`InspectorPanel` + `editor/src/control/client.ts`)
Because `set-component` rewrites the whole component (`control_commands_scene.cpp:131-137`, no merge), every field edit must send the **complete** component DTO with the one field changed.
- On a field `onChange(newValue)`:
  1. Optimistically update the local component DTO in the store (so the UI is responsive and the next poll tick doesn't fight the edit — the poll is gated OFF mid-drag per phase-3).
  2. Build the full DTO (current store DTO + the changed field, with `Transform.rotation` converted back to radians).
  3. Route the write:
     - **Transform** → `client.setTransform(id, { translation?, rotation?, scale? })` (server merges; send only the changed sub-vector; rotation in **radians**). This is the cleanest path and avoids the no-merge gotcha.
     - **Material** → `client.setMaterial(id, Partial<MaterialC>)` (server merges; `albedoTexture` as a string uuid is accepted, `control_commands_scene.cpp:200-207`).
     - **Every other component** (incl. Point/Spot/Directional light `range`/angles/etc.) → `client.setComponent(id, name, fullDto)` (full DTO, **not** `set-light` for point/spot fields).
  4. Funnel high-frequency edits (drag-scrub, sliders) through the generic `coalesce.ts` helper keyed on `(id, component, field-group)` so a drag emits ≤ one write per ~4ms (port of `queueTransform`'s throttle/coalesce, `wt:main.tsx:104-127`), tracking in-flight counts.
- Add typed client methods (in `client.ts`, against `CommandResultMap`) if not already present from earlier phases: `setComponent(id, component, dto)`, `addComponent(id, component)`, `removeComponent(id, component)`, `setTransform(id, Partial<TransformC>)`, `setMaterial(id, Partial<MaterialC>)`, `setComponentField(id, component, field, assetId)`. All ids/uuids are **strings**; always **named** params (never positional, matching `command.cppm:50-61` `positionalOr` accepting `params[name]`).

### 5. Store wiring (`editor/src/state/store.ts`)
- Ensure `componentsBySelected: InspectResult["components"] | null` and an `applyOptimisticField(component, field, value)` action exist for the optimistic update in 4(i). The reconcile poll already writes `componentsBySelected` on a fresh `inspect`; the optimistic action overlays between polls. On a new `selectionVersion` the optimistic overlay is dropped (server is authoritative).
- No new poll logic here — phase-3 owns the version-stamped reconcile loop; phase-6 only adds the optimistic-overlay action and the drag-gate flag read (`isDragging` → poll skips re-`inspect` of the selection).

### 6. Verify against `se`
- `bun run check` (tsc strict, must pass against the generated `@saffron/protocol`).
- Toolbox: build + run the engine headless host (`SAFFRON_EDITOR_NATIVE_VIEWPORT=1 ... ./build/debug/bin/SaffronEditor`) and drive parity checks from a second terminal with `se inspect` / `se set-component` to confirm the inspector reflects external edits within the poll interval and that React edits round-trip (`se inspect` shows the new values).

## Done when

- [ ] Selecting any entity in the hierarchy shows **all** its components (every present row from `inspect`) with correct field widgets — verified across an entity carrying Name+Transform+Mesh+Material and a Point/Spot/Directional light + a Camera.
- [ ] Editing Transform **translation** via drag-scrub moves the entity live; **radians on the wire, degrees in the UI**, with NO 57× error (set rotation to 90° in the UI → `se inspect` shows `rotation ≈ 1.5708` rad).
- [ ] Material `baseColor` (ColorField, vec4), `metallic`/`roughness` sliders (0..1), `emissive` (color3), `emissiveStrength`, and `unlit` (checkbox) all edit and persist (confirmed via `se inspect` and visibly in the viewport).
- [ ] **Add Component** lists only components the entity lacks and adds the chosen one; **Remove** is shown only for `removable` components and removes it; `Name` and `Transform` show no Remove (parity with `editor_panels.cpp:132` / `:161`).
- [ ] Point/Spot `range` + `innerAngle`/`outerAngle` (**degrees**, 0..89) edit via `set-component` (not `set-light`) and render correctly (spot cone narrows/widens as expected).
- [ ] Editing the same entity from a separate `se set-component`/`se set-transform` terminal is reflected in the inspector within the poll interval (reconcile poll re-`inspect`s on `selectionVersion`/`sceneVersion`).
- [ ] The panel contains **no per-component `switch`**: temporarily adding a dummy field to a component's schema renders it via the value-shape fallback with no React code change beyond regenerating `@saffron/protocol`.
- [ ] High-frequency drags are coalesced (the in-flight counter never unbounded-grows during a sustained drag) and the UI stays smooth; the reconcile poll does not clobber an in-progress drag.
- [ ] `bun run check` passes; uuids are strings end-to-end (no `number` parse of `Mesh.mesh`/`Material.albedoTexture`).

## Risks / seams

- **Degrees/radians + spot-angle-degrees mismatch.** The single conversion (`Transform.rotation` only) is the highest-risk parity item; the `convertRadians` hint flag isolates it. Spot `innerAngle`/`outerAngle` are degrees on both sides — do NOT convert them. Verify both explicitly in the checklist.
- **`set-component` is not a merge** (`control_commands_scene.cpp:131-137`). The read-modify-write (full DTO every field write) is mandatory for non-Transform/non-Material components, or a single-field edit will reset the rest of the component to its `fromJson` defaults. Transform/Material use the server-merge helpers and avoid this.
- **Schema-driven rendering must tolerate the extensible component family** without per-component code; the `FIELD_HINTS` table is the deliberate, explicit parity contract for the 8 known components, the schema/value-shape inference is the fallback for anything new. Keep the fallback rendering (never drop an unmapped field).
- **Write/read race during drags vs the reconcile poll.** Phase-3 gates the poll OFF during an active drag and the optimistic overlay covers the gap; the overlay is dropped on a new `selectionVersion` so the server stays authoritative. A stale overlay after an external edit is the failure mode to watch.
- **Uuid-as-string.** Asset Uuid fields (`Mesh.mesh`, `Material.albedoTexture`) are `u64` > 2^53 (`core.cppm:51-53`); any accidental `Number(...)` corrupts them. The `uuid` `FieldKind` keeps them as strings and defers the real picker UI to phase-7's `AssetPicker` (this phase only provides the field slot + a `(none)` clear).
- **Seam for phase-7:** the `uuid` field renderer is the hand-off point — phase-7 swaps the placeholder string slot for the thumbnail `AssetPicker` + `SE_ASSET` drag-drop target calling `assignAsset`/`set-component-field`, with no change to `InspectorPanel` or `fieldRenderer`'s dispatch.
