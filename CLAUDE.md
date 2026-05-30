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
    cmake --preset debug          # first time / after CMake changes
    cmake --build build/debug
    ./build/debug/bin/SaffronEditor
  '
  ```
- For automated/headless verification, bound the run:
  `SAFFRON_EXIT_AFTER_FRAMES=5 ./build/debug/bin/SaffronEditor` exits after N frames.

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
| Shaders | Slang | 2026.10 | `slangc -target spirv`, compiled in CMake |
| Math | GLM | 1.0.1 | |
| Serialization | nlohmann/json | 3.12.0 | `JSON_NOEXCEPTION`; scene save/load |
| Screenshots | stb_image_write | 1.16 | vendored `third_party/stb` + `cmake/stb_impl.cpp` |

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
│       ├── signal/signal.cppm    # module Saffron.Signal — SubscriberList<...> signal/slot
│       ├── window/window.cppm    # module Saffron.Window — SDL3 window + typed event signals
│       ├── scene/scene.cppm      # module Saffron.Scene — entt ECS + ComponentRegistry + JSON serialization
│       ├── rendering/renderer.cppm  # module Saffron.Rendering — Vulkan device/swapchain/frame loop + submit() seam
│       ├── ui/ui.cppm            # module Saffron.Ui — ImGui docking (SDL3 + Vulkan backends) + Viewport
│       ├── editor/editor.cppm    # module Saffron.Editor — hierarchy + generic inspector + component registration
│       ├── control/control.cppm  # module Saffron.Control — unix-socket control plane (commands + screenshots)
│       └── app/app.cppm          # module Saffron.App — App/Layer/AppConfig + run() main loop
├── editor/
│   ├── CMakeLists.txt      # SaffronEditor executable
│   └── source/main.cpp     # client app: builds AppConfig, attaches a Layer, calls se::run()
└── tools/se/               # the `se` control CLI (json over the unix socket; no engine dep)
```

Modules form a DAG (real imports, not a single chain): `Signal→Core`,
`Window→{Core,Signal}`, `Scene→Core`, `Rendering→{Core,Window}`,
`Ui→{Core,Window,Rendering}`, `Editor→{Core,Signal,Scene}`,
`Control→{Core,Window,Rendering,Scene,Editor}`, `App→{Core,Window,Rendering,Ui}`.
The editor exe links `Saffron::Engine` and imports the modules it needs (Core/App/Window/Rendering/Ui/Editor/Control).

### Module conventions
- One namespace: `se`. Engine modules are named `Saffron.<Area>`.
- `core`/`signal`/`app` use `import std`. `window` uses `import std` + the SDL3 **C**
  header (safe — C headers don't clash with the std module).
- `rendering`, `ui`, and `scene` wrap heavy **C++** third-party headers (Vulkan +
  vk-bootstrap + VMA, ImGui, entt + glm), so they use **classic `#include` in the
  global module fragment and do NOT `import std`** — mixing `import std` with a heavy
  C++ header in one TU breaks. `editor` and `control` follow the same rule, and the
  editor TU (`main.cpp`) includes `<imgui.h>` the same way. These modules are still consumed normally by the `import std` modules —
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
- ✅ RAII meta-layer (`Pipeline`, `Image`), renderer-owned, freed before the device.
- ✅ Slang shader compiled to SPIR-V in CMake → graphics pipeline → triangle drawn
  via the `onRender` layer hook + the `submit(lambda)` seam.
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
  add/remove/set component, set-transform, save/load scene, screenshot viewport|window to PNG, quit).
  See `control-plane` memory.

Not done yet (planned):
- A **render system** that draws the ECS scene into the Viewport (mesh + material components,
  offscreen depth) — replaces the placeholder triangle; needs vertex/index `Buffer` meta-layer.
- `RenderGraph`/`RenderPass` frame graph; `Saffron.Physics` (Jolt) RigidBody + system; `resolveRefs`
  + scene-graph parenting; undo/redo.
- `volk`, multi-viewport ImGui, hardware GPU in the toolbox.

See the memory notes (`build-environment`, `saffron-rewrite-plan`,
`code-style-go-conventions`) for deeper rationale.
