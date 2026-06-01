# Phase 1: Engine viewport bridge forward-port + C++ editor relocation to editor-old/

**Status:** COMPLETED

<!-- Flip to COMPLETED when the "Done when" checklist passes, validation-clean. Delete this file only after COMPLETED + merged. -->

**Verified (2026-06-01):** Build validation-clean in the toolbox (`cmake --preset debug && cmake --build build/debug -j1` — all 73 steps, no errors). `editor-old/` builds `SaffronEditor`; `bin/shaders/*.spv` (29) + `models`/`fonts`/`icons` emitted. Headless (weston / Wayland / llvmpipe): the default ImGui editor boots + renders + exits clean (no regression); native present-only mode (`SAFFRON_EDITOR_NATIVE_VIEWPORT=1`) exercises `presentViewportToSwapchain` and exits clean via graceful `quit`; `se viewport-native-info` → `engine-window-ready`, `se help` lists all three viewport commands, `attach-native-viewport` without X11 returns the typed backend error. **Deferred:** the live `XReparentWindow` reparent + `resize-native-viewport` visual check needs an X11 server with a parent window (no Xvfb/Xwayland in the toolbox) — it is the integration test the phase-3 Tauri host performs, or run it on an X11 desktop.

## Goal

Forward-port the Tier-A engine bridge from the `explore-ui` MVP (worktree commit `faf704d`)
onto current `main` so `SaffronEditor` can run as a **headless native-viewport host**: it
reparents its own SDL/X11 window into a host XID and presents *only* the scene (no ImGui),
driven over the existing control socket. Resolve the directory collision up front by moving
the C++ editor to `editor-old/` so `editor/` is free for the Tauri app, while the
`SaffronEditor` target + its shader/asset compilation move intact and keep building.

This phase ports **only** the present-only blit + the three viewport control commands + the
present-only branch in `runEditor`. The native **gizmo overlay**, **billboards**, and
**pointer/keyboard input** are explicitly deferred to phase-4. The **fd-export / DMA-BUF**
path is NOT ported at all.

**Depends on:** none (first phase).

## Current state (verified)

### Directory layout (the collision)
- Root `CMakeLists.txt:31-33` does `add_subdirectory(engine)` / `add_subdirectory(editor)` /
  `add_subdirectory(tools/se)`. On `main`, `editor/` IS the C++ ImGui editor.
- `editor/CMakeLists.txt:1-26` builds the `SaffronEditor` executable from `editor/source/main.cpp`,
  links `Saffron::Engine`, and **owns all engine shader compilation + asset copy**:
  - `editor/CMakeLists.txt:11-13` `saffron_compile_shaders(SaffronEditor <src>/assets/shaders <runtime>/shaders)`.
  - `editor/CMakeLists.txt:16-26` POST_BUILD copies `assets/{models,fonts,icons}` to `SAFFRON_RUNTIME_DIR`.
- `editor/assets/shaders/` currently holds **28** `*.slang` files (verified `ls`: `mesh`, `tonemap`,
  `ibl_*`, `ddgi_*`, `restir_*`, `gtao`, `ssgi`, `taa`, `sky`, `thumbnail`, `light_cull`,
  `point_shadow`, `gbuffer`, `motion`, `contact`, `ao_blur`, `copy_color`, `fxaa`, `triangle`, …).
- `SAFFRON_RUNTIME_DIR = ${CMAKE_BINARY_DIR}/bin` (root `CMakeLists.txt:22`); `assetPath(...)`
  resolves relative to `SDL_GetBasePath()` (`renderer.cppm:1587-1597`), i.e. the exe dir = the
  runtime dir. So "SaffronEditor + its shader/asset compile" is one inseparable artifact.
- `editor/source/main.cpp` is a 6-line stub: `import Saffron.EditorApp; int main(){ return se::runEditor("Saffron Editor", 1600, 900); }`.
- The worktree resolved this as a **pure rename**: `faf704d` shows `{editor => editor-old}/CMakeLists.txt | 0`,
  `{editor => editor-old}/source/main.cpp | 0`, and every `assets/*` path moved with `| 0` (no content change),
  plus root `CMakeLists.txt` flipped `add_subdirectory(editor)` → `add_subdirectory(editor-old)`.

### Renderer (main's split renderer)
- `render_graph.cppm:58-65` `RgAttachment{ resource, loadOp, storeOp, clearValue, std::optional<RgResource> resolve }`.
- `render_graph.cppm:70-79` `RgPass` has `std::vector<RgAttachment> colors;` (MRT) + `std::optional<RgAttachment> depth;`.
  **This is CONFLICT 1**: the worktree used a single `RgPass.color` field; main uses a `colors` vector.
- `renderer.cppm:1456-1585` `endFrame` builds the `ui` pass with `ui.colors.push_back(RgAttachment{ swapImage, eClear, eStore, clearColor })`
  (`:1467-1468`), executes the graph, then either captures (`captureNextSwapchainPath`, `:1496-1520`)
  or transitions the swapchain `eColorAttachmentOptimal → ePresentSrcKHR` (`:1521-1528`), submits + presents.
- `renderer.cppm:1436-1454` `addTonemapPass` — the **mandatory** compute tonemap, added last inside
  `beginFrameGraph` (`renderer.cppm:1423`). The `ui` pass is the only swapchain writer and is added in `endFrame`.
- `renderer.cppm:97-104` device selection (`set_minimum_version(1,3)` + features) — main does **NOT** add
  `VK_KHR_EXTERNAL_MEMORY_FD` / `VK_EXT_EXTERNAL_MEMORY_DMA_BUF` (the worktree added them at `:99-100` for the
  fd path; we do not port them).
- `renderer_types.cppm:643-669` `Pipelines` (no `overlay` field). `renderer_types.cppm:956-1009` `Renderer`
  has `bool useDepthPrepass` (`:974`), `f32 exposureEv` (`:975`), `std::optional<std::string> captureNextSwapchainPath` (`:982`).
  There is **no** `presentViewportOnly` field, **no** `OverlayState`/`OverlayVertex`.
- `renderer_types.cppm:1005` declares `setViewportDesiredSize`, `:1008-1009` declare `viewportWidth`/`viewportHeight`.
- `OffscreenColorFormat = vk::Format::eR16G16B16A16Sfloat` (`renderer_types.cppm:34`), `MaxFramesInFlight = 2` (`:40`).

### Control plane
- `control_commands_render.cpp:1-13` includes `<nlohmann/json.hpp>`, `<unistd.h>`, `<string>`; `module Saffron.Control;`
  importing `Saffron.Core` + `Saffron.Rendering`. **No** SDL/X11 includes yet.
- `registerRenderCommands` registers `ping`/`help`/`render-stats`/`set-aa`/… via `registerCommand(reg, name, help, lambda)`.
  Render commands read params with `positionalOr(params, name, idx)` (`control_commands_render.cpp:66` etc.).
- `command.cppm:27-31` `struct EngineContext { Window& window; Renderer& renderer; … }`.
- `command.cppm:68` declares `auto controlSocketPath() -> std::string;`, defined at `control_server.cpp:147`.
  The socket resolves `SAFFRON_CONTROL_SOCK` → `XDG_RUNTIME_DIR/saffron-control.sock` → `/tmp` fallback.
- `window.cppm:21-25` `struct Window { SDL_Window* handle; u32 width; u32 height; … }`.
- The worktree's fd-export server (`startViewportServer`/`drainViewportServer`/`sendViewportFd`/`memfd`/`SCM_RIGHTS`,
  the second `.viewport` socket, the `viewportServer`/`viewportActive` fields in `command.cppm`) lived in
  `control_server.cpp` + `command.cppm`. **None of it is ported.** `control_server.cpp` + `command.cppm` stay clean on main.

### EditorApp
- `editor_app.cppm:76-83` `runEditor` → builds `AppConfig`, `config.onCreate = [state](se::App& app){ … }` (`:83`).
- `:85-87` `state->editor = newEditorContext(); state->control = newControlContext(); state->assets = newAssetServer(assetPath("assets"));`.
- `:255-275` auto-loads `project.json` if present, else imports `models/cube.gltf` and `spawnModel(scene,"Cube",…)`.
- `:277-377` builds a `Layer`; `layer.onUpdate` (`:279`) calls `pollControl(...)`; `layer.onUi` (`:289-376`) draws
  ALL ImGui panels AND drives `renderScene` (`:305`), `drawGizmo` (`:310`), `drawEditorBillboards` (`:317`),
  ray-pick (`:329-340`), then the Hierarchy/Environment/Assets/Inspector/Render-Stats panels.
- `:390-400` `config.onExit` releases `Ref`s; `app.cppm:183` calls `waitGpuIdle(app.renderer)` **before** `onExit`
  (`app.cppm:192-194`). `app.cppm:52-62` honors `SAFFRON_EXIT_AFTER_FRAMES`.
- The worktree's native path also added the gizmo overlay (`GizmoProjection`, `addLine`/`addBox`,
  `handleNativeGizmoPointer`, `submitNativeGizmo`) + the `NativeGizmoState` in `editor_context.cppm` — all of which
  belong to **phase-4**, not here.

### CMake / deps
- `cmake/Dependencies.cmake:9` `find_package(SDL3 REQUIRED CONFIG)`; `:100-113` `saffron_third_party` INTERFACE target
  links `SDL3::SDL3 Vulkan::Vulkan EnTT … imgui`. **No** `find_package(X11)` / `X11::X11` yet.

### se CLI / docs
- `tools/se/source/main.cpp:112-177` `printResult(cmd, result, mode)` — branches per `cmd` (`ping`, `list-entities`,
  `render-stats`, …) then a UTF-8 pretty-JSON fallback (`:175-176`). Unknown commands fall through to the JSON dump,
  so the new commands work without a formatter; we add concise ones anyway (se-current rule).
- `docs/content/reference/control-commands.md` is the command catalog: `## Scene commands` (`:11`),
  `## Render commands` (`:31`), `## Asset commands` (`:52`) markdown tables of `` | `cmd` | params | desc | ``.

## Implementation

Do every build inside the `saffron-build` toolbox: `toolbox run -c saffron-build bash -lc 'cd <repo> && cmake --build build/debug -j1'`.
Never `rm -rf build/debug` (the clang module BMI cache is expensive; reconfigure with `cmake --preset debug` only if CMake files changed).

### Step 1 — Move the C++ editor to `editor-old/` (pure rename, no content change)

1. `git mv editor editor-old` (moves `editor/CMakeLists.txt`, `editor/source/main.cpp`, `editor/assets/{shaders,models,fonts,icons}`).
2. Edit root `CMakeLists.txt:32`: `add_subdirectory(editor)` → `add_subdirectory(editor-old)`.
3. Do **not** touch `editor-old/CMakeLists.txt` — its relative paths (`${CMAKE_CURRENT_SOURCE_DIR}/assets/...`) already
   resolve correctly from the new dir, and `SAFFRON_RUNTIME_DIR` is an absolute root var so the emit target is unchanged.
4. Reconfigure (`cmake --preset debug`) since `add_subdirectory` changed, then build `SaffronEditor`. Confirm
   `build/debug/bin/shaders/*.spv` + `build/debug/bin/{models,fonts,icons}` still appear and the ImGui editor runs unchanged.

> The `SaffronEditor` target name, its source, its `saffron_compile_shaders` call, and the asset copy all stay byte-identical;
> only the directory and the one root `add_subdirectory` line change. This is the inseparable "headless host" artifact and it
> must keep building for the rest of the migration (it is the viewport host the Tauri app spawns).

### Step 2 — `X11::X11` linkage in `cmake/Dependencies.cmake`

1. After `cmake/Dependencies.cmake:9` (`find_package(SDL3 …)`), add:
   `find_package(X11 REQUIRED)   # X11 child-window embedding for the native-viewport bridge`.
2. In the `saffron_third_party` INTERFACE link list (`:100-113`), add `X11::X11` (alongside `SDL3::SDL3`).
3. Reconfigure. (`X11::X11` brings `Xlib.h` + `libX11` for `XReparentWindow`/`XMoveResizeWindow`/`XMapRaised`/`XFlush`.)

### Step 3 — `presentViewportOnly` + `presentViewportToSwapchain` (renderer)

In `renderer_types.cppm`:
1. Add to `struct Renderer` (near `useDepthPrepass`/`exposureEv`, `:974-975`):
   `bool presentViewportOnly = false;`
2. Declare the new free functions near `endFrame` (`:1002`):
   `void setPresentViewportOnly(Renderer& renderer, bool enabled);`
   (Do **not** add `OverlayState`/`OverlayVertex`/`submitOverlay`/`newOverlayPipeline` — those are phase-4.)

In `renderer.cppm`:
3. Add `#include <cstring>` is **not** needed in phase-1 (it was only for the overlay memcpy). Skip it.
4. Add the setter near `endFrame` (mirror the worktree at its `setPresentViewportOnly`):
   ```cpp
   void setPresentViewportOnly(Renderer& renderer, bool enabled)
   {
       renderer.presentViewportOnly = enabled;
   }
   ```
5. Add `presentViewportToSwapchain(Renderer&, vk::CommandBuffer)` — port verbatim from the worktree
   (`renderer.cppm` present-only block). It:
   - transitions `targets.offscreen` (its tracked `src.layout`) → `eTransferSrcOptimal`
     (srcStage `eComputeShader | eColorAttachmentOutput`, srcAccess `eShaderStorageWrite | eColorAttachmentWrite`;
     dstStage `eTransfer`, dstAccess `eTransferRead`) and writes back `src.layout = eTransferSrcOptimal`;
   - transitions `swapchain.images[frame.imageIndex]` `eUndefined → eTransferDstOptimal`;
   - `cmd.blitImage(offscreen, eTransferSrcOptimal, swap, eTransferDstOptimal, blit, vk::Filter::eNearest)`
     where `blit` maps `offscreen.extent` → `swapchain.extent`;
   - transitions swap `eTransferDstOptimal → ePresentSrcKHR` (srcStage `eTransfer`, dstStage `eBottomOfPipe`).
   Use the existing `transitionImage(...)` helper (same one `endFrame` uses).

### Step 4 — Wire present-only into `endFrame` (CONFLICT 1 + double-present guard)

Edit `renderer.cppm:1456-1585` `endFrame`:
1. Wrap the `ui` pass (`:1463-1477`) in `if (!renderer.presentViewportOnly) { … }`. Keep the existing
   `ui.colors.push_back(RgAttachment{ swapImage, … })` form (**CONFLICT 1**: main uses the `colors` vector, not `.color`).
   In present-only mode no pass writes the swapchain via the graph — `presentViewportToSwapchain` does the blit instead.
2. After `executeRenderGraph(graph, frame.commandBuffer)` (`:1479`), before the capture block, force capture off in
   present-only mode (the swapchain is never in `eColorAttachmentOptimal`, so the capture/transition path is wrong):
   ```cpp
   bool doCapture = renderer.captureNextSwapchainPath.has_value();
   if (renderer.presentViewportOnly) { doCapture = false; }
   ```
   (Insert the `if` right after the existing `doCapture` initializer at `:1496`.)
3. Replace the capture-vs-transition branch (`:1511-1528`) so present-only blits instead:
   ```cpp
   if (renderer.presentViewportOnly)
   {
       presentViewportToSwapchain(renderer, frame.commandBuffer);
   }
   else if (doCapture)
   {
       captureImageToBuffer(/* …unchanged… */);
   }
   else
   {
       transitionImage(/* …unchanged eColorAttachmentOptimal → ePresentSrcKHR… */);
   }
   ```
   This keeps a **single** swapchain writer per frame (either the graph `ui` pass, or the present-only blit — never both),
   resolving the "double-present" risk. The submit/present tail (`:1530-1559`) is unchanged; present-only relies on the same
   `signalSemaphore` + `presentKHR`.

### Step 5 — `viewport-native-info` + `attach-native-viewport` + `resize-native-viewport` (control)

Edit `control_commands_render.cpp`:
1. Extend the global-module-fragment includes (after `<nlohmann/json.hpp>`, before `<unistd.h>`):
   ```cpp
   #include <SDL3/SDL.h>
   #include <X11/Xlib.h>
   ```
   and add `<algorithm>`, `<charconv>`, `<cstdlib>`, `<format>` to the std includes (for `std::max`, `std::from_chars`, `std::format`).
   > Note: `control_commands_render.cpp` already lives in `module Saffron.Control;` and does **not** `import std`, so the
   > Vulkan-free SDL/X11 C headers are safe in the GMF here (same pattern as the rest of the control TUs).
2. Register `viewport-native-info` (port verbatim; returns the `ViewportNativeInfo` DTO):
   ```cpp
   registerCommand(reg, "viewport-native-info", "native viewport bridge status",
       [](EngineContext& ctx, const json&) -> Result<json>
       {
           return json{
               { "platform", "linux" },
               { "transport", "x11-child-window" },
               { "status", "engine-window-ready" },
               { "controlSocket", controlSocketPath() },
               { "width", viewportWidth(ctx.renderer) },
               { "height", viewportHeight(ctx.renderer) },
               { "message", "engine SDL/Vulkan window can be reparented into an X11 host" }
           };
       });
   ```
3. Register `attach-native-viewport` (port verbatim from the worktree `control_commands_render.cpp` diff). Key points:
   - `readU64("parentXid")` accepts unsigned, non-negative signed, **or string** (so JS `string` u64 ids round-trip —
     `std::from_chars`). Returns `Err("expected numeric parentXid")` otherwise.
   - `readI32("x"|"y"|"width"|"height", fallback)` with `width`/`height` defaulting to `ctx.window.width/height`, clamped `>= 1`.
   - `SDL_SetWindowBordered(false)` → `SDL_SetWindowSize` → `SDL_SetWindowPosition` → `SDL_SyncWindow`.
   - `SDL_GetWindowProperties(ctx.window.handle)`; read `Display*` via `SDL_PROP_WINDOW_X11_DISPLAY_POINTER` and
     `::Window` via `SDL_PROP_WINDOW_X11_WINDOW_NUMBER`. If either is null/0, return
     `Err("engine window is not using SDL's X11 backend; run with SDL_VIDEODRIVER=x11")`.
   - `XReparentWindow(display, child, (::Window)*parent, x, y)` → `XMoveResizeWindow` → `XMapRaised` → `XFlush`.
   - Update `ctx.window.width/height`, then `setViewportDesiredSize(ctx.renderer, w, h)` + `setPresentViewportOnly(ctx.renderer, true)`.
   - Return the `AttachResult` DTO `{ attached:true, transport:"x11-child-window", x, y, width, height }`.
4. **NEW (not in the worktree)** — register `resize-native-viewport`. The worktree's `resize` re-sent the *full* attach
   every tick (re-`XReparent` → flicker, `lib.rs:343-364`). This command does **move/resize only** — no reparent, no remap:
   ```cpp
   registerCommand(reg, "resize-native-viewport",
       "resize-native-viewport {x,y,width,height} — move/resize the reparented child (no reparent)",
       [](EngineContext& ctx, const json& params) -> Result<json>
       {
           // reuse the same readI32 helper as attach-native-viewport
           const i32 x = readI32("x", 0);
           const i32 y = readI32("y", 0);
           const i32 width  = std::max(1, readI32("width",  static_cast<i32>(ctx.window.width)));
           const i32 height = std::max(1, readI32("height", static_cast<i32>(ctx.window.height)));

           SDL_SetWindowSize(ctx.window.handle, width, height);
           SDL_SetWindowPosition(ctx.window.handle, x, y);

           SDL_PropertiesID props = SDL_GetWindowProperties(ctx.window.handle);
           Display* display = static_cast<Display*>(
               SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr));
           const ::Window child = static_cast<::Window>(
               SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
           if (display == nullptr || child == 0)
           {
               return Err(std::string{ "engine window is not using SDL's X11 backend; run with SDL_VIDEODRIVER=x11" });
           }
           XMoveResizeWindow(display, child, x, y,
               static_cast<unsigned int>(width), static_cast<unsigned int>(height));
           XFlush(display);

           ctx.window.width  = static_cast<u32>(width);
           ctx.window.height = static_cast<u32>(height);
           setViewportDesiredSize(ctx.renderer, static_cast<u32>(width), static_cast<u32>(height));
           return json{ { "resized", true }, { "x", x }, { "y", y }, { "width", width }, { "height", height } };
       });
   ```
   - Do **not** call `setPresentViewportOnly` here (attach already latched it true) and do **not** `XReparentWindow`/`XMapRaised`.
   - Factor `readI32`/`readU64` so `attach-native-viewport` + `resize-native-viewport` share them (file-local helpers or
     lambdas duplicated in each — keep it simple, the worktree inlined lambdas).

> Do **NOT** port `click-viewport`, `set-gizmo-mode`, or `set-gizmo-space` here. `click-viewport` was a demo that toggled
> `MaterialComponent.baseColor` (replaced by `pick` + `set-material` in phase-2); the gizmo commands belong to phase-2/4.

### Step 6 — Present-only branch in `editor_app.cppm` `runEditor` (no gizmo/billboards/input)

1. After `auto state = std::make_shared<EditorState>();` (`editor_app.cppm:78`), read the env flag:
   ```cpp
   const bool nativeViewportHost = std::getenv("SAFFRON_EDITOR_NATIVE_VIEWPORT") != nullptr;
   ```
   Add `#include <cstdlib>` to the GMF includes if not already present.
2. Capture it into `config.onCreate` (`:83`): `config.onCreate = [state, nativeViewportHost](se::App& app)`.
3. After `state->assets = se::newAssetServer(...)` (`:87`), latch present-only:
   ```cpp
   const bool nativeViewport = nativeViewportHost;
   se::setPresentViewportOnly(app.renderer, nativeViewport);
   ```
4. After the auto-load/seed block (`:255-275`), in native mode auto-select the first mesh entity so the embedded viewport
   has something selected (port the worktree's `forEach<MeshComponent>` → first → `setSelection`):
   ```cpp
   if (nativeViewport)
   {
       se::Entity renderable{ entt::null };
       se::forEach<se::MeshComponent>(state->editor->scene,
           [&renderable](se::Entity entity, se::MeshComponent&)
           {
               if (renderable.handle == entt::null) { renderable = entity; }
           });
       if (renderable.handle != entt::null) { se::setSelection(*state->editor, renderable); }
   }
   ```
5. Capture `nativeViewport` into `layer.onUi` (`:289`) and add an **early present-only branch** at its top
   (before `drawEditorMenuBar`), porting the worktree minus the gizmo:
   ```cpp
   state->editor->scene.catalog = &state->assets.catalog;
   if (nativeViewport)
   {
       se::setViewportDesiredSize(app.renderer, app.window.width, app.window.height);
       se::updateEditorCamera(state->editor->camera, false, ImGui::GetIO().DeltaTime);
       se::CameraView cam = se::editorCameraView(state->editor->camera);
       if (app.window.width > 0 && app.window.height > 0)
       {
           se::renderScene(app.renderer, state->editor->scene, state->assets, cam);
           // submitNativeGizmo(...) is phase-4; no gizmo/billboards/ray-pick here.
       }
       return;  // skip ALL ImGui panels in present-only mode
   }
   ```
   - `updateEditorCamera(..., false, ...)`: the `false` is `viewportHovered` — input routing is phase-3/4, so the
     fly-cam does not move yet in native mode; the scene still renders through the editor camera at its default pose.
   - **Do NOT** add the `app.window.eventSinks.push_back(handleNativeGizmoPointer …)` block from the worktree (`:659-668`) —
     it pulls in the phase-4 gizmo input path and `pickEntity`/`MaterialComponent` toggling. Phase-1 has no native input.
   - `pollControl` in `layer.onUpdate` (`:283`) is unchanged — the control socket already drains every frame, which is
     how `se attach-native-viewport` reaches the engine.
6. Leave the non-native `layer.onUi` body (`:295-376`) and `config.onExit` (`:390-400`) exactly as-is. When
   `SAFFRON_EDITOR_NATIVE_VIEWPORT` is unset, `runEditor` is byte-for-byte the old ImGui editor.

> Do **not** edit `editor_context.cppm` (the `NativeGizmoState` additions are phase-4) and do **not** edit `command.cppm`
> or `control_server.cpp` (the fd-server fields are not ported).

### Step 7 — se CLI formatters + docs

1. `tools/se/source/main.cpp` `printResult` (`:166-174` region): add concise text branches (optional but per se-current rule):
   ```cpp
   if (cmd == "viewport-native-info")
   {
       std::printf("%s  %s  %ux%u  sock=%s\n",
           result.value("status", "").c_str(), result.value("transport", "").c_str(),
           result.value("width", 0u), result.value("height", 0u),
           result.value("controlSocket", "").c_str());
       return;
   }
   if (cmd == "attach-native-viewport" || cmd == "resize-native-viewport")
   {
       std::printf("%s  %dx%d @ (%d,%d)\n",
           result.value("attached", false) || result.value("resized", false) ? "ok" : "fail",
           result.value("width", 0), result.value("height", 0),
           result.value("x", 0), result.value("y", 0));
       return;
   }
   ```
   (Anything not handled still falls through to the JSON dump at `:175-176`, so this is purely for readable `-o text`.)
2. `docs/content/reference/control-commands.md`: add three rows. Put them under `## Render commands` (after the existing
   render rows, before `## Asset commands`):
   ```md
   | `viewport-native-info` | — | native-viewport bridge status `{platform, transport, status, controlSocket, width, height, message}` |
   | `attach-native-viewport` | `{parentXid, x?, y?, width?, height?}` | reparent the engine SDL/X11 window into the host XID; latches present-only. `parentXid` may be a string (u64) |
   | `resize-native-viewport` | `{x, y, width, height}` | move/resize the already-reparented child (no reparent, no flicker) |
   ```
   Also note in prose near the table: these require launching `SaffronEditor` with `SAFFRON_EDITOR_NATIVE_VIEWPORT=1`,
   `SAFFRON_CONTROL_SOCK=<sock>`, `SDL_VIDEODRIVER=x11`, and that under present-only `screenshot target=window` is disabled
   (use `target=viewport`).
3. Build the `se` target (C++20, no modules): `cmake --build build/debug --target se`.

### Step 8 — Build + manual verification (toolbox)

1. `cmake --preset debug` (CMake files changed), then `cmake --build build/debug -j1`. Expect validation-clean.
2. Default (ImGui) run still works: `./build/debug/bin/SaffronEditor` (no env) → the unchanged editor.
3. Native present-only smoke without a host XID — confirm it presents the scene (will present into its own window since the
   blit targets the swapchain even before attach):
   ```sh
   SAFFRON_EDITOR_NATIVE_VIEWPORT=1 SAFFRON_CONTROL_SOCK=/tmp/se.sock SDL_VIDEODRIVER=x11 \
   SAFFRON_EXIT_AFTER_FRAMES=10 ./build/debug/bin/SaffronEditor
   # exits clean (waitGpuIdle before onExit), no leak abort
   ```
4. Reparent smoke against a real XID (use any X11 window id from `xwininfo`, or a throwaway `xterm`):
   ```sh
   SAFFRON_EDITOR_NATIVE_VIEWPORT=1 SAFFRON_CONTROL_SOCK=/tmp/se.sock SDL_VIDEODRIVER=x11 ./build/debug/bin/SaffronEditor &
   SAFFRON_CONTROL_SOCK=/tmp/se.sock ./build/debug/bin/se viewport-native-info        # status engine-window-ready + sock
   SAFFRON_CONTROL_SOCK=/tmp/se.sock ./build/debug/bin/se attach-native-viewport --parentXid <XID> --x 0 --y 0 --width 800 --height 600
   SAFFRON_CONTROL_SOCK=/tmp/se.sock ./build/debug/bin/se resize-native-viewport --x 0 --y 0 --width 1024 --height 768
   ```
   Expect: after `attach`, the engine window reparents over `<XID>` showing only the 3D scene (no ImGui); after `resize`,
   it moves/resizes with no reparent flicker.

## Done when

- [ ] `cmake --preset debug && cmake --build build/debug -j1` succeeds in the toolbox with no new validation errors.
- [ ] `editor-old/` builds `SaffronEditor`; `build/debug/bin/{shaders/*.spv,models,fonts,icons}` are all emitted as before.
- [ ] `./build/debug/bin/SaffronEditor` (no env vars) runs the C++ ImGui editor **unchanged** (all panels, gizmo, picking).
- [ ] Launching `SaffronEditor` with `SAFFRON_EDITOR_NATIVE_VIEWPORT=1 + SAFFRON_CONTROL_SOCK + SDL_VIDEODRIVER=x11` and
      `se attach-native-viewport --parentXid <real XID>` reparents the engine window over it showing **only** the 3D scene.
- [ ] `se resize-native-viewport` moves/resizes the child window with **no** reparent flicker.
- [ ] `se viewport-native-info` returns `status engine-window-ready` + the control socket path.
- [ ] `SAFFRON_EXIT_AFTER_FRAMES=N` bounded present-only run exits clean (`waitGpuIdle` before `onExit`, no leak abort).
- [ ] `se help` lists `viewport-native-info`, `attach-native-viewport`, `resize-native-viewport`; the docs reference table
      has matching rows.
- [ ] `control_server.cpp` + `command.cppm` are unchanged (no fd-server/viewport-server fields); no external-memory device
      extensions were added (`renderer.cppm:97-104` device selection is unchanged from main).

## Risks / seams

- **CONFLICT 1 (MRT colors vector).** Main's `RgPass` uses `std::vector<RgAttachment> colors` (`render_graph.cppm:75`),
  not the worktree's single `.color`. The present-only edit only *guards* the existing `ui.colors.push_back(...)` in an
  `if (!presentViewportOnly)` and adds no new attachment, so the conflict surface is small — but verify the guard keeps the
  `ui` pass as the lone swapchain writer in non-native mode (post-tonemap `sceneColor` sampled into `swapImage`).
- **Double-present.** In present-only mode the graph must not write the swapchain (no `ui` pass) and `presentViewportToSwapchain`
  must be the only `…→ePresentSrcKHR` transition. Step 4's `if/else if/else` enforces exactly one swapchain writer per frame.
- **SDL3 3.4.8 X11 property names.** Relies on `SDL_PROP_WINDOW_X11_DISPLAY_POINTER` + `SDL_PROP_WINDOW_X11_WINDOW_NUMBER`
  and `SDL_VIDEODRIVER=x11`. If null/0, the command returns a typed error (Wayland/non-X11 backend). XWayland-only is accepted.
- **Window screenshots break under present-only** (the swapchain is never `eColorAttachmentOptimal`). Step 4 disables window
  capture in present-only; `screenshot target=viewport` (via `captureViewport`) still works. Document this in the reference page.
- **`editor` → `editor-old` move.** Keep it a pure `git mv` + the one root `add_subdirectory` line; do not edit
  `editor-old/CMakeLists.txt`. The shader GLOB (`CompileShaders.cmake` globs `*.slang` from the passed dir) and
  `SAFFRON_RUNTIME_DIR` (absolute) are unaffected. Reconfigure after the move (the subdir set changed).
- **Deferred to later phases (do not start here):** native gizmo overlay + `OverlayState`/`newOverlayPipeline`/`submitOverlay`
  + `gizmo_overlay.slang` + `editor_context.cppm` `NativeGizmoState` + billboards + pointer/keyboard input (phase-4);
  `click-viewport`/`set-gizmo-*` replacement (phase-2); the entire fd-export/DMA-BUF server (not ported at all).
