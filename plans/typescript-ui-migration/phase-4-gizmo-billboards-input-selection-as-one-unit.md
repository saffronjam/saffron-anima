# Phase 4: Viewport interaction unit — native gizmo overlay + billboards + input forwarding + ray-pick selection round-trip

**Status:** IN PROGRESS — engine + React implemented & build-verified; visible gizmo/billboards/drag pending interactive (display) verification

<!-- Flip to COMPLETED when the "Done when" checklist passes, validation-clean. Delete this file only after COMPLETED + merged. -->

**Done so far (2026-06-01):** Engine builds validation-clean (`-j1`, all steps); `bin/shaders/gizmo_overlay.spv` emitted. Forward-ported the overlay graphics pipeline (`newOverlayPipeline`, 1× samples, alpha blend, no depth, empty layout, `OffscreenColorFormat`), `OverlayVertex`/`OverlayState`, `makeMappedVertexBuffer`/`submitOverlay`, and the `editor-overlay` RgPass appended after `addTonemapPass` (`colors.push_back`, loadOp Load into the 1× resolved `sceneColor`). Refactored the pure-math gizmo (`viewportProject`/`gizmoAxes`/`handleAxis`/`hitNativeGizmo`/`applyNativeGizmoDrag`/`pointSegmentDistance` + `syncNativeGizmo`) into `Saffron.Editor` so the new `gizmo-pointer` command shares it; the OverlayVertex builders (`buildNativeGizmo`/`buildEditorBillboards`/`submitNativeGizmo`/`handleNativeGizmoPointer`) stay in `Saffron.EditorApp`. `NativeGizmoState` + enums on `EditorContext`, mode/space synced from the phase-2 `GizmoOp`/`GizmoSpace`. Billboards are colored overlay glyphs (point=filled box, spot=box+cone line, camera=box outline). `pick` extended to test billboards first (`kind:"billboard"|"mesh"`); `gizmo-pointer {phase,x,y NDC}` command. React: `Topbar` (T/R/S + world/local → `set-gizmo`), `ViewportPanel` pointer handlers (left-click → `pick`; drag → coalesced `gizmo-pointer`), `gizmo` store slice; `bun run check` green. **Headless-verified:** the overlay RgPass executes every frame in present-only mode with **no validation errors across off/msaa4/taa/fxaa** (the mandatory MSAA-ordering test); `gizmo-pointer` hover/begin/drag/end + the extended `pick` (`kind=mesh`) respond + exit clean. **Pending (needs a display):** the visible gizmo handles + billboard glyphs + an actual handle-drag manipulating the transform in the attached webview viewport, and the Topbar/ViewportPanel interaction round-trip.

## Goal

Deliver every in-viewport interaction as one coherent, end-to-end testable unit, because none of these
pieces is useful in isolation under present-only mode (ImGui is skipped, so the C++ ImGui gizmo +
billboards never draw):

1. Forward-port the engine-rendered **native gizmo overlay** (T/R/S handles drawn through a new overlay
   graphics pipeline into the post-tonemap scene color).
2. Re-render **light/camera/empty billboards** through that same overlay pipeline so non-mesh entities are
   visible and pickable in the embedded viewport.
3. Add the **gizmo + camera input model** chosen by the phase-3 spike-0b: command-driven `gizmo-pointer`
   (NDC) by default, with raw SDL pointer forwarding as the optimization.
4. **Round-trip selection**: a viewport ray-pick or billboard-pick updates React `store.selectedId`, gated on
   `selectionVersion`; empty space deselects.
5. A **Topbar** gizmo group (T/R/S + world/local) wired to `set-gizmo`, reflected from `get-gizmo`.

This unit makes the embedded viewport an actual editor surface (select, see handles, manipulate) rather than
a passive picture.

**Depends on:** phase-3 (Tauri/React skeleton + generic `control()` passthrough + typed client + Zustand
store + auto-start/attach + the resolved input-routing decision from spike-0b). Transitively depends on
phase-1 (present-only bridge, `editor-old/` move, `OverlayState`/`presentViewportToSwapchain` are NOT yet on
main — they are forward-ported across phases 1 and 4) and phase-2 (`get-selection` + `selectionVersion`,
`get-gizmo`/`set-gizmo` with **local-space support**, `add-entity`).

## Current state (verified)

### Overlay/gizmo render path does NOT exist on main; it lives only in the worktree

The MVP commit `faf704d` (worktree `/var/home/saffronjam/wt/SaffronEngine.explore-ui`, branch `explore-ui`)
added the overlay pipeline + native gizmo. None of it is on main HEAD. The pieces to forward-port:

- **`OverlayVertex` + `OverlayState`** — worktree `renderer_types.cppm` diff adds (after `RenderStats` at
  main `renderer_types.cppm:459`):
  ```cpp
  struct OverlayVertex { glm::vec2 position; glm::vec4 color; };
  struct OverlayState {
      std::vector<OverlayVertex> vertices;
      std::array<Ref<Buffer>, MaxFramesInFlight> buffers;
      std::array<u32, MaxFramesInFlight> capacity{};
  };
  ```
  Plus `Ref<Pipeline> overlay;` in `Pipelines` (main `renderer_types.cppm` `Pipelines` struct, alongside
  `fxaa`/`cull`), an `OverlayState overlay;` member in `Renderer`, a `bool presentViewportOnly = false;`
  member, and the decls `setPresentViewportOnly`, `newOverlayPipeline`, `submitOverlay`.
- **`newOverlayPipeline`** — worktree `renderer_pipelines.cpp` diff (appended after the depth-prepass pipeline
  at main `renderer_pipelines.cpp:186`): loads `shaders/gizmo_overlay.spv`, vertex input = `OverlayVertex`
  (loc 0 `eR32G32Sfloat` position, loc 1 `eR32G32B32A32Sfloat` color), triangle list, **cull none, depth test
  + write OFF, alpha blend (srcAlpha / oneMinusSrcAlpha), `rasterizationSamples = e1`**, color attachment
  format `OffscreenColorFormat` (`renderer_types.cppm:34` = `eR16G16B16A16Sfloat`), empty pipeline layout (no
  descriptors — vertices carry color), dynamic viewport+scissor.
- **`makeMappedVertexBuffer` + `submitOverlay` + the `editor-overlay` RgPass** — worktree `renderer.cppm` diff:
  a host-mapped `VMA_MEMORY_USAGE_AUTO` + `HOST_ACCESS_SEQUENTIAL_WRITE | MAPPED` vertex buffer; `submitOverlay`
  stashes the vertex vector into `renderer.overlay.vertices`; the pass is appended **inside `beginFrameGraph`
  immediately after `addTonemapPass(renderer, graph)`** (main `renderer.cppm:1423`), guarded by
  `!renderer.overlay.vertices.empty() && renderer.pipelines.overlay`, `loadOp::eLoad` into
  `renderer.graph.sceneColor`, grows/maps the per-frame buffer on demand, `memcpy`s, sets viewport/scissor to
  `targets.offscreen.extent`, binds + draws `vertexCount`.
- **`gizmo_overlay.slang`** — the worktree placed it at `editor-old/assets/shaders/gizmo_overlay.slang` (26
  lines, trivial passthrough vertex `float4(position,0,1)` + color, fragment returns color). After phase-1's
  `editor/` → `editor-old/` move, that is the dir `SaffronEditor` compiles shaders from
  (`saffron_compile_shaders` GLOBs `*.slang`).

> **MRT conflict (must rewrite, do not copy).** The worktree RgPass uses the old single-attachment field
> `overlay.color = RgAttachment{...}`. On main `RgPass` has `std::vector<RgAttachment> colors;`
> (`render_graph.cppm:75`) — the MRT migration already landed (see existing
> `scene.colors.push_back(...)` at `renderer.cppm:1305`, `ui.colors.push_back(...)` at `renderer.cppm:1467`).
> The forward-port MUST be `overlay.colors.push_back(RgAttachment{ renderer.graph.sceneColor,
> vk::AttachmentLoadOp::eLoad, vk::AttachmentStoreOp::eStore, {} });`.

### Sample-count: the overlay legitimately runs at 1x (verified, not a bug)

Main `beginFrameGraph` (`renderer.cppm:690-721`) routes the scene through `msaaColor` (multisampled) → resolves
into the 1x `offscreen` (`renderer.cppm:712`, `sceneColorAtt.resolve = sceneOutput` at `:1303`); FXAA/TAA render
to `scratch` then a compute pass writes back to `renderer.graph.sceneColor` (the 1x offscreen, `:1332`,`:1363`);
`addTonemapPass` (`:1436`) runs a compute pass on `renderer.graph.sceneColor` (1x offscreen). So by the time the
`editor-overlay` pass runs (appended after tonemap), `graph.sceneColor` is **always the 1x offscreen** regardless
of AA mode. The overlay pipeline's hardcoded `rasterizationSamples = e1` is therefore correct. The remaining MSAA
risk is purely **pass ordering** (overlay must be appended after every pass that writes `sceneColor`, including the
TAA/SSGI writeback) — it is, since it follows `addTonemapPass`, the last `sceneColor` writer.

### Native gizmo geometry + hit-test + input live in the worktree editor_app, NOT main

Worktree `editor_app.cppm` diff (the ~400-line block inserted at main `editor_app.cppm:72`, after the
`EditorState` struct) adds these free functions in `namespace se`:
`GizmoProjection`, `viewportProject`, `pixelToNdc`, `cameraPosition`, `addTriangle`, `addLine`, `addBox`,
`pointSegmentDistance`, `axisColor`, `gizmoAxes(transform, space)` (world = identity axes, local = `quat *
basis`), `handleAxis`, `hitNativeGizmo` (axis/plane/uniform pixel hit-test), `applyNativeGizmoDrag` (translate
along projected axis/plane, rotate by `(dx+dy)*0.01`, scale per-axis/uniform), `handleNativeGizmoPointer`
(SDL_EVENT_MOUSE_MOTION/BUTTON_DOWN/UP → hover/drag, and on a miss does `pickEntity` + `setSelection`), and
`submitNativeGizmo` (builds the overlay vertex list for the active mode and calls `submitOverlay`).

The worktree's native `onUi` branch (verified at worktree `editor_app.cppm` diff, around lines 720-740) only
calls `renderScene` + `submitNativeGizmo` — it does **NOT** draw billboards and does **NOT** ray-pick in
`onUi`; pick is socket-driven (the demo `click-viewport`) or via `handleNativeGizmoPointer`'s SDL event sink.

> **`click-viewport` is a demo, NOT to be forward-ported as-is.** Both the worktree
> `handleNativeGizmoPointer` (the BUTTON_DOWN miss branch) and the worktree `click-viewport` command toggle the
> hit entity's `MaterialComponent.baseColor` to a yellow highlight as a visual stand-in for selection. Replace
> that with a clean `pickEntity` + `setSelection` (no material mutation). Phase-2 already replaced
> `click-viewport` with `pick` + explicit `set-material`; this phase relies on the plain `pick` command
> (`control_commands_scene.cpp:304`) and the phase-2 `billboard-pick`.

### Native gizmo state struct — must extend main's EditorContext

Worktree `editor_context.cppm` diff adds (before `EditorContext`): `enum class NativeGizmoMode {Translate,
Rotate, Scale}`, `enum class NativeGizmoSpace {World, Local}`, `enum class NativeGizmoHandle {None,X,Y,Z,XY,YZ,
XZ,Screen,Uniform}`, `struct NativeGizmoState { mode, space, hovered, active, dragging, startMouse,
startTranslation, startRotation, startScale, Entity target }`, and a `NativeGizmoState nativeGizmo;` member on
`EditorContext`. On main, `EditorContext` ends at `gizmoOp = ImGuizmo::TRANSLATE;` (`editor_context.cppm:59`) —
insert `nativeGizmo` right after, and the enums/struct before `EditorContext` (`editor_context.cppm:39`).

> **Phase-2 unified the gizmo state.** Phase-2's `get-gizmo`/`set-gizmo{op,space}` is the single source of
> gizmo state and added **local-space** to both `drawGizmo` (`editor_gizmo.cpp:49` currently hardcodes
> `ImGuizmo::WORLD`) and the native gizmo space field; the worktree `set-gizmo-mode`/`set-gizmo-space`
> commands were aliased/dropped. This phase wires the **Topbar** to that single family and drives the native
> overlay's `nativeGizmo.mode`/`.space` from it. Confirm during implementation that phase-2 stored gizmo state
> in a form the native overlay reads (either directly on `NativeGizmoState`, or `drawGizmo`'s `gizmoOp` +
> a `space` field mapped to `nativeGizmo`). If phase-2 only updated `gizmoOp`, add the
> `NativeGizmoMode/Space ↔ ImGuizmo::OPERATION/space` mapping here so both paths share one state.

### Billboard parity hole (verified)

`drawEditorBillboards` (`editor_gizmo.cpp:71-118`) draws PointLight/SpotLight/Camera icons via
`ImGui::GetWindowDrawList()->AddImage(icon, ...)` and returns the clicked entity — it is the **only** way to
see and click non-mesh entities (`pickEntity` only hits mesh world-AABBs, `assets.cppm:665`). It runs inside
`ImGui::Begin("Viewport")` in main's non-native `onUi` (`editor_app.cppm:316-325`). In present-only mode ImGui
is skipped (worktree `editor_app.cppm` native `onUi` returns before the panels), so lights/cameras/empties are
**invisible AND unpickable**. They must be re-rendered through the overlay pipeline. The icons themselves are
loaded as SVG thumbnails: `state->pointLightIcon = loadIcon("icons/lightbulb.svg")`,
`spotLightIcon = "icons/flashlight.svg"`, cameraIcon (`editor_app.cppm:105-106` + `cameraIcon`), available SVGs
listed under `editor/assets/icons/` (→ `editor-old/assets/icons/` after phase-1).

> **The overlay vertex format has NO texture coords or sampler** (`OverlayVertex{pos,color}`, empty pipeline
> layout). So billboards cannot reuse the ImGui textured-quad path. They must be drawn as **colored screen-space
> glyphs** (e.g. a small filled diamond/box per entity, color-coded: point=warm, spot=cone outline, camera=box
> outline) using the existing `addBox`/`addLine`/`addTriangle` helpers. This is new geometry, not a copy.

### Input model — verified MVP behavior + the spike-0b decision point

- Worktree React forwards pointer via `invoke("viewport_pointer", {event:{kind,x,y,buttons,deltaY}})` for
  move/down/up/wheel (worktree `main.tsx:306-350`), but the Rust `viewport_pointer` shim
  (`lib.rs:367-388`) only acts on `kind=="down"`: it converts to UV and sends `click-viewport {u,v}`.
  **move/up/wheel are dropped.** The engine SDL window receives no synthetic events from this path.
- Worktree `handleNativeGizmoPointer` (engine side) IS wired to `app.window.eventSinks` in the native branch
  (worktree `editor_app.cppm` `config.onCreate`), so it works only if the **reparented SDL child window
  actually receives X11 mouse events** — which is fragile under webview focus competition (this is exactly
  what phase-3 spike-0b resolves).
- The cross-cutting decision (migration plan `openQuestions` + `seClientArchitecture`): **default to
  command-driven** `gizmo-pointer` (NDC) + `set-camera`, raw-pointer forwarding as an optimization. This phase
  implements whichever spike-0b chose; the plan below assumes command-driven as the committed path and lists
  the raw-pointer variant as the alternative.

### Selection round-trip — phase-2 provides the primitives

Main `setSelection` (`editor_context.cpp:16-20`) sets `ctx.selected` + publishes `onSelectionChanged`; there is
no version stamp on main. Phase-2 added `get-selection` (returns `entityRef|null` + a frame-stamped
`selectionVersion` that bumps inside `setSelection`) and `billboard-pick`. The reconcile poll (phase-3
`store.ts` / `client.ts`) reads `get-selection` every tick and only re-`inspect`s when
`selectionVersion`/`sceneVersion` changed. This phase makes a viewport pick (mesh ray-pick OR billboard-pick)
flow into `store.selectedId` via that poll, and adds the optimistic immediate read after a pick command resolves.

### CameraView / projection shapes (for gizmo math)

`CameraView` (`scene.cppm:301`): `{ glm::mat4 view; f32 fov; f32 nearPlane; f32 farPlane; bool valid; }`.
`cameraProjection(const CameraView&, f32 aspect) -> glm::mat4` (`scene.cppm:332`). `editorCameraView(const
EditorCamera&) -> CameraView` and `cameraProjection` are already exported from `Saffron.Scene`/`Saffron.Editor`.
The worktree gizmo math uses `cameraProjection` (un-flipped) + `cam.view` for NDC projection — matches the
non-native gizmo's "un-flipped projection" requirement (`editor_context.cppm:126-130`).

## Implementation

Ordered so each step compiles + is testable in the toolbox (`cmake --build build/debug -j1`).

### A. Engine overlay pipeline + present-only overlay pass (renderer)

1. **`renderer_types.cppm`** — add `OverlayVertex` + `OverlayState` after `RenderStats` (`:459`); add
   `Ref<Pipeline> overlay;` to `Pipelines`; add `OverlayState overlay;` and `bool presentViewportOnly = false;`
   to `Renderer` (note: `presentViewportOnly` + `setPresentViewportOnly` + `presentViewportToSwapchain` are
   forward-ported in **phase-1**; if phase-1 already added them, skip — this phase only needs the overlay
   members). Add decls: `auto newOverlayPipeline(Renderer&) -> Result<Ref<Pipeline>>;`,
   `void submitOverlay(Renderer&, std::vector<OverlayVertex>);`.
2. **`renderer_pipelines.cpp`** — port `newOverlayPipeline` verbatim from the worktree diff (after the
   depth-prepass pipeline `:186`). Keep `rasterizationSamples = e1`, format `OffscreenColorFormat`, alpha
   blend, no depth, empty layout.
3. **`renderer.cppm`**:
   - Add the file-scope fwd decl `auto makeMappedVertexBuffer(Renderer&, vk::DeviceSize) -> Result<Ref<Buffer>>;`
     and `#include <cstring>` (worktree adds both).
   - In `newRenderer` after the descriptor-set creation (where the worktree inserts at main `:214`-equivalent),
     build + assign `renderer.pipelines.overlay = *newOverlayPipeline(renderer);`.
   - In the device/pipeline teardown, `renderer.pipelines.overlay.reset();` and per-frame
     `renderer.overlay.buffers[i].reset();` (worktree teardown diff).
   - In `beginFrame`, `renderer.overlay.vertices.clear();` alongside the other per-frame clears.
   - Add `makeMappedVertexBuffer` + `submitOverlay` (worktree impls).
   - **Append the `editor-overlay` RgPass inside `beginFrameGraph` immediately after
     `addTonemapPass(renderer, graph);` (`:1423`)**, guarded by `!renderer.overlay.vertices.empty() &&
     renderer.pipelines.overlay`, using `overlay.colors.push_back(RgAttachment{ renderer.graph.sceneColor,
     vk::AttachmentLoadOp::eLoad, vk::AttachmentStoreOp::eStore, {} });` (NOT `.color =`). Render area =
     `renderer.targets.offscreen.extent`. Body grows/maps the per-frame buffer, memcpy, set viewport+scissor,
     bind overlay pipeline, bind vertex buffer, `cmd.draw(vertexCount,1,0,0)`.
   - **Do NOT** put the overlay pass in `endFrame` (where the worktree `presentViewportOnly`/`ui` split lives) —
     it belongs in `beginFrameGraph` after tonemap so it composites into the scene color that present-only blits.
     `presentViewportOnly` + `presentViewportToSwapchain` come from phase-1.

   > Do NOT forward-port the external-memory device extensions (`VK_KHR_EXTERNAL_MEMORY_FD`,
   > `VK_EXT_EXTERNAL_MEMORY_DMA_BUF`) from the worktree `newRenderer` diff — they belong to the dropped
   > fd-export path (cross-cutting `viewportBridgeDecision`) and would gate device selection. The overlay path
   > needs none of them.

4. **`editor-old/assets/shaders/gizmo_overlay.slang`** — create it (copy the 26-line worktree file). It is
   GLOB-compiled by `saffron_compile_shaders` to `shaders/gizmo_overlay.spv` in `SAFFRON_RUNTIME_DIR`.

### B. Native gizmo state + geometry + hit-test (editor)

5. **`editor_context.cppm`** — add the `NativeGizmoMode/Space/Handle` enums + `NativeGizmoState` before
   `EditorContext` (`:39`); add `NativeGizmoState nativeGizmo;` after `gizmoOp` (`:59`). Reconcile with phase-2:
   if phase-2's `set-gizmo` already drives a unified gizmo state, make `nativeGizmo.mode/.space` either the
   canonical store or a mirror updated in the `set-gizmo` handler.
6. **`editor_app.cppm`** — port the gizmo helper block (`GizmoProjection` … `submitNativeGizmo`) into the
   anonymous-ish `namespace se` region after `EditorState` (`:68`). **Change** `handleNativeGizmoPointer`'s
   BUTTON_DOWN miss branch to drop the `MaterialComponent` yellow-toggle and just do
   `setSelection(editor, pickEntity(...))` (selection only). Add `#include <SDL3/SDL.h>` +
   `glm/gtc/{constants,matrix_transform,quaternion}.hpp` (worktree adds these).
7. **Local-space gizmo** — `gizmoAxes(transform, NativeGizmoSpace::Local)` already rotates the basis by
   `glm::quat(transform.rotation)` (worktree). Verify the drag math (`applyNativeGizmoDrag`) uses `axes` (the
   space-aware basis) everywhere — it does. This is the native side of phase-2's local-space add.

### C. Billboards through the overlay (editor + engine)

8. Add a new helper `submitEditorBillboards(EditorContext&, Renderer&, const CameraView&, u32 w, u32 h,
   std::vector<OverlayVertex>& out)` in `editor_app.cppm` (or fold into `submitNativeGizmo`'s vertex list so a
   single `submitOverlay` carries both gizmo + billboards). For each `PointLightComponent` / `SpotLightComponent`
   / `CameraComponent` entity: `viewportProject(cam,w,h, transform.translation)`; if visible, emit a small
   color-coded glyph via `addBox`/`addLine`/`addTriangle` (point = filled warm box, spot = box + a short cone
   line along its direction, camera = box outline; selected entity = the yellow highlight color from
   `axisColor`'s active branch). Keep glyph half-size ~12px (match `drawEditorBillboards`' `half = 12.0f`).
   Append these to the same `std::vector<OverlayVertex>` the gizmo builds, so the gizmo draws on top.
9. **`billboard-pick`** (phase-2 command; if phase-2 deferred it, add here): a control command
   `billboard-pick {u,v}` that projects each light/camera entity's translation to viewport pixels (same
   `viewportProject` math, server-side using `editorCameraView(ctx.editor.camera)` + `ctx.renderer` viewport
   size), tests the click pixel against each glyph rect, and `setSelection`s the nearest hit (returns
   `entityRef + {hit:bool}`), else falls through to mesh `pick`. The client calls `billboard-pick` first on a
   left-click; on `hit:false` it calls `pick` (mesh ray) — or a single combined `pick` that the engine extends
   to also test billboards. Prefer extending the existing `pick` command (`control_commands_scene.cpp:304`) to
   test billboards before mesh AABBs, returning a `kind:"billboard"|"mesh"` discriminator, so the client makes
   one call. Decide in implementation; the plan defaults to **extending `pick`** (fewer round-trips) and keeps
   `billboard-pick` as the phase-2-named alias if already shipped.

### D. Input model (per spike-0b) — command-driven default

10. **`gizmo-pointer` control command** (new, `control_commands_scene.cpp`): `gizmo-pointer {phase:
    hover|begin|drag|end, x, y}` where `x,y` are **NDC in [-1,1]** (client converts CSS-px → NDC using the
    viewport rect, the same `u*2-1` mapping `pick` uses). The handler maps NDC → pixel using the renderer
    viewport size, then drives the existing engine gizmo logic server-side:
    - `hover` → `ctx.editor.nativeGizmo.hovered = hitNativeGizmo(...)` (move `hitNativeGizmo`/`applyNativeGizmoDrag`
      into a header-visible spot in `Saffron.Editor` or expose thin wrappers, since the command TU needs them).
    - `begin` → set `active`/`dragging`/`start*` from the current transform (mirror
      `handleNativeGizmoPointer`'s BUTTON_DOWN gizmo branch).
    - `drag` → `applyNativeGizmoDrag(...)` (writes the selected entity's `TransformComponent`).
    - `end` → clear `dragging`/`active`/`target`.
    Returns `{ hovered:Handle, dragging:bool }`. The client throttles `drag` via the coalesced-write helper
    (phase-3 `coalesce.ts`).

    > **Refactor seam:** `hitNativeGizmo`/`applyNativeGizmoDrag` currently live as free functions in the
    > `editor_app.cppm` TU (the editor *app*, not the `Saffron.Editor` module). The `gizmo-pointer` command
    > lives in `Saffron.Control` (`control_commands_scene.cpp`). Move these gizmo helpers (and
    > `viewportProject`/`gizmoAxes`/`handleAxis`/`pointSegmentDistance`) into `Saffron.Editor`
    > (`editor_gizmo.cpp` + decls in `editor_context.cppm`) so BOTH the native SDL event sink AND the control
    > command call one implementation. This also lets phase-2's `set-gizmo` and this command share state.

11. **Topbar input variant (alternative, if spike-0b proved raw input reaches the child):** extend the Rust
    `viewport_pointer` shim (worktree `lib.rs:367`) to forward move/up/wheel (not just down) and synthesize SDL
    events into the child — but the MVP never did this and it is the biggest risk; keep it OFF unless spike-0b
    explicitly chose it. The committed path is command-driven (step 10).

12. **Camera input:** if spike-0b chose command-driven, wire WASD/RMB-look to phase-2 `set-camera` from the
    ViewportPanel keydown/pointer handlers (throttled). If native keyboard reaches the child, leave the
    engine's `updateEditorCamera` (already called in the worktree native `onUi`) as-is. The plan defaults to:
    keep `updateEditorCamera` running engine-side (it reads `ImGui::GetIO().DeltaTime` only, harmless) and add
    `set-camera`-driven look/move from React only if the spike showed native keyboard does not arrive.

### E. React: Topbar gizmo group + ViewportPanel input + selection round-trip

13. **`editor/src/panels/Topbar.tsx`** — port the MVP gizmo group (worktree `main.tsx:208-256`): three buttons
    (Move3D/Rotate3D/Scaling, lucide) for translate/rotate/scale + a world/local toggle. On click call
    `client.setGizmo({op, space})` (phase-2 typed method). Reflect `store.gizmo` (populated by the reconcile
    poll's `getGizmo`, or optimistically on click). Remove the MVP's manual Start Engine/Attach buttons (auto in
    phase-3).
14. **`editor/src/panels/ViewportPanel.tsx`** — replace the MVP placeholder pointer handlers
    (`main.tsx:306-350`) with the chosen input model:
    - On left-click (pointerdown then pointerup with no drag): convert CSS-px (`offsetX/Y`) → NDC using the
      panel rect, call `client.pick({u,v})` (which now also tests billboards, step 9). On `hit:false`, the
      engine deselected; the poll will surface `selectedId=null`.
    - On gizmo drag (pointerdown over a handle region — but the engine owns hit-test, so just stream pointer):
      send `gizmo-pointer hover` on move, `begin` on pointerdown, throttled `drag` on move while down, `end` on
      pointerup. Use the coalesced-write helper for `drag`.
    - Keep the bounds-sync glue from phase-3 (ResizeObserver + `resize-native-viewport`).
15. **`editor/src/state/store.ts` + `editor/src/control/client.ts`** — ensure `selectedId`, `selectionVersion`,
    `gizmo {op,space}` are in the store; the reconcile poll (phase-3) already reads `get-selection` every tick.
    Add the optimistic path: after `pick`/`billboard-pick` resolves with a hit, set `store.selectedId`
    immediately (don't wait a full poll interval). Gate writes OFF during an active gizmo drag (the poll must
    not clobber the optimistic transform mid-drag — cross-cutting `seClientArchitecture`).
16. **`editor/src-tauri/src/lib.rs`** — with phase-3's generic `control(cmd, params)` passthrough, NO new Rust
    shim is needed for `gizmo-pointer`/`billboard-pick`/`pick`/`set-gizmo`/`get-gizmo`/`get-selection`; the
    client calls them through `control()`. Only edit `lib.rs` if the raw-pointer-forwarding variant (step 11)
    was chosen — then extend `viewport_pointer`.

### F. se CLI + docs (AGENTS.md se-current rule)

17. **`tools/se/source/main.cpp`** — add `printResult` formatters for the new commands (`gizmo-pointer`,
    `billboard-pick`, and `pick`'s new `kind` field if extended). Anchor: `printResult` at
    `tools/se/source/main.cpp:112`, the `render-stats` block at `:166` is the pattern.
18. **`docs/content/reference`** — document `gizmo-pointer` + `billboard-pick` (and the `pick` billboard
    extension) alongside the existing pick/select/gizmo reference rows; update the hub `_index.md` row. Add an
    explanation note (or extend phase-1's bridge page) describing the overlay-rendered gizmo + billboards under
    present-only (ImGui is skipped, so handles/billboards are engine-drawn).

## Done when

- [ ] `cmake --build build/debug -j1` in the `saffron-build` toolbox succeeds, validation-clean (no new VVL
      errors), with the overlay pipeline + `gizmo_overlay.spv` built and present in `SAFFRON_RUNTIME_DIR`.
- [ ] `editor-old/` C++ ImGui editor still builds + runs unchanged when `SAFFRON_EDITOR_NATIVE_VIEWPORT` is unset
      (the ImGui `drawGizmo`/`drawEditorBillboards` path is untouched).
- [ ] In the embedded Tauri viewport (present-only), selecting a mesh entity shows **visible T/R/S gizmo
      handles** rendered by the engine overlay; lights/cameras/empties show **billboards** (color-coded glyphs),
      all of which are absent in the MVP present-only mode today.
- [ ] Under `se set-aa msaa4` (and `taa`, `fxaa`, `off`) the overlay + billboards still render correctly with
      **no sample-count validation error** (the overlay runs at 1x on the resolved offscreen; this is the
      explicit MSAA-ordering test).
- [ ] Left-click in the viewport **ray-picks a mesh** AND clicking a **light/camera billboard selects it**
      (single `pick` call, billboards tested first); empty space **deselects**. React `store.selectedId` updates
      within the reconcile poll interval (and immediately via the optimistic post-pick read).
- [ ] Topbar T/R/S + world/local change the gizmo: `se get-gizmo` reflects the change AND the visible handle
      geometry changes (translate arrows → rotate rings → scale boxes; world vs local axis orientation).
- [ ] Dragging a gizmo handle (command-driven `gizmo-pointer`, or raw pointer if spike-0b chose it) moves /
      rotates / scales the selected entity and **persists** (`se inspect <id>` shows the new transform; a save
      round-trips it).
- [ ] `se select <id>` or `se pick --u .. --v ..` from a **separate terminal** updates the React selection
      within the poll interval (proves the round-trip is not UI-local).
- [ ] No input-focus deadlock between the webview and the reparented child window (per the spike-0b model:
      command-driven means the webview keeps focus and pointer events route through `control()`).
- [ ] `se gizmo-pointer` and `se billboard-pick` (and the extended `pick`) appear in `se help` with docs pages;
      the docs hub row is updated.

## Risks / seams

- **Overlay sample-count under MSAA** — mitigated by design (overlay writes the 1x resolved `offscreen` after
  tonemap), but the explicit `set-aa msaa4`/`taa` test is mandatory because a future change that moves the
  overlay before the resolve would silently break the e1 assumption.
- **Overlay RgPass ordering vs TAA/SSGI/AA** — the overlay MUST stay appended after `addTonemapPass` (the last
  `sceneColor` writer). If a later phase adds a post-tonemap pass that also writes `sceneColor`, re-confirm the
  overlay still lands last (or it will be overwritten / the graph will derive a wrong barrier).
- **Input routing for the reparented child is the biggest interaction risk** — resolved by spike-0b. The
  command-driven `gizmo-pointer`/`set-camera` path is the robust fallback and the committed default; raw SDL
  pointer/keyboard forwarding stays an optimization, not a dependency.
- **Billboards are new geometry, not a copy** — the overlay vertex format has no UV/sampler, so the textured
  `ImGui::AddImage` icons become colored glyphs. Visual parity is approximate (glyphs vs. SVG icons); document
  this as accepted (the SVG icons remain in `editor-old/assets/icons/` for the retired ImGui path).
- **Gizmo-state duplication** — `nativeGizmo` (overlay) and `gizmoOp` (ImGuizmo) + phase-2's `set-gizmo` must be
  one source of truth. Move the gizmo math into `Saffron.Editor` so the SDL event sink and the `gizmo-pointer`
  command share it, and so `set-gizmo` mutates the same state the overlay reads.
- **Selection race during drags** — gate the reconcile poll's writes OFF while a gizmo drag is active so the
  poll's `inspect`-driven transform read does not clobber the optimistic local transform mid-drag (cross-cutting
  `seClientArchitecture`). Commit the authoritative transform on `end`.
- **`pickEntity` NDC convention** — `pickEntity` expects `ndc` matching the Y-flipped rendered image
  (`assets.cppm:662-664`); the existing `pick` command maps `u,v` (0,0 = top-left) → `{u*2-1, v*2-1}`. Reuse
  that exact mapping for `gizmo-pointer` and the billboard hit-test so screen-to-world stays consistent across
  ray-pick, billboard-pick, and gizmo.
- **Non-goals (parity-correct):** no multi-viewport gizmo, no snapping/grid, no undo on gizmo drags (the C++
  editor lacks these too) — state them so they aren't read as gaps.
