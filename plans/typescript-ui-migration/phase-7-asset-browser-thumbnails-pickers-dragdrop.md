# Phase 7: Asset browser (thumbnails over the wire) + pickers + drag-drop + import

**Status:** IN PROGRESS — implemented & `bun run check`/`build` + cargo (dialog plugin) green; thumbnails-over-socket / drag-drop / import / View-modal pending interactive (display) verification

<!-- Flip to COMPLETED when the "Done when" checklist passes, validation-clean. Delete this file only after COMPLETED + merged. -->

**Done so far (2026-06-01):** shadcn-based asset browser. Rust: `tauri-plugin-dialog` 2.7.1 added (`.plugin(tauri_plugin_dialog::init())` + `dialog:default` capability), cargo build green. React: `AssetTile` (lazy 128px socket thumbnail with a module-level blob-URL cache + in-flight dedupe + invalidate-on-load; inline rename; HTML5 drag source `application/x-se-asset`), `AssetsPanel` (tile grid + Import via `@tauri-apps/plugin-dialog` `open()` + OS file-drop via `onDragDropEvent`), `AssetPicker` (Popover thumbnail combo wired into the inspector's uuid fields — Mesh.mesh/Material.albedoTexture→`assign-asset`, others→`set-component-field`), `AssetViewer` (shadcn Dialog 512 preview). **Occlusion:** the View modal sets a `viewportHidden` flag → `ViewportPanel` parks the native window off-screen (`-10000,-10000,1×1`) while open and restores on close. `bun run check` + `bun run build` green. (Divergence: `import-model` spawns+selects an entity, matching `se import-model`.)

## Goal

Port the C++ ImGui **Assets** panel to the Tauri/React editor at full parity: a tile
grid with **real thumbnails fetched over the socket** (mesh render / texture image / type
icon), **in-place rename**, **asset pickers** (a thumbnail combo per `Uuid` field —
`Mesh.mesh`, `Material.albedoTexture`, `Environment.skyTexture`), **drag-drop** assigning a
tile into an inspector field (matching the C++ `SE_ASSET` semantics), **import** via a Tauri
file dialog, and an optional **View** preview modal. Everything flows through the typed client
(`editor/src/control/client.ts`) and the generated protocol types — no new control commands
are added here (they all landed in phase-2).

**Depends on:** phase-6 (the generic inspector — this phase wires the asset pickers into the
inspector's `Uuid` fields and the environment panel's sky-texture field; the `ComboField`/
`AssetPicker` widget the inspector renders for `Uuid` fields is delivered here).

## Current state (verified)

### C++ Assets panel (the parity target)
- `assetCatalogPanel` — `engine/source/saffron/editor/editor_panels.cpp:226-325`. A 72px tile
  grid (`tileSize = 72.0f`, `editor_panels.cpp:246`), column count from
  `ImGui::GetContentRegionAvail().x / cellWidth` (`:248`). Per tile: a thumbnail `ImGui::Image`
  if `thumbnailFor(entry) != 0` (`:265-268`), else a placeholder `Button("MESH"/"TEX"/"?")`
  (`:271-281`); **in-place rename** via `ImGui::InputText("##name", &entry.name)` (`:310`);
  double-click / context-menu **View** → `onView(entry)` (`:282-298`); the tile is a
  **drag-drop SOURCE** setting payload `"SE_ASSET"` = `AssetDragPayload{ entry.id.value,
  entry.type }` (`:301-307`). An **Import...** button opens `drawImportModal` (`:232-236`).
  Empty-catalog placeholder text at `:241`.
- `AssetDragPayload` — `engine/source/saffron/editor/editor_context.cppm:63-67`:
  `{ u64 id; AssetType type = AssetType::Mesh; }`.
- `drawAssetPicker` (the picker combo + drag-drop **TARGET**) —
  `engine/source/saffron/editor/editor_components.cpp:21-84`. Combo lists `(none)` + every
  catalog entry whose `entry.type == type`, each row showing a 16px thumbnail + name
  (`:45-67`); selecting writes `target = entry.id`. The combo is also a `BeginDragDropTarget`
  accepting `"SE_ASSET"` and writing `target = Uuid{ drag->id }` **only when `drag->type ==
  type`** (`:72-83`). Used by the Mesh picker (`editor_components.cpp:133`), the Albedo picker
  (`:172`), and the sky-texture picker (`editor_panels.cpp:211`).
- `viewerPanel` (the View modal) — `editor_panels.cpp:327-351`. A floating window showing a
  single square `ImGui::Image(previewId, side×side)` (`:336-343`).
- The wiring lives in `engine/source/saffron/editorapp/editor_app.cppm`:
  - `thumbnailFor` closure (`:111-149`): per-asset, cached in `state->thumbnails`
    (`:114-118`). Texture → `loadTextureAsset` (`:121`); Mesh → `loadMeshAsset` +
    `renderMeshThumbnail(app.renderer, mesh, 128)` (`:132-138`); else the type SVG icon
    (`:148`). SVG type icons (`box.svg`/`image.svg`/`file.svg`/`eye.svg`) loaded via
    `uploadSvgIcon` (`:90-104`).
  - `onView` closure (`:152-184`): re-renders a mesh at size **512** (`renderMeshThumbnail(...,
    512)`, `:165`) or shows the texture; sets `state->viewer.{previewId,title,open}`.
  - `importToCatalog` closure (`:188-205`): routes `.png/.jpg/.jpeg` → `importTexture`, else →
    `importModel`. Bound to File ▸ Import, the panel button, and **file-drop**
    (`app.window.onFileDropped`, `:249-253`).
  - On project load (`onLoadProject`, `:235-247`) all cached thumbnails are dropped so they
    re-generate — the **TS analog: clear the client thumbnail cache on `loadProject`/
    `loadScene`** (phase-8 resets the store; this phase's cache must subscribe to that).

### Engine asset catalog + commands (the wire surface — all already present)
- `AssetEntry` — `engine/source/saffron/scene/scene.cppm:118-124`: `{ Uuid id; std::string
  name; AssetType type; std::string path; }`. `AssetType { Mesh, Texture, Other }` (`:116`).
  **`Uuid.value` is a `u64`** (`core.cppm:51-53`) → **string on the wire, never a JS number.**
- `list-assets` — `engine/source/saffron/control/control_commands_asset.cpp:63-73`. Returns
  `{ assets: [ { id, name, type:"mesh"|"texture"|"other", path } ] }` (`id` emitted raw `u64`).
- `rename-asset {id|name, newName}` — `control_commands_asset.cpp:75-94`. Returns `{ id, name }`.
- `assign-asset {entity, slot:mesh|albedo, id|name}` — `control_commands_asset.cpp:96-140`.
  Adds the component if missing, writes `MeshComponent.mesh` (slot `mesh`) or
  `MaterialComponent.albedoTexture` (slot `albedo`). **Only covers mesh + albedo** — the sky
  texture (`Environment.skyTexture`) and any other `Uuid` field need `set-component-field`
  (phase-2) or `set-environment`.
- `import-model {path}` — `control_commands_asset.cpp:24-43`. **Spawns** an entity (selected)
  and returns `entityRef` + `{ mesh, albedoTexture }`. For a **catalog-only** import (parity
  with `importToCatalog`, no auto-spawn) the browser uses `import-texture` for images; for
  models the engine's `import-model` always spawns. (See **Risks** — phase-2 may add a
  no-spawn variant; if absent, the browser imports a model by calling `import-model` and the
  spawned entity is acceptable parity with File-drop which does NOT spawn — confirm against the
  phase-2 decision; default here: call `import-model` and let it spawn, matching the `se`
  behavior, and the new entity simply shows in the hierarchy.)
- `import-texture {path}` — `control_commands_asset.cpp:47-61`. Returns `{ texture: <u64> }`.
  No spawn.
- `assetTypeName`/`assetTypeFromName` — `engine/source/saffron/assets/assets.cppm:49-61`
  (`mesh|texture|other`).
- **Phase-2 commands this phase consumes** (must exist before starting):
  - `get-thumbnail { assetId, size }` → `{ format:"png", base64, width, height }` (GPU→CPU
    readback modeled on `captureViewport`/`writeBufferToPng`,
    `engine/source/saffron/rendering/renderer_capture.cpp:38-103`; `renderMeshThumbnail`
    currently returns `Ref<GpuTexture>` not bytes, `renderer_types.cppm:1045`).
  - `set-component-field { entity, component, field, assetId }` — assigns any `Uuid` field
    (needed for the sky texture + future `Uuid` fields beyond mesh/albedo).
  - `view-asset` (or `get-thumbnail` @512) for the View modal preview.

### MVP frontend (load-bearing primitives to reuse, NOT asset code — none exists)
- `wt:editor/src/main.tsx` (457 lines) has **no** asset panel, picker, drag-drop, or import UI.
- The **drag-scrub** primitive `VectorEditor` (`wt:main.tsx:385-444`) and the
  **write-coalescing** `queueTransform` (`wt:main.tsx:104-144`) are reused by the inspector
  (phase-6), not directly here, but the same `client.call<R>` envelope + Zustand store from
  phase-3 are the substrate.
- The MVP capability set is `["core:default"]` only
  (`wt:editor/src-tauri/capabilities/default.json`) and Cargo has **no** dialog plugin — this
  phase adds `tauri-plugin-dialog` + its permission (phase-3 may have added it for menus;
  confirm and add if missing).
- The MVP stack: React 19.2, Vite 7.2, TS 5.9, `lucide-react` 0.468, `@tauri-apps/api` 2.8
  (`wt:editor/package.json`). No new runtime deps needed beyond `@tauri-apps/plugin-dialog`.

### Shared types (generated, schema-first — see crossCutting)
- `AssetEntry` + `Thumbnail` schemas authored in phase-2 (`schemas/control/`), generated into
  `editor/src/protocol/`. `AssetEntry.id` and all `Uuid` fields are **`string`**. This phase
  adds the UI-only `AssetDragPayload` TS type (the React DnD payload, not a wire DTO).

## Implementation

Ordered. All engine-side work is done (phase-1/2); this phase is **frontend + capabilities
only**. Files under `editor/src/`.

### 1. Typed client methods (`editor/src/control/client.ts`)
Add (named params, ids as strings, against `CommandResultMap`):
- `listAssets(): Promise<AssetEntry[]>` → `control("list-assets",{})` then `.assets`.
- `getThumbnail(assetId: string, size: number): Promise<Thumbnail>` →
  `control("get-thumbnail", { assetId, size })`.
- `renameAsset(asset: string, name: string): Promise<{id:string,name:string}>` →
  `control("rename-asset", { asset, name })`.
- `assignAsset(entity: string, slot: "mesh"|"albedo", asset: string)` →
  `control("assign-asset", { entity, slot, asset })`.
- `setComponentField(entity: string, component: string, field: string, assetId: string)` →
  `control("set-component-field", { entity, component, field, assetId })` (sky texture + any
  non-mesh/albedo `Uuid` field).
- `importModel(path: string): Promise<EntityRef & {mesh:string,albedoTexture:string}>` →
  `control("import-model", { path })`.
- `importTexture(path: string): Promise<{texture:string}>` →
  `control("import-texture", { path })`.
- `viewAsset(assetId: string): Promise<Thumbnail>` (or reuse `getThumbnail(assetId, 512)`).

Each surfaces `ok:false` as a typed rejection (phase-3 envelope). All `id`/`assetId`/`asset`
params are **strings**; never coerce to `number`.

### 2. Asset state + thumbnail cache (`editor/src/state/store.ts`)
- Add `assets: AssetEntry[]` to the Zustand store, refreshed by the reconcile poll **only when
  `sceneVersion` changes** (asset imports/loads bump `sceneVersion`; parity with the hierarchy
  poll diffing — see crossCutting). Add `refreshAssets()` that calls `client.listAssets()` and
  sets `assets`.
- Add a module-level **thumbnail cache** `Map<string, { url: string; size: number }>` keyed by
  `assetId` (NOT in Zustand — it holds blob URLs and must survive re-renders without triggering
  store churn). Provide `getThumbnailUrl(assetId, size): Promise<string>`:
  - cache hit (same or larger cached size) → return the cached blob URL;
  - miss → `client.getThumbnail(assetId, size)`, decode `base64` → `Blob` →
    `URL.createObjectURL`, store, return.
- Add `invalidateThumbnails()` that `URL.revokeObjectURL`s every entry and clears the map. Wire
  it to the `loadProject`/`loadScene` store-reset (phase-8) **and** to a `rename`/`import`
  refresh so stale thumbnails regenerate (parity with `editor_app.cppm:235-240`). Renaming does
  **not** invalidate thumbnails (the image is unchanged) — only re-fetch the asset list.

Base64→Blob helper (no `Buffer` in the webview):
```ts
function base64ToBlob(b64: string, mime = "image/png"): Blob {
  const bin = atob(b64);
  const bytes = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i);
  return new Blob([bytes], { type: mime });
}
```

### 3. `editor/src/components/AssetTile.tsx`
- Props: `entry: AssetEntry`, `onView(entry)`, `selected?`, thumbnail size (default 72px to
  match `tileSize`).
- On mount / `entry.id` change: `getThumbnailUrl(entry.id, 128)` (render at 128 like
  `editor_app.cppm:138`, display at 72) into local state; show a lucide type-icon placeholder
  (`Box` for mesh, `Image` for texture, `File` otherwise — matching the C++ SVG icons) until it
  resolves, and **fall back to the icon on a getThumbnail rejection** (parity with the
  `*Icon.id` fallbacks in `thumbnailFor`).
- **Rename in place**: a controlled `<input>` under the tile; commit on blur / Enter →
  `client.renameAsset(entry.id, value)` then `refreshAssets()`. Stop pointer propagation on the
  input so a drag on the tile doesn't start while editing (parity with the MVP numeric-input
  `stopPropagation`, `wt:main.tsx`).
- **Drag SOURCE**: `draggable`, on `dragstart` set the DnD payload via
  `e.dataTransfer.setData("application/x-se-asset", JSON.stringify({ id: entry.id, type:
  entry.type }))` — the TS analog of `SE_ASSET`/`AssetDragPayload`. Use the HTML5 DnD API (the
  webview's native DnD; do not invent a custom pointer-drag here — the inspector targets are
  DOM `dragover`/`drop` targets). Also set `e.dataTransfer.effectAllowed = "copy"`.
- **View**: double-click → `onView(entry)`.

### 4. `editor/src/panels/AssetsPanel.tsx`
- Import button → opens the Tauri file dialog (`@tauri-apps/plugin-dialog` `open`) filtered to
  models + images; on a chosen path call `client.importModel(path)` or `client.importTexture`
  routed by extension (parity with `importToCatalog`, `editor_app.cppm:188-205`:
  `.png/.jpg/.jpeg` → texture, else model), then `refreshAssets()`.
- A responsive CSS-grid tile layout (`grid-template-columns: repeat(auto-fill, minmax(72px,
  1fr))`) over `store.assets`, each an `<AssetTile>`. Empty state mirrors
  `editor_panels.cpp:241` ("No assets yet — import or drag-and-drop a model or texture.").
- A panel-level **drop zone**: `onDragOver`/`onDrop` for OS file drops → import (parity with
  `onFileDropped`). Tauri file-drop also arrives via the `tauri://drag-drop` window event;
  subscribe in `ViewportPanel`/`App` and route to import (phase-3 may own the window event —
  reuse it here). Guard against the webview's default navigation on drop.
- `onView(entry)` opens `<AssetViewer>` (step 6).

### 5. `editor/src/components/AssetPicker.tsx` (the `ComboField` for `Uuid` fields)
This is the React `drawAssetPicker`. The inspector (phase-6) renders it for any `Uuid` field; it
is also used by the Environment panel (phase-8) for the sky texture.
- Props: `value: string` (current `Uuid`, `"0"` = none), `assetType: "mesh"|"texture"`,
  `onChange(assetId: string)`, optional `label`.
- A combo/dropdown listing `(none)` + every `store.assets` entry whose `entry.type ===
  assetType` (parity with the type filter at `editor_components.cpp:47-49`), each row showing a
  16px thumbnail (`getThumbnailUrl(id, 64)` displayed at 16) + name. Selecting calls
  `onChange(entry.id)`; `(none)` calls `onChange("0")`.
- Show the current value's thumbnail + name in the collapsed combo (look up `store.assets` by
  `value`; if not found show "(none)").
- **Drop TARGET**: `onDragOver` (call `preventDefault` so drop fires) + `onDrop` reading
  `application/x-se-asset`; **accept only when the dragged `type === assetType`** (parity with
  `editor_components.cpp:77`), then `onChange(payload.id)`. Add a visual highlight on
  `dragenter`/`dragleave`.
- The picker is **field-agnostic** about the write: it calls `onChange`. The caller wires the
  write:
  - `Mesh.mesh` / `Material.albedoTexture` → `client.assignAsset(entity,
    "mesh"|"albedo", assetId)` (the dedicated, minimal command).
  - `Environment.skyTexture` and any other `Uuid` field → `client.setComponentField(...)` or
    `client.setEnvironment({ skyTexture: assetId })` (phase-8). The inspector's generic
    `fieldRenderer` (phase-6) maps a `Uuid`-typed schema field to `<AssetPicker>` with
    `assetType` derived from the field (mesh field → `mesh`, albedo/sky/texture field →
    `texture`) and `onChange` → `assignAsset` for mesh/albedo else `setComponentField`.

### 6. `editor/src/components/AssetViewer.tsx` (the View modal)
- A modal (DOM overlay) showing a single large square preview from
  `client.viewAsset(entry.id)` / `getThumbnail(entry.id, 512)` (parity with the 512 re-render at
  `editor_app.cppm:165`), title = asset name (parity with `viewerPanel`,
  `editor_panels.cpp:327-351`).
- **CRITICAL occlusion rule** (crossCutting `loadingOverlayDesign`): the reparented X11 child
  window **always paints over its rect** and the webview cannot draw on top of it. The modal
  MUST therefore either (a) render **outside** the viewport's native rect, or (b) temporarily
  **lower/unmap** the native window while open. Default: render the modal centered but **clamped
  to the non-viewport region** (the panels area), OR send `resize-native-viewport` to shrink the
  native window off-screen while the modal is open and restore on close. Pick (a) if the
  panel layout leaves enough non-viewport space; document the chosen approach in the modal.
  Revoke the blob URL on close.

### 7. Capabilities + plugin (`editor/src-tauri/`)
- Add `tauri-plugin-dialog` to `editor/src-tauri/Cargo.toml` (if phase-3 didn't already) and
  register it in `lib.rs` (`.plugin(tauri_plugin_dialog::init())`).
- Add `"dialog:default"` (and `"dialog:allow-open"`) to
  `editor/src-tauri/capabilities/default.json` `permissions` (the MVP has only `core:default`).
- No new Rust command (file dialog is a frontend plugin call; import goes through the generic
  `control(cmd, params)` passthrough from phase-3).

### 8. `se` / docs
No new control commands in this phase, so no `tools/se/source/main.cpp` change is required.
The asset commands this phase consumes already have `se` formatters (`list-assets` at
`tools/se/source/main.cpp:155`) and phase-2 added `get-thumbnail`/`set-component-field`/
`view-asset` to `se` + docs. **Verify** those `se` formatters + the
`docs/content/reference/control-commands.md` rows exist (phase-2 owns them); add nothing here
unless a gap surfaces.

## Done when
- [ ] The Assets panel lists **every** catalog entry (`client.listAssets()` reflected in the
      store; re-fetched only on `sceneVersion` change, no per-tick churn).
- [ ] Each tile shows a **correct thumbnail**: a mesh shows its 3D render, a texture shows its
      image, an `other`/failed asset shows the type icon (mesh `Box`, texture `Image`, else
      `File`).
- [ ] Thumbnails are **cached client-side** (a blob-URL cache; no per-frame / per-render
      re-fetch — verified by network/log: one `get-thumbnail` per asset per size).
- [ ] Renaming a tile (input commit) persists via `rename-asset` and survives a project
      reload (`se rename-asset` from a terminal also reflects after the next poll).
- [ ] A **Mesh** picker (inspector `Mesh.mesh`) and a **Material albedo** picker lists matching
      assets with thumbnails; choosing one updates the entity and is **visible in the embedded
      viewport** (via `assign-asset`).
- [ ] An **Environment sky-texture** picker assigns a texture via `set-component-field` /
      `set-environment` (used by phase-8's panel; the picker widget itself works here).
- [ ] **Drag-drop** a tile onto a Mesh / albedo / sky-texture field assigns it; a type-mismatch
      drag is **rejected** (e.g. dragging a texture onto the Mesh field does nothing — parity
      with the C++ type guard).
- [ ] **Import** opens a Tauri file dialog; importing a model and a texture each add the entry
      to the catalog (visible in the panel + in the matching picker) after `refreshAssets()`.
- [ ] OS file-drop onto the panel imports the file (parity with `onFileDropped`).
- [ ] The **View** modal shows a 512 preview and is **not occluded** by the X11 child window
      (rendered outside the native rect or with the native window lowered).
- [ ] `bun run check` passes; ids are typed `string` end-to-end (no `Uuid` typed as `number`).

## Risks / seams
- **Thumbnail latency/throughput over the socket.** `get-thumbnail` does a GPU→CPU readback +
  PNG encode per call (phase-2; off the present path). Mitigate with the blob-URL cache (step 2),
  lazy-load on tile mount, and a single size per use (128 grid / 16-from-64 combo / 512 view).
  If the poll stalls behind a thumbnail batch, fetch thumbnails **outside** the reconcile poll
  (a separate fire-and-forget queue) so the per-tick `get-selection`/`render-stats` stay cheap.
- **View modal occlusion by the X11 window** (the core viewport-bridge constraint): the webview
  cannot paint over the native child. The modal must live outside the native rect or lower the
  native window while open (step 6) — same rule as every overlapping element (dropdowns,
  context menus). The `AssetPicker` combo dropdown has the same hazard if it opens over the
  viewport rect; keep pickers in the inspector/environment panels (left/side docks), away from
  the viewport center, so their dropdowns never overlap the native window.
- **Drag-drop semantics must match C++ `SE_ASSET`** (`editor_components.cpp:72-83`): type-gated
  accept, mesh/albedo via `assign-asset`, everything else (sky texture, future `Uuid` fields)
  via `set-component-field`. Use a distinct MIME (`application/x-se-asset`) so OS file-drop and
  asset-tile-drop are distinguishable in the same `drop` handler.
- **Import model spawns an entity.** `import-model` always spawns + selects
  (`control_commands_asset.cpp:37-39`), unlike the C++ catalog-only `importToCatalog`
  (`editor_app.cppm:201`). If phase-2 did not add a no-spawn variant, accept the spawn (it
  matches `se import-model`) and note the divergence; the entity simply appears in the
  hierarchy. Revisit only if catalog-only import is required for parity.
- **Cache invalidation on load.** `loadProject`/`loadScene` change the catalog; the blob-URL
  cache MUST be invalidated (revoke URLs, clear map) or stale thumbnails persist (parity with
  `editor_app.cppm:235-240`). Wire `invalidateThumbnails()` into the phase-8 store reset.

## Notes
- **No engine changes** in this phase — all required commands (`list-assets`, `rename-asset`,
  `assign-asset`, `import-model`, `import-texture`, plus phase-2's `get-thumbnail`,
  `set-component-field`, `view-asset`) exist. This is purely the React asset surface + the
  Tauri dialog capability.
- **Non-goal (parity):** the C++ editor has no asset folders/tags/search and no per-asset
  metadata beyond name/type/path — do not add them here.
- The `AssetPicker` widget delivered here is the `ComboField`/`AssetPicker` the phase-6
  inspector references for `Uuid` fields; if phase-6 stubbed it, this phase replaces the stub.
