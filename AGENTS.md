# SaffronEngine

A from-scratch **Vulkan** renderer / **C++26** game engine with an ImGui-based
editor. This is a clean-slate rewrite (branch `main`) of an older DirectX 11 /
premake engine; the prior code lives on `old-master`, `rework`, and the various
experiment branches and is kept only for reference.

The design deliberately preserves the *API shape* that worked in the old engine
(an `App`/`Layer` lifecycle, a deferred `submit(lambda)` render seam, a frame
graph, an entt scene, signal/slot events) while dropping everything DX11-specific
and all heavy OOP. See `CONVENTIONS.md` for the coding style — it is **not
optional**, the whole codebase follows it.

**Comments are minimal** (see `CONVENTIONS.md` → *Comments*): no inline noise, **no
section/banner dividers ever**, brief `///` doc comments on exported declarations,
and no change-journey notes ("previously/used to/refactor/now that…"). Say what the
code does now — and *why* if it's non-obvious — never by contrast with the past.

---

## TL;DR for a new session

- **You cannot build on the host.** This is Fedora **Silverblue** (immutable);
  there is no `g++`/`cmake`/Vulkan SDK on the host by design. Everything builds
  inside the **`saffron-build`** toolbox container.
- Run any build/test command via:
  `toolbox run -c saffron-build bash -lc '<command>'`
  The home directory is shared, so files edited on the host are seen in the toolbox.
- Configure + build + run:
  ```sh
  toolbox run -c saffron-build bash -lc '
    cd /var/home/saffronjam/repos/SaffronEngine
    cmake --preset debug           # first time / after CMake changes
    cmake --build build/debug -j1  # -j1: parallel builds intermittently hit a clang module-BMI ICE
    ./build/debug/bin/SaffronEditor
  '
  ```
- For automated/headless verification, bound the run:
  `SAFFRON_EXIT_AFTER_FRAMES=5 ./build/debug/bin/SaffronEditor` exits after N frames.
- **No display?** The editor needs a Wayland/X display. In the toolbox run a headless compositor —
  `weston --backend=headless --width=1280 --height=720 --socket=wl-x --idle-time=0 &`, then
  `export WAYLAND_DISPLAY=wl-x SDL_VIDEODRIVER=wayland` and launch under it — for a real swapchain
  that `se screenshot` can read. Use a unique `--socket` + `SAFFRON_CONTROL_SOCK` per run, and
  capture the editor exit code to a file *before* any `pkill` (the toolbox wrapper surfaces the
  pkill signal, not the engine's real exit code).
- **Do not write to Claude's `~/.claude/.../memory/` stores** (project-scoped or user-level). Keep
  durable project knowledge in the repo instead — this `AGENTS.md`, `CONVENTIONS.md`, or a `plans/`
  file — so it is version-controlled and shared. If a fact is worth saving, put it here; otherwise
  don't. Nothing in this repo should reference an out-of-repo path for project knowledge.

---

## Tech stack (all current as of 2026-05)

| Area | Choice | Version | Notes |
|------|--------|---------|-------|
| Language | C++26 | `-std=c++26` (gnu) | Named modules + `import std` |
| Compiler | Clang + libc++ | 21.1.8 | libc++ ships the `std` module; GCC 16 isn't in F43 |
| Build | CMake + Ninja | 3.31 / 1.13 | FetchContent for vendored static deps |
| Windowing/input | SDL3 | 3.4.8 | System package (C ABI) |
| Vulkan | **Vulkan-Hpp (`vk::`)** | headers 1.4.341, target **1.3** | dynamic rendering + synchronization2 |
| Vulkan bootstrap | vk-bootstrap | 1.4.352 | instance/device/swapchain selection |
| GPU allocation | VMA | 3.3.0 | one impl TU in `cmake/vma_impl.cpp` |
| ECS | EnTT | 3.16.0 | scene/entity + value components |
| UI | Dear ImGui | 1.92.8-**docking** | `imgui_impl_sdl3` + `imgui_impl_vulkan`, dynamic rendering |
| Gizmo | ImGuizmo | master | compiled into the `imgui` lib; in-viewport TRS manipulation |
| Shaders | Slang | 2026.10 | `slangc -target spirv`, compiled in CMake |
| Math | GLM | 1.0.1 | |
| Serialization | nlohmann/json | 3.12.0 | `JSON_NOEXCEPTION`; scene save/load |
| Screenshots | stb_image_write | 1.16 | vendored `third_party/stb` + `cmake/stb_impl.cpp` |
| Texture decode | stb_image | 2.30 | vendored alongside; decodes albedo PNG/JPG |
| glTF import | cgltf | 1.15 | vendored single header + impl TU; no-throw C API |
| OBJ import | tinyobjloader | 1.0.6 | vendored single header + impl TU; no-throw bool API |

**Vulkan via Vulkan-Hpp (`vk::`) with `VULKAN_HPP_NO_EXCEPTIONS`** — every call
returns a result we convert to `std::expected` and check immediately. We do **not**
use `vk::raii` (it throws). Instead, data-plane resources are owned by small **RAII
meta-layer** wrapper types (e.g. `Pipeline`: move-only, destructor frees its `vk::`
handles). The renderer owns these (e.g. `std::vector<Pipeline>`) and frees them
before the device. `volk` is not used (we link the system loader); it can be added
later as a dispatch optimization.

---

## Build environment (Silverblue + toolbox)

- Host: Fedora **Silverblue 43**, ostree-booted, `/var/home/saffronjam`. No C++
  toolchain on the host.
- Toolbox `saffron-build` (from `fedora-toolbox:43`) has: clang/clang++ 21,
  libc++/libc++abi 21, cmake 3.31, ninja, lld, vulkan-headers/loader-devel/
  validation-layers/tools 1.4.341, SDL3-devel 3.4.8, glslc/glslang/spirv-tools,
  g++ 15 (fallback only). Slang prebuilt is under `~/.cache/saffron-slang/`.
- GPU in the toolbox is currently **llvmpipe** (Mesa software Vulkan 1.4) — fine
  for correctness/validation. Install `mesa-vulkan-drivers` in the toolbox for
  hardware acceleration.
- `import std` requires the experimental CMake gate (UUID is CMake-3.31-specific,
  set in the root `CMakeLists.txt`) and `CMAKE_CXX_MODULE_STD ON` per target.
  **Do not** set `CMAKE_CXX_EXTENSIONS OFF` — the internal std module builds as
  `gnu++26` and consumers must match, or the BMI is rejected.

The toolchain is driven by `CMakePresets.json` (`debug` / `release` presets pin
`clang++`, `-stdlib=libc++`, `-fuse-ld=lld`, Ninja).

---

## Layout

```
SaffronEngine/
├── CMakeLists.txt          # root: import-std gate, C++26, includes Dependencies, subdirs
├── CMakePresets.json       # clang/libc++ debug+release presets
├── CONVENTIONS.md          # Go-flavored C++ rules (authoritative)
├── cmake/
│   ├── Dependencies.cmake  # FetchContent deps + imgui/vma targets + saffron_third_party
│   └── vma_impl.cpp        # the single VMA_IMPLEMENTATION translation unit
├── engine/
│   ├── CMakeLists.txt      # SaffronEngine static lib (FILE_SET CXX_MODULES)
│   └── source/saffron/
│       ├── core/core.cppm        # module Saffron.Core  — aliases, TimeSpan, logging
│       ├── json/json.cppm        # module Saffron.Json  — error-as-value gateway over nlohmann (no abort)
│       ├── signal/signal.cppm    # module Saffron.Signal — SubscriberList<...> signal/slot
│       ├── window/window.cppm    # module Saffron.Window — SDL3 window + typed event signals
│       ├── geometry/geometry.cppm  # module Saffron.Geometry — Vertex/Mesh/Submesh, glTF+OBJ import, .smesh
│       ├── scene/scene.cppm      # module Saffron.Scene — entt ECS + ComponentRegistry + JSON serialization
│       ├── rendering/render_graph.cppm  # partition Saffron.Rendering:RenderGraph — declared-usage → auto barriers
│       ├── rendering/renderer.cppm  # module Saffron.Rendering — Vulkan device/swapchain + render-graph frame + submit() seam
│       ├── ui/ui.cppm            # module Saffron.Ui — ImGui docking (SDL3 + Vulkan backends) + Viewport
│       ├── assets/assets.cppm    # module Saffron.Assets — AssetServer (Uuid→mesh registry) + importModel + renderScene
│       ├── editor/editor.cppm    # module Saffron.Editor — hierarchy + generic inspector + component registration
│       ├── control/control.cppm  # module Saffron.Control — unix-socket control plane (commands + screenshots)
│       └── app/app.cppm          # module Saffron.App — App/Layer/AppConfig + run() main loop
├── editor/
│   ├── CMakeLists.txt      # SaffronEditor executable
│   ├── assets/models/      # source models (cube.gltf/.obj), copied next to the exe
│   └── source/main.cpp     # client app: builds AppConfig, attaches a Layer, calls se::run()
├── tools/se/               # the `se` control CLI (json over the unix socket; no engine dep)
└── plans/                  # phased implementation plans for FUTURE expansions (not yet built)
```

Modules form a DAG (real imports, not a single chain): `Signal→Core`, `Json→Core`,
`Window→{Core,Signal}`, `Geometry→{Core}`, `Scene→{Core,Json}`, `Rendering→{Core,Window,Geometry}`
(with a `:RenderGraph` partition), `Ui→{Core,Window,Rendering}`, `Assets→{Core,Json,Geometry,Rendering,Scene}`,
`Editor→{Core,Signal,Scene,Json,Ui}`, `Control→{Core,Json,Window,Rendering,Scene,Editor,Assets}`, `App→{Core,Window,Rendering,Ui}`.
The editor exe links `Saffron::Engine` and imports the modules it needs (Core/App/Window/Rendering/Ui/Editor/Control/Scene/Assets).

### Module conventions
- One namespace: `se`. Engine modules are named `Saffron.<Area>`.
- `core`/`signal`/`app` use `import std`. `window` uses `import std` + the SDL3 **C**
  header (safe — C headers don't clash with the std module).
- `rendering`, `ui`, and `scene` wrap heavy **C++** third-party headers (Vulkan +
  vk-bootstrap + VMA, ImGui, entt + glm), so they use **classic `#include` in the
  global module fragment and do NOT `import std`** — mixing `import std` with a heavy
  C++ header in one TU breaks. `geometry` (cgltf + tinyobjloader + glm), `json` (nlohmann),
  `assets`, `editor`, and `control` follow the same rule, and the editor TU (`main.cpp`)
  includes `<imgui.h>` the same way. These modules are still consumed normally by the `import std` modules —
  the BMI carries the std types.

---

## Architecture (the preserved "concept", Go-style)

- **Lifecycle:** the client fills an `se::AppConfig` (window config + `onCreate` /
  `onExit` closures) and calls `se::run(config)`. `run` owns the main loop:
  poll events → update layers → `beginFrame` → ImGui → record layer UI → `endFrame`
  (present).
- **Layer = struct of closures** (`onAttach/onUpdate/onUi/onDetach`), *not* a
  virtual base. `attachLayer(app, layer)` pushes it. This is the Go-interface-as-
  itable pattern.
- **Renderer seam:** `submit(renderer, [](VkCommandBuffer cmd){ ... })` records a
  closure into the current frame; `endFrame` replays them inside the dynamic-
  rendering pass. This is the backend-agnostic seam from the old engine (a D3D11
  context became a `VkCommandBuffer`).
- **Events:** `SubscriberList<Args...>` is the engine-wide signal/slot
  (`subscribe(handler) -> SubscriptionId`, handler returns `true` to stop
  propagation). `Window` exposes typed signals (`onResize`, `onKeyPressed`, …) and
  a raw `eventSinks` list (ImGui feeds off that).
- **Errors:** fallible functions return `std::expected<T, std::string>`. No
  exceptions in engine code.

---

## Current status

Working and verified (validation-clean) in the toolbox:
- ✅ Build system + all vendored deps under Clang 21 + libc++ + `import std`.
- ✅ SDL3 window + Go-style App/Layer lifecycle + signal/slot events.
- ✅ Vulkan 1.3 via Vulkan-Hpp `vk::` (no-exceptions): device/swapchain (vk-bootstrap),
  VMA allocator, sync2 + dynamic rendering, clears + presents, swapchain recreation,
  per-image-fence sync.
- ✅ RAII meta-layer wrappers (`Pipeline`, `Image`, `GpuMesh`, `GpuTexture`, `Buffer`), move-only,
  passed around as **`Ref<T>` = `std::shared_ptr<T>`** logical objects (no opaque handles, no base class).
  Factories return `std::expected<Ref<T>, ...>`; the dtor frees the `vk::`/VMA resource when the last `Ref`
  drops. Teardown contract: the client releases its `Ref`s in `onExit` and `run` calls `waitGpuIdle` first,
  so nothing outlives `vmaDestroyAllocator`. See `meta-layer-resources` memory.
- ✅ Slang shaders compiled to SPIR-V in CMake → graphics + compute pipelines, recorded via the
  `submit(lambda)` seam / `onRender` hook.
- ✅ Two-pass frame: scene → offscreen `Image`, then ImGui → swapchain. The scene shows
  in a dockable **Viewport** panel (`ImGui_ImplVulkan_AddTexture`, 1.92.8 no-sampler;
  generation-counter descriptor refresh; 1-frame-lag resize). `SAFFRON_CAPTURE=path`
  dumps the offscreen image to a PNG.
- ✅ ImGui docking (SDL3 + Vulkan backends, dynamic rendering).
- ✅ entt `Scene`/`Entity` + value components + `forEach`.
- ✅ **Modular `ComponentRegistry`** (struct-of-closures itable; `registerComponent<C>`) driving
  registry-based **JSON scene save/load** + the editor — adding a component is one `registerComponent`
  call, no central edits. See `ecs-architecture` memory.
- ✅ Editor: **Hierarchy** + generic **Inspector** (add/remove component) + File save/load; selection
  via `SubscriberList<Entity>`.
- ✅ **Control plane** (`Saffron.Control`) + the `se` CLI: a non-blocking unix socket, drained per
  frame on the main thread, drives the running editor (list/create/destroy/select entities,
  add/remove/set component, set-transform, set-material, save/load scene + save/load project,
  import-model/texture, render-stats, set-clustered, pick, inspect, list-assets, rename-asset,
  assign-asset, focus, screenshot viewport|window to PNG, quit). See `control-plane` memory.

> **Keep `se` current.** When a feature adds engine state worth driving or inspecting, add a matching
> control command (one `registerCommand` in `control.cppm`) so the running editor stays scriptable and
> visually debuggable from the CLI. Treat it as part of "done" for a feature, not an afterthought.

- ✅ **Model import + mesh rendering**: `Saffron.Geometry` imports glTF (cgltf) + OBJ (tinyobjloader) into a
  common `Mesh`, baked to a versioned `.smesh`; `GpuMesh` (VMA vertex/index buffers) + a depth-tested mesh
  pipeline; `Saffron.Assets` (an `AssetServer` that owns the asset catalog + Uuid→GPU caches) + `renderScene`
  draw the ECS scene (`MeshComponent` + `CameraComponent`). Import via `se import-model`, File ▸ Import, or
  drag-and-drop. See `mesh-asset-pipeline` + `asset-catalog` memories.
- ✅ **Materials + textures**: descriptor sets (set 0 = albedo combined image sampler) + a shared sampler +
  `GpuTexture`/`uploadTexture`; a per-entity `MaterialComponent` (base color + albedo `Uuid`); albedo textures
  imported from glTF/OBJ (or `se import-texture`), copied into `assets/textures/`, sRGB, persisted + reloaded
  cross-process. `se set-material`.
- ✅ **Instanced scene rendering**: `renderScene` buckets `(mesh, albedo)` and draws each bucket as one
  instanced `drawIndexed` from a per-frame instance SSBO (set 2) indexed by `SV_VulkanInstanceID`
  (= `firstInstance + instance`; plain `SV_InstanceID` is base-relative). Per-instance model + normal matrix +
  base color. `se render-stats` reports draw calls / batches / instances.
- ✅ **Clustered forward (Forward+) lighting**: `DirectionalLightComponent` + dynamic
  `PointLightComponent`/`SpotLightComponent` → a per-frame light SSBO (set 1); the engine's first **compute**
  pipeline (`light_cull.slang`) culls them into a 16×9×24 froxel grid (exponential view-space Z), dispatched in
  `endFrame` before the scene pass with a compute→fragment barrier; the mesh fragment loops only its cluster's
  lights. `se set-clustered 0` falls back to a brute-force loop (verified pixel-identical). See `clustered-lighting`
  memory. (Per-cluster cap 64; excess lights are dropped silently.)
- ✅ **Scene authoring**: in-viewport **ImGuizmo** translate/rotate/scale (drawn into the Viewport window so it
  clips + takes input; un-flipped projection so it is not mirrored; W/E/R cycle; `glm::decompose` delta
  write-back) + a **Create** menu (Empty / Cube / Point/Spot/Directional Light / Camera). `TransformComponent`
  rotation is **Euler XYZ radians** (the inspector edits it directly, so it never clips at 90°).
- ✅ **Editor viewport camera + picking**: a fly-cam `EditorCamera` (the scene-view eye, separate from ECS
  cameras) — hold RMB to look + WASD move, Shift up / Ctrl down; `renderScene`/gizmo draw through it. Left-click
  ray-picks the nearest entity by world-space mesh AABB (`pickEntity`; empty space deselects). `se pick`/`focus`.
- ✅ **Editor shell**: Roboto + Roboto Mono fonts (data fields monospace); a default DockBuilder layout
  (Hierarchy/Inspector left, Assets bottom, Viewport center).
- ✅ **Project asset catalog**: imported models/textures become **named, renameable (UTF-8) catalog entries**
  (`AssetCatalog` in `Saffron.Scene`, owned by `AssetServer`; `Scene` borrows a `const AssetCatalog*` so the
  registry-driven inspector pickers can read it). The **Assets** panel shows a tile grid with **best-effort
  thumbnails** — textures as their image, meshes as a `renderMeshThumbnail` 3D preview, else a vendored Lucide
  **SVG** icon (nanosvg via `uploadSvgIcon`). Mesh/Material fields are **picker combos** (also ImGui drag-drop
  targets for asset tiles). Import via the modal/drag-drop (catalog-only, no auto-spawn). The whole project
  (catalog + scene) saves to one **`project.json`** (`saveProject`/`loadProject`; legacy `asset_registry.json`
  migrated on first load). `se list-assets`/`rename-asset`/`assign-asset`/`save-project`/`load-project`.
  See `asset-catalog` memory. (Non-latin names round-trip; rendering them needs a broader font — follow-up.)
- ✅ **Render graph** (`Saffron.Rendering:RenderGraph`): passes declare resource reads/writes (`RgUsage`) +
  color/depth attachments; the graph derives all barriers + layout transitions and records each pass body. It
  replaced the hand-written `endFrame` (the cull→scene→ui frame is now declared, not coded). **App-authorable**:
  layers add passes via the `onRenderGraph(RenderGraph&)` hook. Single graphics queue, no transient aliasing
  (right-sized; seams left for aliasing/async). See `render-graph-architecture` memory.
- ✅ **Post-process** demonstrator: an in-place compute tonemap (Reinhard+gamma) added to the graph from a layer
  (`se set-postprocess`); proves app-authored passes + RMW transitions derive correctly.
- ✅ **Draw-item layer**: `renderScene` emits a flat `DrawItem` list; `submitDrawList` batches it + stores a
  `SceneDrawList` the scene + depth passes consume (decoupled from the `submit()` closure seam).
- ✅ **Depth pre-pass** (`se set-depth-prepass`): a vertex-only pipeline lays down depth before the scene pass
  (loadOp Load, eLessOrEqual); the graph auto-inserts the depth barrier.
- ✅ **Material / PSO cache**: a `Material` selects a shader/variant; the renderer owns mesh PSOs in a keyed
  cache (`requestMeshPipeline`, build-on-miss) — the client no longer creates pipelines. Übershader: N materials
  → 1 PSO; an **unlit** permutation via a Slang `vk::constant_id` specialization constant is a 2nd cached PSO.
  `se render-stats` reports `pipelines`.
- ✅ **Bindless textures**: one global combined-image-sampler array (set 0, partiallyBound + updateAfterBind,
  required at device select). `uploadTexture` returns a stable slot; the albedo index is per-instance, so items
  differing only by texture batch into **one** instanced draw (verified: 2 textures → 1 batch). `GpuTexture`
  carries a `bindlessIndex` (no per-texture descriptor set).
- ✅ **Anti-aliasing** (`se set-aa off|fxaa|msaa2|msaa4|msaa8`): **MSAA** (multisampled scene color+depth, resolved
  into the offscreen via a graph resolve attachment; PSOs bake the sample count) for clean geometry edges, and
  **FXAA** (a compute post-process on a 1x scratch → offscreen) as the cheap alternative. Default off; `render-stats`
  reports `aa`.
- ✅ **Skybox + scene environment** (`plans/skybox/` phases 1-5; `se get/set-environment`): a
  `SceneEnvironment` on `Scene` (`SkyMode {Color,Texture,Procedural}`, clear/sky/ambient fields;
  `SceneVersion` 1→2 migration) drives a fullscreen visible-sky pass (`sky.slang`) drawn before the
  scene pass into the AA-chosen color target (loadOp flips Clear→Load). **Procedural reuses the baked
  IBL `envCube`**, so background and lighting share one source; the procedural sun follows the scene's
  directional light via an on-demand IBL re-bake (`requestEnvBake`, dirty-gated, consumed GPU-idle in
  `beginFrameGraph`). **HDR**: `.hdr` panoramas decode (`stbi_loadf`) + upload as **rgba16f** via
  `uploadTextureFloat` into the same bindless array (per-asset `AssetEntry.hdr`, no version bump);
  a `SkyMode::Texture` HDR panorama drives both the visible sky and the IBL through
  `ibl_equirect.slang` (`EnvSource::Equirect`). DDGI reads the scene sky color. Two gotchas: sample
  the equirect panorama through the **eRepeat** `linearSampler` (not the eClampToEdge `ibl.sampler`,
  which seams the meridian); `destroyRenderer` must `ibl.envPanorama.reset()` before
  `vmaDestroyAllocator`. In Texture mode the baked IBL ignores `skyRotation`/`skyIntensity` (applied
  by the sky pass only) — a documented future seam. Phases 6-8 (reflection probes, procedural
  atmosphere, clouds/time-of-day) are roadmap.

Not done yet (planned):
- **PBR** (metallic/roughness/normal maps — tangents + `materialSlot` per-submesh multi-material are reserved),
  shadows (a shadow pass now slots into the graph), transparency pass, and the full dynamic-lighting /
  global-illumination / ray-tracing roadmap — all scoped phase-by-phase in `plans/lighting/`.
- **Transient render-graph resources** (graph-created images + memory aliasing) + async compute; GPU-driven
  culling (`vkCmdDrawIndexedIndirect`/MDI, mesh shaders) — the graph + draw-item layer leave the seams.
- `Saffron.Physics` (Jolt) RigidBody + system; `resolveRefs` + scene-graph parenting; undo/redo.
- `volk`, multi-viewport ImGui (incl. multi-viewport gizmo), hardware GPU in the toolbox.

## Future expansion plans (`plans/`)

The `plans/` folder holds **phased, dependency-ordered implementation plans for future
expansions** that are scoped but not yet built. Each subfolder is one feature area
(e.g. `plans/lighting/`, `plans/skybox/`) with a `README.md` index and numbered
`phase-N-*.md` files. Plans are grounded in the current code (file/function
references) so a session can pick one up directly.

Convention: each plan carries a `**Status:**` line (`NOT STARTED` / `IN PROGRESS` /
`COMPLETED`). **Mark a plan `COMPLETED` when its work is done; delete a plan file only
*after* it is `COMPLETED`** (so pending work is never lost). When implementing a
feature, check `plans/` first — if a plan covers it, follow and update that plan rather
than starting cold.

- `plans/lighting/` — dynamic lighting roadmap: PBR+HDR → IBL → shadows → screen-space
  GI → temporal → DDGI (no-bake GI) → ray tracing → ReSTIR. See the `lighting-roadmap`
  memory for the investigation behind it.
- `plans/skybox/` — skybox + scene-environment rendering (shares cubemap/IBL infra with
  `plans/lighting/phase-2`).

See the memory notes (`build-environment`, `saffron-rewrite-plan`,
`code-style-go-conventions`) for deeper rationale.
