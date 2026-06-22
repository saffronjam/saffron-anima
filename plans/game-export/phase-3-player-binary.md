# Phase 3 — `saffron-player` binary

**Status:** COMPLETED — `saffron-player` crate (window + swapchain via `saffron-app`'s windowed
path; loads `project.json` next to the exe with CLI/`SAFFRON_PROJECT` override; reads `app.json`
into the shared `saffron_protocol::AppManifest`; `ProjectHost` over `&mut Renderer`; runs the
shared `RuntimeSession`; pre-baked `.spv` only). `saffron-window` re-exports the raw event types;
`RuntimeSession::advance` added as the always-playing convenience. Verified: build + `clippy -D
warnings` clean; headless-offscreen smoke against `dev-project` loads the project, starts the
script VM (scripts execute each frame), renders, exits clean + validation-clean. v1 limits:
`fullscreen`/`vsync` manifest fields are read but not yet applied (FIFO present, no fullscreen
path); on-screen windowed present is the Phase-1-validated path (nested-weston FIFO deadlocks, so
verified via Phase 1 + this headless smoke rather than a live present here).

A thin standalone runtime: open a window, load a project from a folder, run the shared
`saffron-runtime` session, present to the swapchain. It links **none** of the editor stack.

## The crate

- New `engine/crates/player` → `saffron-player` (a binary).
- Deps: `saffron-app`, `saffron-runtime`, `saffron-rendering`, `saffron-window`, `saffron-scene`,
  `saffron-assets`, `saffron-protocol` (for the `app` config DTO). **No** `saffron-control`,
  **no** `saffron-sceneedit`.

## Boot

1. Locate `project.json` **next to the executable** (Godot-style); allow a CLI arg / env override
   for dev iteration against an unstaged project.
2. Read `app.json` (sibling): `title`, `width`, `height`, `fullscreen`, `vsync`.
3. Build an `AppConfig` (`engine/crates/app/src/lib.rs`) with a **real window** (the windowed path
   from Phase 1) sized/titled from `app.json`; select present mode from `vsync`.
4. Implement the `ProjectHost` trait (`engine/crates/assets/src/project.rs`:
   `wait_gpu_idle` / `render_settings_to_json` / `apply_render_settings`) against the renderer, and
   call `AssetServer::load_project` to populate the scene + catalog. Reuse `RendererUploader`
   (`engine/crates/assets/src/gpu.rs`) as the `GpuUploader` so assets upload on demand.
5. Create a `RuntimeSession`, `start_scripts`.

## Per-frame loop

Feed window input into the session's `ScriptInputState` → `session.advance(dt)` → `render_scene`
to the swapchain (`engine/crates/assets/src/render_scene.rs`). No control poll, no shm publish, no
overlay build. Script `print`/logs and errors go to stdout/stderr (no editor ring to drain into).

## Shaders

- **Material shaders:** load pre-baked `materials/<uuid>*.spv` only. On a miss, **error** — never
  invoke `slangc`. (The codegen/compile path from `codegen.rs` is not linked or not reachable in
  the player.)
- **Engine shaders:** load from `shaders/` next to the binary exactly as the host does
  (`resolve_shader_dir`, `engine/crates/rendering/src/pipelines.rs`) — already pre-baked by
  `xtask shaders`, no runtime compile.

## Watch-outs

- The player must run with `SAFFRON_EDITOR_NATIVE_VIEWPORT` **unset** and no shm env vars — assert
  it never constructs `ViewportShmPublisher` or a control server.
- Asset-root resolution: the staged layout has `assets/` next to the binary; confirm
  `set_asset_root` / `engine_asset_path` resolve relative to the project/exe dir (or set
  `SAFFRON_ASSET_DIR` in the staged launch), not the dev `target/` tree.

## Gate

`just engine` clean (workspace now builds `saffron-runtime` + `saffron-player`); manual: hand-stage
a folder (player + a project's `project.json`/`app.json`/`assets`/`src`/`shaders` + pre-baked
material `.spv`), run the player directly, confirm the scene renders and scripts/physics run in a
standalone window. `just prepare-for-commit` clean.
