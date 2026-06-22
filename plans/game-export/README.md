# App export — standalone player (v1) — design

**Status:** IN PROGRESS — all six phases implemented and individually gated green (build +
`clippy -D warnings` + unit/protocol tests + `bun run check`/lint + `just prepare-for-commit`);
the export-app e2e passes and the player boots a staged folder validation-clean. Final full
`just e2e` sweep in progress. See each `phase-N-*.md` for the per-phase result.

The decision-locked design for a first, deliberately limited **App export**: an "Export App…"
action in the editor that cooks the open project into a staged folder containing a
**`saffron-player`** runtime binary plus the project's data, runnable on its own with no editor,
no control plane, and no toolchain present (no `slangc`). The model is Godot's *export template +
data* split — a precompiled runtime binary alongside packed project data — using a loose-folder
layout for v1; single-file packing is a later iteration.

"App", never "Game" — the runtime and manifest stay domain-neutral, since not everything built on
the engine is a game.

This is **a plan only.** No engine/editor source is touched here. Each `phase-N-*.md` carries its
own `**Status:**` line and the concrete edits; this README is the locked design they implement.

---

## 1. What's missing today vs what this adds

A project can be authored and run **only inside the editor**: the editor spawns `saffron-host`
headless and drives it over the JSON-over-unix-socket control plane, with frames published into
shared memory for the Wayland subsurface presenter. The host renders the scene **offscreen**;
there is no on-screen swapchain present path exercised anywhere, no way to load a project without
the editor's `load-project` control command, and material shaders are compiled **at runtime** via
`slangc` (`engine/crates/assets/src/codegen.rs`). So there is no artifact you can hand to someone
who does not have the editor and toolbox.

This plan adds, on the existing seams and with one code path per concern:

- a **windowed present path** (real window + swapchain), proven before anything depends on it;
- a shared **`saffron-runtime`** crate holding the play-mode simulation spine, consumed by both
  the editor host and the new player (no duplicated "advance the world" logic);
- a thin **`saffron-player`** binary that loads `project.json` next to itself, runs the runtime,
  and presents to a window — linking none of `control`/`sceneedit`/`shm`;
- an engine-side **export cook** (one control command) that pre-bakes every material's SPIR-V and
  stages the player + data into a runnable folder;
- the editor **"Export App…"** menu item + modal, and an **`sa export`** CLI for headless/CI use.

---

## 2. Architecture summary

The gameplay simulation spine (advance animation → run scripts → step physics, once per frame)
lives today inside `HostLayer::update_session` (`engine/crates/host/src/layer.rs`), tangled with
edit-mode state, the control socket, shm publishing, and the gizmo overlay.

**That spine is extracted into a new `saffron-runtime` crate** that both binaries consume:

```
saffron-runtime  (new)  ── shared play spine (anim + scripts + physics tick)
   ├─ saffron-host   (editor: edit + play, control, shm, overlay)
   └─ saffron-player (new: window + swapchain + run, nothing editor)
```

This honors the repo's no-duplication rule (one code path for "advance the world a frame") and
keeps `control`/`sceneedit`/`shm` out of the shipped binary entirely. Per **NO LEGACY**, the
host's inline play spine is *removed* and rebuilt on the shared crate in the same change — not
left beside it.

`saffron-runtime` DAG position (depends only on simulation crates, never on rendering/window):

```
saffron-runtime → {saffron-core, saffron-scene, saffron-assets, saffron-animation,
                   saffron-script, saffron-physics, saffron-geometry}
```

`render_scene` already lives in `saffron-assets` (`engine/crates/assets/src/render_scene.rs`) and
is shared as-is; only the *simulation* tick moves into the new crate.

**Export is owned by the engine** (it holds the catalog and the shader compilers), exposed as one
`export-app` control command — matching the AGENTS.md "feature gets a control command" rule. The
Tauri bridge re-implements no cooking. The cook pre-bakes material SPIR-V so the shipped player
never needs `slangc`; engine shaders are already pre-baked by `xtask shaders` and shipped next to
the binary.

Staged layout:

```
<Title>/
  saffron-player          (the runtime binary)
  project.json  app.json  (data + manifest)
  shaders/  assets/  src/
```

---

## 3. Key decisions (locked)

- **App, not Game.** The manifest is `app.json`; config fields are `title/width/height/fullscreen/
  vsync`; the runtime is `saffron-player`. Nothing in the runtime assumes a game.
- **Shared `saffron-runtime` crate, not a duplicated spine, not a host-reuse mode.** The play
  spine is the one piece both binaries need; extracting it keeps a single code path and yields a
  shipped binary free of editor/control/sceneedit code. (Rejected: duplicating the spine in the
  player — violates NO LEGACY; reusing `saffron-host` in a "player mode" — drags `control` +
  `sceneedit` into the shipped binary.)
- **Loose-folder staging for v1.** A runnable folder, not a single-file pack. The asset loaders
  already abstract "open container, read chunk," so a `.spack` pack file is a contained later
  change (Godot's embed-PCK footer is the reference for v2). (Rejected for v1: embedding, ZIP,
  compression, encryption.)
- **Pre-baked material SPIR-V; the player never invokes `slangc`.** The cook calls the existing
  `AssetServer::compile_material_graph` / `compile_material_mesh_shader` (`codegen.rs`) for every
  material and ships the `.spv`; the player loads pre-baked `.spv` only and **errors** on a miss
  (never compiles). Mirrors Unreal/Unity compiling shaders at cook/build time.
- **Scripts ship as Luau source, interpreted at runtime.** `ScriptHost` already loads from the
  project `src/` and interprets via `mlua` — no AOT/transpile story needed (unlike Unity IL2CPP).
- **Export config persists in `project.json` under an `app` block**, written into the staged
  `app.json` — one source of truth, two roles (Godot folds export presets next to the project).
  Adding the block bumps `PROJECT_VERSION` (`engine/crates/assets/src/project.rs`); older
  `project.json` re-save to gain it — data migration is out of scope per AGENTS.md.
- **Whole catalog copied; no dead-asset stripping in v1.** Capable without a reference-graph pass;
  stripping (which Unreal/Unity do) is a later optimization, not needed to be correct.
- **Synchronous export.** The `export-app` command cooks inline; the host frame loop freezes
  briefly while shaders compile. Acceptable for a deliberate action; streamed progress is later.
- **Linux x86_64 (Wayland/winit) only.** The editor's target. Other platforms / cross-compile /
  signing / installers are out of v1; the modal shows the target as a static label, not a picker.

---

## 4. How the major engines do it (research that shaped this)

- **Godot** — the closest analog (an editor that spawns a separate runtime, clean engine/editor
  split). Ships a precompiled **export template** (runtime binary, no editor) + a **PCK** data
  pack the runtime auto-loads; PCK can be a sibling file or appended to the exe and found via a
  magic+size footer. This plan copies the *un-embedded* path (loose folder) for v1.
- **Unreal** — `BuildCookRun`: **cook** (per-platform asset conversion + shader compile, strip
  unreferenced) → **stage** (lay out, optionally `.pak`, chunkable) → **package** (native bundle +
  Shipping-config exe). Takeaway: asset cooking and shader compilation happen at build time, and
  the shipped exe is a different build configuration.
- **Unity** — Player build emits a runtime + serialized data (+ AssetBundles); scripts via Mono
  (ship IL) or IL2CPP (C#→C++→native AOT with code stripping). Takeaway: interpreted-script
  shipping is a valid model — which is what our Luau already does.

Sources:
[Godot — exporting projects](https://docs.godotengine.org/en/stable/tutorials/export/exporting_projects.html) ·
[Godot — packs/patches/mods](https://docs.godotengine.org/en/stable/tutorials/export/exporting_pcks.html) ·
[Unreal — packaging](https://dev.epicgames.com/documentation/unreal-engine/packaging-your-project) ·
[Unreal — cook/package/deploy/run](https://dev.epicgames.com/documentation/unreal-engine/build-operations-cooking-packaging-deploying-and-running-projects-in-unreal-engine) ·
[Unity — IL2CPP](https://docs.unity3d.com/Manual/IL2CPP.html)

---

## 5. Phases (dependency-ordered)

| # | Phase | Depends on | One-line |
|---|---|---|---|
| 1 | [Windowed present foundation](phase-1-windowed-present.md) | — | prove `Window::new` → `SurfaceSource::Window` → swapchain present; resize/recreate; input flow |
| 2 | [Extract `saffron-runtime`](phase-2-runtime-crate.md) | 1 | new crate holding the play spine; refactor `HostLayer` onto it; delete the inline spine |
| 3 | [`saffron-player` binary](phase-3-player-binary.md) | 2 | window + swapchain; load `project.json` next to exe; run runtime; pre-baked `.spv` only |
| 4 | [Export cook + staging](phase-4-export-cook.md) | 3 | `app` block + DTO; `export-app` command; pre-bake material SPIR-V; stage folder + `app.json`; e2e |
| 5 | [Editor "Export App…" UI](phase-5-editor-ui.md) | 4 | project-menu item + `ExportModal` + Zustand state + typed client + protocol regen |
| 6 | [`sa export` CLI + docs](phase-6-cli-docs.md) | 5 | `sa export <dir>`; docs how-to + hub row; final `prepare-for-commit` gate |

Dependency rationale: windowed present (1) is the load-bearing, untested foundation everything
draws to screen through; the runtime crate (2) is the shared spine the player needs; the player
(3) needs both; the cook (4) needs a player binary to stage; the editor UI (5) needs the
`export-app` command + DTO; the CLI + docs (6) close it out.

---

## 6. Keep-current (part of "done", per AGENTS.md)

- After **each** phase: `just engine` then `just prepare-for-commit` (format + lint); fix every
  warning the phase raises. The `PROJECT_VERSION` bump (Phase 4) extends its matching project
  round-trip handling in the same phase.
- Each drivable-state phase adds its `registerCommand` and runs the control-schema contract test +
  `bun run check` (git-diff-clean on generated files): Phase 4 (`export-app`) and Phase 5
  (frontend client) are control-touching.
- One concept page lands in Phase 6 under `docs/content/` (Diátaxis how-to, "Exporting an app") +
  its hub row, per the keep-docs-current rule (`docs-page` skill conventions).
- **NO LEGACY** enforced per phase: Phase 2 deletes the host's inline play spine when moving it to
  `saffron-runtime`; Phase 4's `PROJECT_VERSION` bump replaces the reader (old files re-save).
- **Concurrent builds:** while iterating shared `engine/target`, a second agent's build waits on
  the cargo lock; to build fully in parallel use a private `CARGO_TARGET_DIR=engine/target-<name>`
  and point the player/e2e at the matching binary via `SAFFRON_ANIMA_BIN` / an export-binary path.
