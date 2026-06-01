# Phase 10: Retire the C++ ImGui editor + CMake/CI/docs finalize

**Status:** IN PROGRESS — C++ ImGui editor retired + engine build validation-clean + headless host smoke EXIT=0. `editor_panels.cpp` deleted (the only caller was `editor_app` onUi, now slimmed to the present-only host path), its CMake source line + the panel decls/ImGui closures (`editor_context.cppm`) + `drawGizmo`/`drawEditorBillboards` (`editor_gizmo.cpp`) removed; `editor_components.cpp` kept (registry serde backs the control path + copy-entity); the native-gizmo math + overlay + control surface untouched. `SaffronEditor` (`editor-old/`) is now the always-present-only headless host the Tauri app spawns (`editor/`). **Finalize DONE:** `AGENTS.md` editor/layout updated; `tools/ci/check.sh` gate + root `Makefile` (`check`/`engine`/`editor`/`schema`) + an honest self-hosted `.github/workflows/ci.yml` (documents that a hosted runner can't build the toolbox stack) + `tools/ci/README.md`; the **docs retarget is complete** — the 11 `ui-and-editor` pages were rewritten for the Tauri/React editor (`imgui-integration.md` → new `tauri-editor-and-x11-bridge.md`), the hub + `build-and-run` how-to updated, `hugo --gc` builds clean (181 pages, 0 errors). **Only remaining:** the live end-to-end Tauri-editor parity walkthrough (`bun run tauri dev` on an X11/XWayland desktop) — needs a display.

<!-- Flip to COMPLETED when the "Done when" checklist passes, validation-clean. Delete this file only after COMPLETED + merged. -->

## Goal

Make the TS/Tauri editor the sole editor. With every panel at parity (phases 4-9
shipped), delete the C++ ImGui *panel* code from `editor-old/` while keeping
`SaffronEditor` + its shader/asset compilation alive as the headless native-viewport
host the Tauri app spawns and reparents. Finalize the build (no `editor/` vs
`editor-old/` collision), CI (`bun run check` + the schema contract test + a bounded
native-viewport smoke), and docs + `AGENTS.md`, then mark the migration COMPLETED.

This phase removes code; it adds none to the engine and no control commands. The
entire risk surface is "what does the present-only native host still depend on" — so
removal is surgical, rebuild-after-each-deletion, and the headless host's
shader/asset emission must survive intact.

**Depends on:** phase-9 (theme/dock parity) — i.e. the full panel surface (phases
4-8) and visual parity (phase-9) are done, because parity is the precondition for
deleting the only other editor.

## Current state (verified)

### Layout assumed at the start of this phase (set by phase-1)

Phase-1 already moved the C++ editor to `editor-old/` and put the Tauri app at
`editor/`, exactly as the worktree did at `faf704d`
(`wt:CMakeLists.txt:32 = add_subdirectory(editor-old)`;
`wt:editor-old/CMakeLists.txt` is byte-identical to today's
`editor/CMakeLists.txt`). On **main today** that move has NOT happened yet — these
are the live references this phase inherits and must update:

- Root `CMakeLists.txt:31-33` currently reads:
  ```cmake
  add_subdirectory(engine)
  add_subdirectory(editor)
  add_subdirectory(tools/se)
  ```
  After phase-1 line 32 is `add_subdirectory(editor-old)`. **This phase decides its
  final form** (see Implementation step 4).
- `editor/CMakeLists.txt` (today) / `editor-old/CMakeLists.txt` (post phase-1) is the
  *inseparable headless-host artifact*: it declares `add_executable(SaffronEditor)`
  from `source/main.cpp` (`editor/CMakeLists.txt:1-3`), links `Saffron::Engine`
  (`:8`), runs `saffron_compile_shaders(SaffronEditor <src>/assets/shaders
  ${SAFFRON_RUNTIME_DIR}/shaders)` (`:11-13`), and copies `assets/models`,
  `assets/fonts`, `assets/icons` to `${SAFFRON_RUNTIME_DIR}` in a POST_BUILD step
  (`:16-26`). `SAFFRON_RUNTIME_DIR = ${CMAKE_BINARY_DIR}/bin` is set once at
  `CMakeLists.txt:22`. **Every one of the ~31 engine shaders is GLOBed from this one
  `assets/shaders` dir** — `CompileShaders.cmake` GLOBs `*.slang`, so the engine has
  no shaders if this target's compile step is dropped.
- `editor/source/main.cpp` is a 6-line stub: `import Saffron.EditorApp; int main() {
  return se::runEditor("Saffron Editor", 1600, 900); }`. It survives unchanged as the
  headless-host entry point.

### What `runEditor` does today (the code that must split)

`engine/source/saffron/editorapp/editor_app.cppm` is **one** `runEditor` that builds
the ImGui editor inline. On main it has NO native branch yet (phase-1 adds it). Key
anchors in the current file:

- `editor_app.cppm:289-376` — `layer.onUi` draws ALL ImGui panels AND drives the
  viewport: `drawEditorMenuBar` (`:295`), `viewportPanel` (`:296`),
  `updateEditorCamera` (`:298`), `renderScene` (`:305`), `drawGizmo` (`:310`),
  `drawEditorBillboards` (`:317`), ray-pick via `pickEntity` (`:329-340`),
  `hierarchyPanel`/`environmentPanel`/`assetCatalogPanel`/`viewerPanel`/Render-Stats/
  `inspectorPanel` (`:343-375`).
- After phase-1 this `onUi` gains the early `if (nativeViewport) { ...renderScene +
  submitNativeGizmo...; return; }` branch (verified shape at
  `wt:editor_app.cppm:728-741`: it calls **only** `setViewportDesiredSize`,
  `updateEditorCamera`, `renderScene`, `submitNativeGizmo`, then `return` — never any
  ImGui panel). Phase-4 adds ray-pick + billboards to that native branch.

### The C++ ImGui panel surface to delete (verified)

These are the panel/menu draw functions that **only** the ImGui (non-native) path
uses:

- `engine/source/saffron/editor/editor_panels.cpp` (429 lines) — `hierarchyPanel`
  (`:19-109`), `inspectorPanel` (`:111-170`), `drawImportModal` (`:172-189`),
  `environmentPanel` (`:191-224`), `assetCatalogPanel` (`:226-325`), `viewerPanel`
  (`:327-351`), `drawEditorMenuBar` (`:353-427`). All pure ImGui. **Delete this whole
  TU.**
- Their declarations in `engine/source/saffron/editor/editor_context.cppm:88-139`
  (`hierarchyPanel`, `inspectorPanel`, `drawImportModal`, `assetCatalogPanel`,
  `viewerPanel`, `environmentPanel`, `drawEditorMenuBar`, `drawEditorBillboards`).
  **Delete these decls** (but see the "keep" list — `drawAssetPicker`,
  `registerBuiltinComponents`, gizmo/camera decls stay).
- `engine/source/saffron/editor/editor_components.cpp` — the ImGui inspector draw
  lambdas + `drawAssetPicker` (`editor_components.cpp:21-84`). **This is a
  judgement call (see Risks):** the lambdas are ImGui, but `registerBuiltinComponents`
  also wires the **JSON serialize/deserialize** that the *control* path
  (`inspect`/`set-component`/save-load) and the Hierarchy `Copy` deep-copy
  (`editor_panels.cpp:89-98`) depend on. The native host still calls
  `registerBuiltinComponents` (`editor_app.cppm:150`). **Keep `editor_components.cpp`;
  it is not dead** — its ImGui draw closures simply never fire under present-only
  (the registry rows' `drawInspector` is only invoked by `inspectorPanel`, which is
  gone). Removing the ImGui includes from it is out of scope (low value, high churn).

### The Saffron.Editor pieces the native path REUSES (must NOT delete)

Verified by the worktree native branch (`wt:editor_app.cppm:728-741`) + phase-4:

- `EditorCamera` + `editorCameraView` + `updateEditorCamera` + `editorCameraForward`
  (`editor_context.cppm:24-35`, `:116-124`; impl `editor_camera.cpp`) — the fly-cam.
- `pickEntity` (declared in `Saffron.Scene`, called at `editor_app.cppm:339`) — used
  by the native ray-pick (phase-4) and the `pick` control command.
- `drawGizmo` (`editor_context.cppm:131-132`, `editor_gizmo.cpp:20-69`) — the ImGuizmo
  gizmo. **Verify before deleting:** the native path uses `submitNativeGizmo`
  (engine overlay, phase-4), NOT `drawGizmo`. `drawGizmo` (ImGuizmo) is ImGui-only and
  is a deletion *candidate* — but `ctx.gizmoOp` (`editor_context.cppm:59`) is shared
  state the native gizmo also reads. Delete `drawGizmo` + its decl only if nothing in
  the native path references it (grep first); keep `ctx.gizmoOp` / the gizmo op+space
  state on `EditorContext` (phase-2/4 added op+space + `nativeGizmo` state there).
- `registerBuiltinComponents` + the `ComponentRegistry` serialize/deserialize (drives
  `inspect`/`set-component`/save-load over the socket).
- `setSelection` + `onSelectionChanged` (`editor_context.cppm:44`, `:69`) — the
  native `pick`/`get-selection` round-trip (phase-4).
- The overlay submit path (`submitNativeGizmo` + billboard overlay) lives in
  `editor_app.cppm` (native), not in `editor_panels.cpp`.

### CMake/CI/docs anchors to finalize

- Engine CMake `engine/CMakeLists.txt`: `editor_panels.cpp` is listed as a PRIVATE
  source (`engine/CMakeLists.txt`, in the `target_sources(SaffronEngine PRIVATE ...)`
  block alongside `editor_camera.cpp`/`editor_gizmo.cpp`/`editor_components.cpp`).
  Deleting `editor_panels.cpp` requires removing that one line.
- Docs to touch: `docs/content/how-to/_index.md` (table of recipes — `build-and-run`
  row), `docs/content/how-to/build-and-run.md` (the toolbox/run recipe, currently all
  C++), `docs/content/explanations/_index.md` (`UI & editor` + `Tooling & control`
  rows), and the `ui-and-editor/` section (`hierarchy-panel.md`, `inspector.md`,
  `gizmo.md`, `assets-panel-and-thumbnails.md`, etc. — they describe the now-deleted
  ImGui panels). `docs/content/explanations/tooling-and-control/_index.md` already
  frames "a running editor is scriptable" — the X11 bridge + shared-type pipeline
  pages slot in there.
- `AGENTS.md:21-39` (TL;DR build+run recipe — `./build/debug/bin/SaffronEditor` is
  described as *the* editor) and `AGENTS.md:96-130` (Layout: `editor/` row says
  "SaffronEditor executable"). Both must reflect the Tauri editor as default.
- `tools/se/source/main.cpp:112` `printResult` + the `se` CLI stay (the TS client and
  the contract test both use `se` payloads). No `se` changes in this phase unless a
  prior phase left a gap.

## Implementation

Do the steps **in order**, rebuilding (`-j1`) after each removal so a broken native
dependency surfaces immediately (Risks #1).

### Step 0 — Parity gate (no code yet)

Before deleting anything, confirm 100% parity by running the C++ host (no env vars →
ImGui editor) and the Tauri editor side by side against the same `project.json`, and
walking the checklist:

- Hierarchy (list/select/add-preset/copy/delete) — TS phase-5 vs
  `hierarchyPanel` (`editor_panels.cpp:19-109`).
- Generic inspector (all 8 components, add/remove, deg/rad) — TS phase-6 vs
  `inspectorPanel` (`:111-170`) + `editor_components.cpp:86-278`.
- Asset browser (thumbnails, rename, drag-drop, pickers, import, View modal) — TS
  phase-7 vs `assetCatalogPanel` (`:226-325`) + `viewerPanel` (`:327-351`).
- Environment panel — TS phase-8 vs `environmentPanel` (`:191-224`).
- Create menu (all presets) — TS phase-5 vs `drawEditorMenuBar`/`Create`
  (`:386-424`).
- Gizmo T/R/S + world/local, fly-cam, ray-pick + billboard selection — TS phase-4 vs
  `drawGizmo`/`updateEditorCamera`/`drawEditorBillboards`.
- Save/load project + scene, import, render-stats — TS phase-8.

Record the walkthrough result in this file's COMPLETED comment. **Do not proceed to
deletion until every row passes.**

### Step 1 — Remove the `onUi` ImGui-panel block from the native host

In `engine/source/saffron/editorapp/editor_app.cppm`, the post-phase-1 `onUi` lambda
has two paths: the native early-return branch (`renderScene` + `submitNativeGizmo` +
ray-pick/billboards from phase-4) and the ImGui-panel block. Because the native host
is now the **only** consumer of `runEditor`:

1. Delete the ImGui-panel block of `onUi` (the `drawEditorMenuBar` … `inspectorPanel`
   tail — today `editor_app.cppm:295-375`, post-phase-1 the `else`/fallthrough after
   the native `return`). Keep the native branch as the whole `onUi` body.
2. Delete the `nativeViewport` gating now that there is no ImGui path: `onCreate`
   keeps `setPresentViewportOnly(app.renderer, true)` unconditionally. The
   `SAFFRON_EDITOR_NATIVE_VIEWPORT` env check (`wt:editor_app.cppm:484`) can be kept
   as a guard (still useful: an unset env can keep a minimal fallback for
   debugging) — **decision:** keep the env var so the host is still runnable
   standalone for debugging (sized full-window, no host XID), but the ImGui panels
   are gone in both states. Document the env var in `editor/README.md`.
3. Remove now-unused captures/locals (`thumbnailFor`/`onView` were captured by the
   panel block; the asset-thumbnail cache + `viewer` state in `EditorState` were
   ImGui-only — but `thumbnailFor` is still passed to `registerBuiltinComponents`
   (`editor_app.cppm:150`), so KEEP `thumbnailFor` and the icon/thumbnail cache;
   they feed the registry. Remove only `onView`, `viewerPanel`, the `viewer` struct
   in `EditorState` (`editor_app.cppm:62-67`), and the `eyeIcon`/billboard-icon
   `ImTextureID`s if phase-4's overlay billboards no longer use ImGui `ImTextureID`s
   — verify against phase-4's billboard implementation before removing the billboard
   icons).
4. Rebuild `-j1`. Fix any unresolved symbol by re-adding the capture (do not delete
   a panel function yet — that is step 2).

### Step 2 — Delete the ImGui panel TU + its declarations

1. `git rm engine/source/saffron/editor/editor_panels.cpp`.
2. In `engine/CMakeLists.txt`, remove the
   `source/saffron/editor/editor_panels.cpp` line from the
   `target_sources(SaffronEngine PRIVATE ...)` block.
3. In `engine/source/saffron/editor/editor_context.cppm`, delete the decls for the
   removed functions: `hierarchyPanel` (`:88`), `inspectorPanel` (`:90`),
   `drawImportModal` (`:94`), `assetCatalogPanel` (`:99-102`), `viewerPanel`
   (`:106`), `environmentPanel` (`:110-111`), `drawEditorMenuBar` (`:113`),
   `drawEditorBillboards` (`:136-139` — only if phase-4 replaced the ImGui billboard
   with an engine-overlay billboard that does NOT reuse this signature; otherwise
   keep). **Keep:** `drawAssetPicker` (`:74-75`), `registerBuiltinComponents`
   (`:80-81`), `newEditorContext`/`destroyEditorContext` (`:85-86`),
   `setSelection` (`:69`), `editorCameraForward`/`editorCameraView`/
   `updateEditorCamera` (`:116-124`), and the gizmo op/space state.
4. Delete the `onImport`/`onSaveProject`/`onLoadProject`/`onCreateCube`/`importPath`
   members on `EditorContext` (`editor_context.cppm:48-58`) **only if** the native
   host no longer wires them. The native host wires save/load/import over the
   *control* commands (phase-2's `add-entity`/`importModel`/`saveProject` go through
   `control_commands_*`, not `EditorContext` closures), so these editor-closure
   members are ImGui-only. Verify by grep: if `editor_app.cppm` (native) sets
   `state->editor->onImport` etc. only inside the deleted panel-wiring, remove them.
   `onCreateCube` is replaced by the `add-entity --preset cube` control command
   (phase-2). **Remove surgically; rebuild after.**
5. `drawGizmo` (`editor_gizmo.cpp:20-69`) is ImGuizmo (ImGui). The native path uses
   `submitNativeGizmo`. **Delete `drawGizmo` + its decl** (`editor_context.cppm:
   126-132`) only after grepping the engine for callers and confirming the native
   path does not reference it. Keep `editor_gizmo.cpp` as the file if
   `submitNativeGizmo`/billboard-overlay live there (phase-4 may have added them
   here); otherwise the file becomes deletable. Keep `ctx.gizmoOp` and the gizmo
   op/space + `nativeGizmo` state on `EditorContext`.
6. Rebuild `-j1`. Resolve compile errors by confirming each removed symbol was truly
   ImGui-only (Risks #1).

### Step 3 — Drop the ImGuizmo/ImGui dependency from the editor module if it is now unused

After step 2, check whether `editor_context.cppm` / `editor_gizmo.cpp` /
`editor_components.cpp` still `#include <imgui.h>`/`<ImGuizmo.h>`. They likely still
do (`registerBuiltinComponents` draw lambdas, `drawAssetPicker`,
`submitNativeGizmo`). **Do not** strip ImGui from the engine — ImGui is still linked
for the renderer's ImGui-Vulkan backend used by the offscreen→swapchain present in
non-native fallback and by `uiRegisterTexture`. This step is a no-op unless a TU's
ImGui include became unused; if so remove only that include. Keep `imgui` linked in
`Dependencies.cmake`.

### Step 4 — Rewire CMake so the headless host survives, no dir collision

The directory layout after phase-1 is `editor/` = Tauri, `editor-old/` = C++ host.
This phase keeps that split but makes the *naming* honest (it is no longer "old" — it
is the production viewport host):

1. **Decision:** rename `editor-old/` → `engine-host/` (or keep `editor-old/` to
   avoid a large rename diff — **recommended: keep `editor-old/`** to minimize churn
   and because the worktree, CI scripts, and shader GLOB path all already reference
   it; a rename buys nothing functional). Document in `editor/README.md` + `AGENTS.md`
   that `editor-old/` is the *headless viewport host* (not dead code).
2. Confirm `editor-old/CMakeLists.txt` is intact: `add_executable(SaffronEditor)`,
   `saffron_compile_shaders(SaffronEditor .../assets/shaders
   ${SAFFRON_RUNTIME_DIR}/shaders)`, and the models/fonts/icons POST_BUILD copy. **Do
   not touch the shader compile or asset copy** — they emit all ~31 engine shaders
   (incl. `gizmo_overlay.slang`, added under `editor-old/assets/shaders/` in phase-4)
   + models/fonts/icons to `SAFFRON_RUNTIME_DIR`. The TS editor's icons are its own
   assets bundled by Vite/Tauri and are unrelated to this copy.
3. Root `CMakeLists.txt:31-33` final form:
   ```cmake
   add_subdirectory(engine)
   add_subdirectory(editor-old)   # SaffronEditor: the headless native-viewport host (shaders + assets)
   add_subdirectory(tools/se)
   ```
   The Tauri app at `editor/` is **not** an `add_subdirectory` (it is a Bun/Cargo
   build, not CMake). Add a comment in `CMakeLists.txt` noting that `editor/` builds
   via `bun run` / Tauri, not CMake, and that `editor-old/` is the engine's shader/
   asset host — so a reader doesn't expect an `add_subdirectory(editor)`.
4. Rebuild `-j1`; confirm `build/debug/bin/SaffronEditor` exists, `build/debug/bin/
   shaders/*.spv` has the full set (count parity with pre-phase shader count), and
   `build/debug/bin/{models,fonts,icons}` are populated.

### Step 5 — Finalize CI (per the phase-3 build-env spike)

Phase-3 resolved whether Bun/Tauri build in the `saffron-build` toolbox or on the
host. CI must run **both** the engine (toolbox) and the frontend (toolbox or host per
that finding):

1. **Engine build + headless smoke** (toolbox):
   ```sh
   toolbox run -c saffron-build bash -lc '
     cd /var/home/saffronjam/repos/SaffronEngine
     cmake --preset debug
     cmake --build build/debug -j1'
   ```
   then a bounded present-only smoke that exercises the native host without a host
   XID (it must run + present + exit clean, no leak abort — the skybox README notes
   the VMA teardown leak is fixed, commit `7243ca4`):
   ```sh
   SAFFRON_EDITOR_NATIVE_VIEWPORT=1 SAFFRON_EXIT_AFTER_FRAMES=5 \
     ./build/debug/bin/SaffronEditor
   ```
   (No `attach-native-viewport` in CI — there is no parent window; the smoke just
   proves present-only renders + tears down. A full attach smoke needs an X server
   and is out of scope for headless CI; note this.)
2. **Schema contract test** (the phase-2 `tools/check-control-schema`): spawn the
   host with a control socket, run representative `se` commands, validate every reply
   against `schemas/control/*.schema.json`. Runs in the toolbox (it only needs `se` +
   the schemas + a JSON-schema validator).
3. **Frontend type check**: `bun run check` (tsc + the `gen-protocol` prebuild) in
   the documented environment from the phase-3 spike. Must regenerate
   `editor/src/protocol/` deterministically and pass `tsc`.
4. Wire all three into the project's CI runner (a `tools/ci/` script or the existing
   CI config — match whatever phases 1-9 established; if none, add a documented
   `tools/ci/check.sh` that runs the three steps and is referenced from
   `editor/README.md`).

### Step 6 — Docs

1. `docs/content/how-to/build-and-run.md` — replace the C++-only recipe with: (a)
   build the engine + headless host (toolbox, `-j1`); (b) run the Tauri editor (`bun
   install`, `bun run dev` / `bun run tauri dev`) which auto-spawns `SaffronEditor`
   and auto-attaches the viewport (no manual button). Keep the
   `SAFFRON_EXIT_AFTER_FRAMES`/`SAFFRON_CAPTURE` headless note for the host. Update
   the "In the code" table rows.
2. `docs/content/how-to/_index.md` — the `build-and-run` row text; add (if not
   present) a "run the Tauri editor" recipe row.
3. New explanation page `docs/content/explanations/tooling-and-control/
   tauri-editor-and-x11-bridge.md` — the production bridge: Tauri Rust `.setup()`
   spawns `SaffronEditor` (env `SAFFRON_EDITOR_NATIVE_VIEWPORT=1` +
   `SAFFRON_CONTROL_SOCK` + `SDL_VIDEODRIVER=x11`), the engine reparents its SDL/X11
   window over the viewport div (`attach-native-viewport` →
   `XReparentWindow`/`XMapRaised`, `control_commands_render.cpp`), present-only
   (`presentViewportToSwapchain`), auto-start/auto-attach + loading overlay, and the
   one generic `control(cmd,params)` passthrough. Cross-link the
   `se-cli-protocol`/`control-plane-architecture` pages.
4. New explanation page `docs/content/explanations/tooling-and-control/
   shared-types-pipeline.md` — schema-first: `schemas/control/*.schema.json`
   (draft 2020-12) is the source of truth, `json-schema-to-typescript` →
   `@saffron/protocol`, the contract test validates live `se` output, u64-as-string,
   camelCase. Note C++26 reflection deferred + `dump-schema`/reflect-cpp as the
   forward seam.
5. `docs/content/explanations/tooling-and-control/_index.md` — add the two new page
   rows to the table.
6. `docs/content/explanations/_index.md` — update the `UI & editor` row to say the
   editor is the Tauri/TS app over an embedded engine viewport (not ImGui panels);
   keep the `Tooling & control` row.
7. The `ui-and-editor/` pages (`hierarchy-panel.md`, `inspector.md`, `gizmo.md`,
   `assets-panel-and-thumbnails.md`, `selection.md`, `viewport-panel.md`,
   `theme-and-fonts.md`, `imgui-integration.md`, `editor-camera.md`,
   `mesh-thumbnails.md`, `asset-pickers-and-drag-drop.md`) describe ImGui internals.
   **Decision:** retarget each to describe the TS/Tauri panel that replaced it +
   which control commands back it (e.g. `hierarchy-panel.md` → "the Hierarchy panel
   is React, backed by `list-entities`/`add-entity`/`copy-entity`; selection
   round-trips via `get-selection`"). The engine-side pieces that survive
   (`editor-camera.md` → `EditorCamera`, `mesh-thumbnails.md` → `get-thumbnail`
   readback, `gizmo.md` → engine overlay + `set-gizmo`) keep their engine grounding.
   This is a rewrite-in-place, not a delete, to preserve the Diátaxis hub inventory.
8. **State non-goals explicitly** (in the new bridge page + `editor/README.md`): no
   undo/redo, no scene-graph parenting/`resolveRefs`, no multi-viewport, no
   native-Wayland (XWayland-only) — the C++ editor lacked these too, so they are not
   regressions.
9. Build the docs to confirm no broken links: per `docs/README.md` (Hugo +
   hugo-book; Hugo at `~/.local/bin`). Every body page needs a `# Title` H1.

### Step 7 — `AGENTS.md`

1. TL;DR (`AGENTS.md:21-39`): keep the toolbox engine-build recipe; add the Tauri
   editor run recipe (build engine `-j1`, then `bun run tauri dev` in `editor/`,
   which auto-spawns + auto-attaches). Note that `./build/debug/bin/SaffronEditor` is
   now the *headless viewport host* (present-only when `SAFFRON_EDITOR_NATIVE_VIEWPORT
   =1`), not a standalone GUI editor.
2. Layout section (`AGENTS.md:96-130`): update the `editor/` row to "Bun/Tauri/React
   TypeScript editor"; add an `editor-old/` row "SaffronEditor — the headless
   native-viewport host (owns engine shader compile + model/font/icon copy)".
3. Add a one-line tech-stack note (the table at `:43-`): the editor frontend is
   Bun + Tauri 2 + React + TypeScript, talking JSON-over-unix-socket to the engine.
4. Current-status section: mark the C++ ImGui editor retired and the TS/Tauri editor
   as the default; reference `plans/typescript-ui-migration/` as COMPLETED.

### Step 8 — Mark the plan COMPLETED

1. Flip every phase file's `**Status:**` to `COMPLETED` (or confirm prior phases
   already did), and flip this file's.
2. Update the `plans/typescript-ui-migration/README.md` status to COMPLETED.
3. Per the repo convention (`plans/skybox/README.md:7-9`): keep the plan files until
   the branch is merged; delete only after COMPLETED + merged.

## Done when

- [ ] Step-0 parity walkthrough passes on every row (hierarchy, generic inspector,
      asset browser w/ thumbnails+drag-drop, environment, create menu, gizmo
      T/R/S+world/local, fly-cam, ray-pick+billboard selection, save/load
      project/scene, import, render-stats); result recorded in this file.
- [ ] `editor_panels.cpp` is deleted and removed from `engine/CMakeLists.txt`; the
      panel decls are gone from `editor_context.cppm`; no other engine TU references
      them.
- [ ] A clean toolbox build (`cmake --preset debug` + `cmake --build build/debug
      -j1`) succeeds with no new validation errors and produces
      `build/debug/bin/SaffronEditor` + the full `build/debug/bin/shaders/*.spv`
      set (count parity with the pre-deletion shader count, incl.
      `gizmo_overlay.spv`) + populated `build/debug/bin/{models,fonts,icons}`.
- [ ] `SAFFRON_EDITOR_NATIVE_VIEWPORT=1 SAFFRON_EXIT_AFTER_FRAMES=5
      ./build/debug/bin/SaffronEditor` runs present-only and exits code 0 (no VMA
      teardown abort, no orphan).
- [ ] Launching the Tauri editor (`bun run tauri dev` in `editor/`) gives a full
      editor — all panels at parity — over an embedded live viewport with
      auto-start/auto-attach and the loading overlay, with NO button clicks.
- [ ] Closing the Tauri window terminates `SaffronEditor` and unlinks the per-instance
      control socket (no orphan process, no stale socket in `XDG_RUNTIME_DIR`).
- [ ] CI runs all three checks green in the documented environment: the engine build +
      bounded native-viewport smoke (toolbox), the schema contract test
      (`tools/check-control-schema`), and `bun run check` (frontend, regenerating
      `editor/src/protocol/`).
- [ ] No dead C++ ImGui panel code remains — only the `Saffron.Editor` pieces the
      native path uses survive (`EditorCamera`/`updateEditorCamera`, `pickEntity`,
      gizmo op/space + `nativeGizmo` state + `submitNativeGizmo`, billboard overlay,
      `registerBuiltinComponents` serialize/deserialize, `setSelection`/
      `onSelectionChanged`, `drawAssetPicker` if still used by the registry).
- [ ] `editor_components.cpp` builds and its registry serialize/deserialize still
      backs `inspect`/`set-component`/save-load + Hierarchy `copy-entity` (verified by
      an `se inspect`/`set-component` round-trip + a `copy-entity` diff).
- [ ] Root `CMakeLists.txt` builds engine + `editor-old` (headless host) + `tools/se`
      with no `editor/` vs `editor-old/` collision; a comment notes `editor/` builds
      via Bun/Tauri (not CMake).
- [ ] `docs/` has: an updated `build-and-run` how-to (Tauri run path), the
      `tauri-editor-and-x11-bridge` + `shared-types-pipeline` explanation pages with
      stated non-goals (undo/redo, parenting, multi-viewport, native Wayland), the
      retargeted `ui-and-editor/` pages, and updated hub `_index.md` rows; `hugo`
      builds with no broken links.
- [ ] `AGENTS.md` reflects the TS/Tauri editor as the default editor + the toolbox/Bun
      run recipe + the `editor-old/` headless-host role.
- [ ] `plans/typescript-ui-migration/` (all phase files + README) marked COMPLETED.

## Risks / seams

1. **Removing C++ editor code breaks the native path.** The native host reuses
   `EditorCamera`/`pickEntity`/gizmo op+space state/`submitNativeGizmo`/billboard
   overlay/`registerBuiltinComponents`/`setSelection`. Mitigation: delete one symbol
   at a time and rebuild `-j1` after each (steps 1→2→5 are ordered for exactly this);
   grep every candidate symbol for callers before deleting; never bulk-delete.
2. **Shader/asset compilation must survive the deletion.** All ~31 engine shaders
   (incl. `gizmo_overlay.slang`) + models/fonts/icons are emitted *only* by
   `editor-old/CMakeLists.txt`'s `saffron_compile_shaders` + POST_BUILD copy
   (`editor/CMakeLists.txt:11-26` today). Mitigation: this phase **does not touch**
   that target's compile/copy; it only deletes ImGui *panel* sources from the engine
   lib + `onUi`. Done-when asserts the shader/asset count parity.
3. **`editor_components.cpp` is not dead.** Its ImGui draw lambdas are dead under
   present-only, but `registerBuiltinComponents` wires the JSON serde the control
   path + `copy-entity` depend on. Do NOT delete it; do NOT strip its ImGui include
   (low value, breaks the registry build). The control inspector/save-load is the
   regression risk if this is mishandled — covered by a done-when round-trip check.
4. **CI must build C++ (toolbox) AND the Bun/Tauri frontend.** The frontend
   environment (host vs toolbox, webkit2gtk availability) is decided by the phase-3
   build-env spike; this phase only wires whatever that spike resolved. The native
   attach cannot be smoke-tested headless (no X server / parent window) — CI smokes
   present-only only; the full attach is a manual/interactive check.
5. **`drawGizmo` (ImGuizmo) vs `submitNativeGizmo` (overlay).** Both read
   `ctx.gizmoOp`. Delete `drawGizmo` only after confirming the native path uses the
   overlay submit, not ImGuizmo. Keep the shared gizmo op/space state on
   `EditorContext`.
6. **Naming churn vs clarity.** Keeping `editor-old/` for the headless host is
   intentional (minimizes diff, matches the worktree + shader GLOB path); the
   "old" label is a documentation problem solved in `AGENTS.md`/`editor/README.md`,
   not a rename. A future rename to `engine-host/` is a clean seam if desired.
7. **Docs hub inventory.** The `ui-and-editor/` pages are retargeted in place (not
   deleted) so the Diátaxis explanation inventory in the hub tables stays consistent;
   deleting them would leave dangling hub rows.
