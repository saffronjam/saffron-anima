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
| Vulkan | raw C API | headers 1.4.341, target **1.3** | dynamic rendering + synchronization2 |
| Vulkan bootstrap | vk-bootstrap | 1.4.352 | instance/device/swapchain selection |
| GPU allocation | VMA | 3.3.0 | one impl TU in `cmake/vma_impl.cpp` |
| ECS | EnTT | 3.16.0 | (not wired in yet) |
| UI | Dear ImGui | 1.92.8-**docking** | `imgui_impl_sdl3` + `imgui_impl_vulkan`, dynamic rendering |
| Shaders | Slang | 2026.10 | `slangc -target spirv` (prebuilt, not yet wired) |
| Math | GLM | 1.0.1 | |
| Serialization | nlohmann/json | 3.12.0 | (not wired in yet) |

**We intentionally do NOT use Vulkan-Hpp / vk::raii** — its exception-throwing
RAII conflicts with the Go-style "no exceptions, explicit cleanup" rule. The
renderer uses the raw C API with explicit `destroy*` functions and
`std::expected<T, std::string>` at boundaries. `volk` is also not used yet (we
link the system loader directly); it can be added later as a dispatch optimization.

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
│       ├── scene/scene.cppm      # module Saffron.Scene — entt ECS, value components, forEach
│       ├── rendering/renderer.cppm  # module Saffron.Rendering — Vulkan device/swapchain/frame loop + submit() seam
│       ├── ui/ui.cppm            # module Saffron.Ui — ImGui docking (SDL3 + Vulkan backends)
│       └── app/app.cppm          # module Saffron.App — App/Layer/AppConfig + run() main loop
└── editor/
    ├── CMakeLists.txt      # SaffronEditor executable
    └── source/main.cpp     # client app: builds AppConfig, attaches a Layer, calls se::run()
```

Modules form a DAG (real imports, not a single chain): `Signal→Core`,
`Window→{Core,Signal}`, `Scene→Core`, `Rendering→{Core,Window}`,
`Ui→{Core,Window,Rendering}`, `App→{Core,Window,Rendering,Ui}`. The editor links
`Saffron::Engine` and imports the modules it needs (Core/App/Window/Scene).

### Module conventions
- One namespace: `se`. Engine modules are named `Saffron.<Area>`.
- `core`/`signal`/`app` use `import std`. `window` uses `import std` + the SDL3 **C**
  header (safe — C headers don't clash with the std module).
- `rendering`, `ui`, and `scene` wrap heavy **C++** third-party headers (Vulkan +
  vk-bootstrap + VMA, ImGui, entt + glm), so they use **classic `#include` in the
  global module fragment and do NOT `import std`** — mixing `import std` with a heavy
  C++ header in one TU breaks. The editor TU (`main.cpp`) includes `<imgui.h>` the
  same way. These modules are still consumed normally by the `import std` modules —
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
- ✅ Vulkan 1.3 device/swapchain (vk-bootstrap), VMA allocator, sync2 + dynamic
  rendering, clears the screen and presents, swapchain recreation on resize.
- ✅ ImGui docking integrated (SDL3 + Vulkan backends, dynamic rendering) — *being
  built/verified at time of writing.*

Not done yet (planned):
- entt `Scene`/`Entity` + value components + JSON serialization.
- `RenderGraph` / `RenderPass` typed-handle frame graph + `SceneRenderer` facade.
- Slang shader compile step (CMake rule) + a triangle/mesh pipeline.
- Editor panels (viewport image, entity inspector, hierarchy) wired via signals.
- `volk`, multi-viewport ImGui, hardware GPU in the toolbox, framebuffer capture.

See the memory notes (`build-environment`, `saffron-rewrite-plan`,
`code-style-go-conventions`) for deeper rationale.
