# Saffron Anima

A from-scratch **Vulkan** renderer / **C++26** game engine. The engine is a static library
(`Saffron::Anima`) that also builds **`SaffronAnima`**, a *headless viewport host*: it renders the
scene plus a native gizmo overlay offscreen, publishes frames into shared memory, and serves the
control plane — **no UI panels of its own**. The **editor is the Tauri/React/TypeScript app in
`editor/`**; it spawns the host, presents its frames on a Wayland subsurface below the transparent
webview (UI composites over the live viewport), and drives every operation over a
JSON-over-unix-socket control plane. This is a clean-slate rewrite of an older DirectX 11 engine (prior code on `old-master`
/ `rework`, reference only); it preserves the API *shape* that worked — an `App`/`Layer` lifecycle, a
deferred `submit(lambda)` render seam, a frame graph, an entt scene, signal/slot events — and drops
everything DX11-specific and the heavy OOP.

## Conventions (not optional)

- **NO LEGACY. NO COMPAT SHIMS. EVER.** This is a clean-slate codebase (`main` is an intentional orphan
  fresh start — there is nothing on disk, in the field, or downstream to be backward-compatible with).
  There is exactly **one** way to do each thing, and one code path for it. When a change would break an
  existing flow, command, file format, component, or test: **break it, then rebuild that flow on the new
  design — in the same change.** This is absolute and overrides any instinct toward caution:
  - **Never** keep an old code path alive "for back-compat" or "so callers don't break".
  - **Never** add a second command / function / format / field that duplicates an existing one's purpose
    just to avoid disturbing its callers. Replace the old one and update every caller.
  - **Never** defer a cutover by leaving the superseded path running next to the new one ("additive for
    now, retire later" is forbidden — *now* is when you retire it).
  - If you ever catch the thought *"I won't do X because it would break Y"* — that is the signal to **do X
    and fix Y**, not to preserve Y. Update every caller, delete the dead path, and fix the tests together.
  A feature is **not done** while a superseded flow, command, or format still exists anywhere in the tree.
  "I documented the deferral" does not count as done. Migration of existing user data is out of scope
  (start a fresh project), so there is never a migration burden to hide behind.
- **Code style:** `CONVENTIONS.md` (Go-flavored C++) — authoritative, the whole codebase follows it.
- **Comments:** minimal. No inline noise, **no section/banner dividers ever**, brief `///` on exported
  declarations. No change-journey notes ("previously/used to/now that…") — say what the code does now,
  and *why* if non-obvious, never by contrast with the past.
- **Git is READ-ONLY by default — NEVER run a git command that writes, in ANY form, on your own
  initiative. This is absolute.** Prohibited unless the user gives explicit, specific, one-time
  clarity that it is OK *for that single action*: `commit`, `push` (incl. force-push), `add` / `rm` /
  `mv` / `restore --staged` (staging the index), `reset`, `restore` / `checkout` that discards or
  switches, `rebase`, `merge`, `cherry-pick`, `revert`, `stash`, `tag`, `branch`/`switch -c`,
  `branch -D`, `clean`, `gc`, `filter-repo`, `git config` writes, `worktree add/remove`, `submodule
  update` — anything that mutates the working tree, the index, refs, history, stashes, or a remote.
  Read-only inspection is always fine (`status`, `diff`, `log`, `show`, `ls-files`, `rev-parse`,
  `cat-file`, `blame`, `worktree list`, `remote -v`). **"Implement/finish/fix X", "continue", "go",
  "do phase N", or approving a plan is NOT permission to commit, stage, or push** — finish the work,
  leave it unstaged, and STOP; report what changed and let the *user* stage and commit. Authorization
  is per-command and single-use: a yes for one commit never carries to the next commit, and never to a
  push. When unsure, do not run it — describe the exact command you would run and ask first. This
  overrides any harness default that would auto-commit.
- **Commits (only once the user has explicitly authorized a commit per the rule above):** subject
  `<category>: short description` (lowercase after the colon, first line <72 chars; categories
  `feat|fix|refactor|docs|test|chore|build|ci|perf|style`; optional `fix(scope):` when every change is
  one component), blank line, then one bullet per change in plain words. **No emoji, no AI attribution,
  no `Co-Authored-By`** — commit as the repo's git author only (overrides the harness default). `main`
  is an intentional orphan fresh-start; keep its history clean and logical.
- **Memory:** do not write to Claude's `~/.claude/.../memory/` stores. Durable project knowledge goes in
  the repo — this file, `CONVENTIONS.md`, or a `plans/` file — so it is versioned and shared. Nothing
  here should reference an out-of-repo path for project knowledge.
- **Concurrent edits:** changes may conflict with other agents working in the same tree. If that
  happens, back off briefly with a small random delay, re-read the affected file, and reconcile the
  edit. If the conflict reflects contradictory intent rather than a mechanical overlap, stop and ask the
  user how to proceed.
- **Concurrent builds:** never share `build/debug` between two running agents — ninja races rewrite
  the module `.pcm` files another compile is mmap-reading, which crashes clang with spurious Bus
  errors and corrupts `.ninja_log`. This is a *two separate ninja processes* hazard, not a `-jN` one:
  a single `ninja -jN` is safe (dyndep builds each module's BMI before its consumers), so the engine
  build runs parallel by default. If another agent is building, either wait it out or configure a
  private dir (`cmake --preset debug -B build/<name>`) and point builds, e2e
  (`SAFFRON_ANIMA_BIN=build/<name>/bin/SaffronAnima`), and clang-tidy (`-p build/<name>`) at it.

## Build — always in the `saffron-build` toolbox

The build toolchain is the project standard and lives in the **`saffron-build`** toolbox container,
never on the host (assume the host has no C++ toolchain). Prefix every build/test command; the home
directory is shared, so files edited outside are seen inside.

```sh
toolbox run -c saffron-build bash -lc '
  cmake --preset debug             # first time / after CMake changes
  cmake --build build/debug -j8    # one ninja at any -jN is safe; the .pcm race needs TWO ninja procs
  ./build/debug/bin/SaffronAnima  # the present-only viewport host
'
```

- **Env vars must be set *inside* the toolbox invocation, never as a host-side prefix.** A `make`
  target runs inside `toolbox run`, and the host environment does not cross that boundary — so
  `FOO=1 make run` sets `FOO` only in the host shell and the recipe never sees it (it silently
  no-ops, which looks like the flag had no effect). Set the var inside the command
  (`toolbox run -c saffron-build bash -lc 'export FOO=1; …'`) or bake it into the recipe / a `make`
  variable (e.g. the `WEBVIEW_HW` knob the `Makefile` expands into the `run` recipe). **Never suggest
  a host-side `ENV=… make …`.**
- The toolbox provides clang 21 + libc++ (ships the `std` module), CMake + Ninja, the Vulkan 1.4 SDK,
  SDL3, and Slang. `CMakePresets.json` (`debug`/`release`) pins clang++, `-stdlib=libc++`, lld, Ninja.
- **`import std` gotchas:** needs the experimental CMake gate (set in the root `CMakeLists.txt`) +
  `CMAKE_CXX_MODULE_STD ON` per target. **Do not** set `CMAKE_CXX_EXTENSIONS OFF` — the std module
  builds as `gnu++26` and consumers must match, or the BMI is rejected.
- GPU is software (Mesa llvmpipe) unless `mesa-vulkan-drivers` is installed in the toolbox — fine for
  correctness/validation.

### Headless runs & the verification gate

- `SAFFRON_EXIT_AFTER_FRAMES=N ./build/debug/bin/SaffronAnima` exits after N frames.
- **No display?** Run a headless compositor in the toolbox, then point SDL at it:
  `weston --backend=headless --width=1280 --height=720 --socket=wl-x --idle-time=0 &`, then
  `export WAYLAND_DISPLAY=wl-x SDL_VIDEODRIVER=wayland`. Use a unique `--socket` + `SAFFRON_CONTROL_SOCK`
  per run, and capture the exit code to a file *before* any `pkill` (the toolbox wrapper surfaces the
  pkill signal, not the real exit code).
- The reproducible gate is `tools/ci/check.sh`: engine build → present-only smoke → control-schema
  contract test → frontend `bun run build`. `make check` wraps it once the toolbox/bun/display are set
  up (also `make engine|editor|schema`). There is intentionally no GitHub-hosted CI (a stock runner
  can't reproduce the toolbox); `.github/workflows/ci.yml` targets a self-hosted runner.
- `make e2e` runs the `tests/e2e` suite — TypeScript on `bun test` that boots a headless engine and
  drives it over the control plane (typed via `@saffron/protocol`), asserting responses and a
  validation-clean log. It is the language-appropriate place for engine behaviour tests: the wire is
  JSON, so the driver need not be C++.
- Convenience targets (all run inside the toolbox; `make help` lists them): `make run` starts the
  editor, which spawns the engine; `make run-engine` starts only the present-only host; `make run-docs`
  serves the Hugo site. `make format` runs clang-format (`.clang-format`) over the C++ and oxfmt over
  the editor TypeScript; `make lint` runs the clang-format check + clang-tidy (`.clang-tidy`) + oxlint
  (`editor/.oxlintrc.json`); `make prepare-for-commit` does format then lint. clang-format enforces the
  layout `CONVENTIONS.md` describes — adopting it is a one-time normalization across the tree.

### The editor (Tauri/React)

With `bun` on PATH inside the toolbox:

```sh
cd editor && bun install && bun run check && bun run tauri dev
```

`bun run check` regenerates `@saffron/protocol` from the control DTOs in `control_dto.cppm` and typechecks; `bun run format`
(oxfmt) and `bun run lint` (oxlint) cover style. `tauri dev` spawns `build/debug/bin/SaffronAnima`
(override with `SAFFRON_ANIMA_BIN`) and needs a Wayland session for the subsurface presenter.

## Architecture

- **Lifecycle:** a client fills `sa::AppConfig` (window config + `onCreate`/`onExit`) and calls
  `sa::run(config)`, which owns the main loop: poll events → `onUpdate` → `beginFrame` → `onRender`
  (submit GPU work) → `beginFrameGraph` (cull + scene passes) → `onRenderGraph` (app passes) →
  `endFrame` (derive barriers, execute, present). `run` calls `waitGpuIdle` before any teardown.
- **Layer = struct of optional closures** (`onAttach/onUpdate/onRender/onUi/onRenderGraph/onDetach`),
  not a virtual base; `attachLayer(app, layer)` pushes it (the Go-interface-as-itable pattern).
- **Render seam:** `submit(renderer, [](cmd){ … })` records a closure into the current frame.
- **Render graph:** each pass *declares* its resource usage (`RgUsage`: `ColorWrite`, `SampledRead`,
  `StorageImageRWCompute`, …) + attachments; the graph derives every barrier and layout transition and
  records each pass body. No pass writes a barrier by hand; apps add passes via `onRenderGraph`.
- **Resources:** Vulkan-Hpp (`vk::`) with `VULKAN_HPP_NO_EXCEPTIONS` — every call → `std::expected`,
  checked on the spot. No `vk::raii` (it throws). Data-plane resources are move-only RAII wrappers held
  as **`Ref<T> = std::shared_ptr<T>`**, freed before the device (teardown: client drops its `Ref`s in
  `onExit`, `run` calls `waitGpuIdle` first, so nothing outlives `vmaDestroyAllocator`).
- **Events:** `SubscriberList<Args...>` signal/slot (handler returns `true` to stop propagation);
  `Window` exposes typed signals (`onResize`, `onKeyPressed`, …).
- **Errors:** fallible functions return `std::expected<T, std::string>`; no exceptions in engine code.

## Modules (one namespace `sa`, named `Saffron.<Area>`)

DAG, leaves first (real imports, not a chain):

```
Core
Signal   → Core
Json     → Core
Window   → {Core, Signal}
Geometry → Core
Scene    → {Core, Json}
Animation→ {Core, Geometry, Scene}                pose/clip types + samplers (classic #include)
Physics  → {Core, Geometry, Scene, Animation}     Jolt wrapper; :Types partition; only physics.cpp includes <Jolt/…>
Script   → {Core, Scene}                          (Lua 5.5 + LuaBridge3; only Host may import it)
Rendering→ {Core, Window, Geometry}              partitions :Types :Detail :RenderGraph
Assets   → {Core, Json, Geometry, Rendering, Scene}
SceneEdit→ {Core, Signal, Scene, Json}            partition :Context
Control  → {Core, Json, Window, Rendering, Scene, SceneEdit, Assets, Physics}   partitions :Dto :Command
App      → {Core, Window, Rendering}
Host     → {Core, App, Window, Rendering, SceneEdit, Control, Scene, Animation, Physics, Script, Assets}   (the SaffronAnima exe)
```

- `core`/`signal`/`app` use `import std`; `window` uses `import std` + the SDL3 **C** header (safe).
- Modules wrapping heavy **C++** third-party headers (`rendering`, `scene`, `animation`, `physics`,
  `script`, `geometry`, `json`, `assets`, `sceneedit`, `control`, `host`) use classic `#include` in the global
  module fragment and **do NOT `import std`** (mixing breaks the TU). They are still consumed normally by
  the `import std` modules — the BMI carries the std types.
- Larger modules split into an interface partition + `.cpp` implementation units.
- There is no engine UI toolkit: the in-viewport gizmo is a **native overlay** (`OverlayVertex` /
  `buildNativeGizmo` in `Saffron.Host`), and the full editor UI is the React/Tauri frontend.

## Layout

```
engine/source/saffron/  one dir per module above (core, rendering, host, sceneedit, …)
engine/source/main.cpp  the SaffronAnima host stub: int main(){ return sa::runHost(...); }
engine/assets/          shaders (*.slang → SPIR-V in CMake), models, fonts, icons (copied next to the exe)
editor/                 Tauri/React/TS editor — src/ (React + Zustand + typed control client), src-tauri/ (Rust bridge)
schemas/control/        DTO-first wire contract → @saffron/protocol: hand-authored envelope.schema.json + generated openrpc/command-manifest JSON (from control_dto.cppm)
tools/sa/               the `sa` control CLI (json over the unix socket; no engine dep)
tools/gen-control-dto/  gen.ts: control_dto.cppm → C++ serde + @saffron/protocol types + OpenRPC + manifest (the wire-contract generator)
tools/ci/, tools/check-control-schema/, tools/check-projects/   the reproducible gate, the live-vs-schema contract test, the project-feature smoke
tests/e2e/              end-to-end tests (bun) driving a headless engine over the control plane
cmake/                  Dependencies.cmake (FetchContent deps + vma target) + vma/stb impl TUs
docs/                   Hugo (hugo-book) docs site — per-concept explanations + how-to/reference/tutorials
plans/                  phased, dependency-ordered plans for future expansions
```

## Stack

| Area | Choice | Notes |
|------|--------|-------|
| Language / compiler | C++26, Clang 21 + libc++ | named modules + `import std` (`gnu++26`) |
| Build | CMake 3.31 + Ninja | FetchContent for vendored static deps |
| Vulkan | Vulkan-Hpp `vk::`, headers 1.4.341, target 1.4 | no-exceptions; dynamic rendering + sync2 |
| Bootstrap / alloc | vk-bootstrap, VMA 3.3 | device/swapchain selection; one VMA impl TU |
| Window / ECS / math | SDL3 3.4, EnTT 3.16, GLM 1.0 | |
| Shaders | Slang | `slangc -target spirv`, compiled in CMake |
| Serialization | nlohmann/json (`JSON_NOEXCEPTION`) | scene/project save/load |
| Import / images | cgltf, tinyobjloader, stb_image(_write) | glTF/OBJ → `.smesh`; texture decode; PNG screenshots |
| Editor | Tauri 2 + React 19 + Vite + shadcn/ui + Tailwind v4, Bun | |

## Keep current (part of "done")

- **Milestone gate:** after each feature — and at each phase boundary of a larger task, not only at
  the very end — run `make engine` then `make prepare-for-commit` (format + lint) and fix every
  warning your change raises. The point is a clean testing ground at intervals (a green `build/debug`
  a plain `make run` picks up), not one big reconciliation at the end. This composes with the
  concurrent rules above: if the build or lint fails *only* because of another agent's in-flight
  changes (see **Concurrent edits** / **Concurrent builds**), assume it will land soon — leave it,
  note it, and move on. **Never** fix another agent's parallel work to make the gate pass. When unsure
  whether a failure is yours, gate your own changes in isolation via a private `build/<name>` dir, and
  it is fine to defer the shared-`build/debug` build until the tree settles.
- **`sa` CLI:** a feature that adds engine state worth driving/inspecting gets a matching control
  command (one `registerCommand` in `Saffron.Control`), so the running editor stays scriptable and
  visually debuggable from a shell.
- **`docs/`:** a change that adds/alters an engine concept updates the matching explanation page under
  `docs/content/` and its hub `_index.md` row, in the same change.

## Docs site

Hugo (hugo-book theme, organised by Diátaxis). Needs **Hugo extended** (it compiles SCSS); the theme is
a git submodule.

```sh
git submodule update --init --depth 1 docs/themes/hugo-book
cd docs && hugo server   # preview at http://localhost:1313/saffron-anima/
```

Page conventions: one concept per page; TOML front matter (start from an archetype); **title** is a
short sentence-case noun phrase, and the front-matter `title` must equal the body `# H1` (the theme
doesn't render the title). Lead with the concept and why, not "file X does Y"; put code pointers in a
slim `What | File | Symbols` table (symbols, not line numbers). Math via KaTeX (`$…$`, `math = true`),
diagrams via ` ```mermaid `, callouts via GitHub alerts. Voice plain and direct — run prose through the
`humanizer` pass. Theme overrides live in `docs/assets/_custom.scss` + `docs/layouts/_partials/docs/inject/head.html`.

## Plans (`plans/`)

Phased, dependency-ordered implementation plans for scoped-but-unbuilt expansions; each subfolder is one
feature area with a `README.md` index + numbered `phase-N-*.md` files, grounded in current code. Each
plan carries a `**Status:**` line (`NOT STARTED`/`IN PROGRESS`/`COMPLETED`); mark it `COMPLETED` when
done, and delete a plan file only *after* it is `COMPLETED`. **Check `plans/` first** when implementing
a feature — follow and update a matching plan rather than starting cold.

## Status

- **Built** (per-concept reference is `docs/`): the full forward+ PBR pipeline — clustered lighting, IBL,
  shadows (directional/spot/point/contact/ray-traced), DDGI + voxel GI + SSGI + ReSTIR, GTAO, TAA, motion
  vectors, tonemap, MSAA/FXAA; bindless + instanced rendering with an übershader/PSO cache; the render
  graph; entt scene + registry-driven JSON project format; glTF/OBJ import + asset catalog; a native
  material system (`.smat` PBR assets + params buffer, importer, instances/overrides, thumbnails) with a
  node-graph editor (React Flow model → Slang codegen for preview and scene entities); skeletal animation
  behind `Saffron.Animation` (glTF clip import, an animation-player runtime with transitions/blending, a
  compute-skinning prepass feeding motion vectors + skinned-BLAS rebuild, foot IK, a native skeleton
  overlay, animation control commands, and the editor timeline panel); the control
  plane + `sa` CLI; the Tauri editor; per-entity Lua scripting (Lua 5.5 + LuaBridge3 behind
  `Saffron.Script`: ScriptComponent slots, script-declared fields + overrides, Inspector UI, project
  `src/` scaffold); physics behind `Saffron.Physics` (Jolt vendored, cross-platform-deterministic; a
  per-play world on the play edge; rigidbody/collider split components with five shapes + materials +
  auto-fit; object-layer matrix + sensors/triggers + a contact-event ring to scripts; kinematic
  bone-following; a `CharacterVirtual` controller; raycast/shapecast queries + a Lua `sa.raycast`; and a
  motor-driven ragdoll routed through the `PoseBuffer.override_`/`weight` blend layer — passive,
  active, and partial, with import auto-fit).
- **Not yet:** transient render-graph resources (graph-created images + aliasing) + async compute;
  GPU-driven culling (MDI / mesh shaders); scene-graph parenting; undo/redo; hardware GPU in the
  toolbox.
