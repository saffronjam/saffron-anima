# Phase 3: Build/input spikes + Tauri/React skeleton + generic passthrough + typed client + auto-start/attach + crash recovery

**Status:** IN PROGRESS — build-verified; runtime (auto-attach) pending interactive verification

<!-- Flip to COMPLETED when the "Done when" checklist passes, validation-clean. Delete this file only after COMPLETED + merged. -->

**Done so far (2026-06-01):** Scaffolded the Tauri app under `editor/` (the C++ host lives at `editor-old/` from phase 1). **Spike-0a confirmed empirically:** the whole stack builds in the `saffron-build` toolbox with host bun on PATH — `cargo build` of the Rust bridge is green (tauri 2.11, edition 2024, webkit2gtk-4.1) and `bun install` + `bun run check` are green. `lib.rs` rewritten to ONE generic `control(cmd, params)` passthrough (the 8 MVP shims deleted) + dedicated window-handle commands (`start_engine`, `attach_native_viewport`, `resize_native_viewport`→the phase-1 engine command (no reparent flicker), `quit_engine`, `engine_alive` via `child.try_wait()` not `Option::is_some`), per-PID `XDG_RUNTIME_DIR` socket, `.setup()` auto-start with a liveness-aware readiness thread emitting `engine-phase`/`viewport-error`, and `RunEvent::ExitRequested` teardown. `@saffron/protocol` generated from the phase-2 `schemas/control/` via `json-schema-to-typescript` (24 types, `Uuid=string` through `tsType`, byte-deterministic). Typed `client.ts` (named params, ids as strings; entity selector key is `entity`, thumbnail key is `asset`), `coalesce.ts`, a Zustand `store.ts` + focus-gated reconcile poll + crash watchdog, `ViewportPanel` bounds-sync (ResizeObserver + resize-end debounce + HiDPI scaleFactor) + auto-attach on non-zero rect, `LoadingOverlay` (Retry/Restart), `App` event wiring. The dead fd-export/GTK bridge + gtk/gdk/glib/base64/thiserror/libc deps were NOT brought over. **Modernized vs the MVP:** Rust edition 2021→**2024**, TS/Vite ES2022→**ESNext**, tauri 2.8→**2.11**.

**Remaining (runtime, needs a display I can't drive headless):** `bun run tauri dev` actually launching → auto-spawn → X11 reparent into the webview window → loading overlay → live embedded scene with no clicks; crash-recovery Restart; HiDPI cover; two-instance socket isolation. The toolbox has no XWayland (weston-headless is Wayland-only), so the X11 child-window reparent can only be verified on a real X11/XWayland desktop. **Spike-0b (keyboard into the reparented child) not yet run** — phase 4 defaults to the control-command-driven camera/gizmo regardless, so it is not blocking.

## Goal

De-risk the two unproven premises (toolbox frontend build, keyboard-into-child-window),
stand up the scalable Tauri/React structure under `editor/`, replace the worktree's 8
bespoke Rust shims with ONE generic `control(cmd, params)` passthrough, generate
`@saffron/protocol` from the phase-2 schemas, build the typed client + Zustand store +
version-stamped reconcile poll, and implement robust auto-start + auto-attach + a loading
overlay + full lifecycle/crash recovery. After this phase the Tauri app launches, spawns
SaffronEditor, and shows the embedded live viewport with **zero button clicks** — but the
viewport still has no in-app interaction (gizmo/billboards/selection are phase-4) and no
data panels yet (phases 5-8).

**Depends on:** phase-2 (the new editor commands + `schemas/control/*.schema.json` + the
contract test must exist; the typed client + `gen-protocol` consume those schemas).
Transitively depends on phase-1 (the `editor/` dir is free, SaffronEditor builds as the
headless host at `editor-old/`, and `viewport-native-info`/`attach-native-viewport`/
`resize-native-viewport` exist on main).

## Current state (verified)

### Build environment (the spike-0a answer, already measured)
- Host (Fedora Silverblue 43) has **bun 1.3.5** (`/var/home/saffronjam/.bun/bin/bun`) and
  **node v24.8.0** (`~/.nvm/.../v24.8.0/bin/node`) but **no cargo/rustc** and **no
  webkit2gtk** (`pkg-config --exists webkit2gtk-4.1` → absent).
- The `saffron-build` toolbox has **cargo 1.95.0 + rustc 1.95.0**, **webkit2gtk-4.1 2.52.3**,
  **javascriptcoregtk-4.1 2.52.3**, **gtk+-3.0 3.24.52**, **libsoup-3.0 3.6.6** but **no
  bun/node on PATH**.
- BUT the home dir is shared into the toolbox, so the host's bun/node binaries **run inside
  the toolbox** by absolute path: `toolbox run -c saffron-build bash -lc
  '/var/home/saffronjam/.bun/bin/bun --version'` → `1.3.5`; the node binary likewise. So
  **the entire stack (bun + cargo + rustc + webkit2gtk-4.1) is available in the toolbox** —
  spike-0a is effectively pre-resolved: build everything in the toolbox, with the host bun
  on PATH. `tauri-cli` is not installed yet (no `~/.cargo/bin`); it ships as the
  `@tauri-apps/cli` bun devDep (the worktree uses `bun run tauri`/`bun run tauri:dev`), so no
  separate cargo-installed CLI is required. The worktree `editor/README.md:20-27` already
  documents this exact toolbox-run recipe.

### MVP frontend (worktree, to study + replace)
- `wt:editor/src/main.tsx` (458 lines): a single-file React 19 app — `App()` +
  `VectorEditor` + `formatNumber`, rendered via `createRoot(...).render(<StrictMode><App/>)`
  at `:453-457`. Only `main.tsx` + `styles.css` (258 lines).
- Stack (`wt:editor/package.json`): React 19.2, Vite 7.2 + `@vitejs/plugin-react` 5,
  TS 5.9 (strict), `lucide-react` 0.468, `@tauri-apps/api` ^2.8, `@tauri-apps/cli` ^2.8
  (devDep). Scripts: `dev` = `vite --host 127.0.0.1`, `build` = `tsc && vite build`,
  `check` = `tsc --noEmit`, `tauri`/`tauri:dev`.
- `wt:editor/vite.config.ts`: `strictPort: true, port: 1420`, `envPrefix ["VITE_","TAURI_"]`,
  `build.target es2022`, `minify false`.
- `wt:editor/tsconfig.json`: `strict`, `moduleResolution "Bundler"`, `noEmit`, `jsx
  react-jsx`, `include ["src"]`.
- THREE load-bearing mechanisms to port (not the layout):
  - **Drag-scrub** `VectorEditor` (`wt:main.tsx:374-444`): pointer-capture on an axis label,
    `clientX` delta × step, numeric `<input>` `stopPropagation` on `onPointerDown`. → becomes
    the shared `NumberDrag`/`VectorEditor` primitive (phase-6).
  - **Write-coalescing** `queueTransform` (`wt:main.tsx:104-144`): buffers the latest part,
    throttles to ≥4ms between invokes, tracks `sent/completed/inFlight` counters. → becomes
    the generic `coalesce.ts` helper (THIS phase).
  - **Bounds-sync** (`wt:main.tsx:156-190`): `ResizeObserver` + a 250ms `setInterval` +
    `window 'resize'` → `resize_native_viewport` only on a real bounds diff. → becomes
    `ViewportPanel`'s bounds glue (THIS phase), generalized with a resize-end commit +
    `scaleFactor`.
- All engine I/O goes through `invoke()` → the 8 Rust shims. **No socket access, no typed
  client, no polling, no event channel.** Capabilities = `core:default` only
  (`wt:editor/src-tauri/capabilities/default.json`).
- The viewport region is a placeholder `div` (`viewport-host` ref, `wt:main.tsx:306-357`)
  that the engine reparents its X11 window **over**; it never renders pixels and only forwards
  pointer/wheel. The placeholder `<div className="viewport-placeholder">` at
  `wt:main.tsx:352-356` is what becomes the loading overlay.

### MVP Rust bridge (worktree, to replace with one passthrough)
- `wt:editor/src-tauri/src/lib.rs` (411 lines): the WORKING bridge. `EditorState { engine:
  Mutex<Option<Child>>, viewport: Mutex<ViewportBridge>, socket_path }` (`:12-26`), with
  `socket_path = /tmp/saffron-editor-<uid>.sock` (`:23`) — **per-uid, not per-pid** (two app
  instances collide).
- `control_request_with_params(socket_path, cmd, params)` (`:118-160`): fresh `UnixStream`,
  500ms read timeout, writes `{id:1, cmd, params}\n`, reads until `\n`, returns `result` on
  `ok:true` else `Err(error)`. This is the one helper the generic passthrough keeps.
- `parent_xid(window)` (`:89-97`): only `RawWindowHandle::Xlib(h) => h.window` — **X11-only**,
  errors on any other handle.
- `start_engine` (`:274-306`): spawns the binary with `SAFFRON_EDITOR_NATIVE_VIEWPORT=1`,
  `SAFFRON_CONTROL_SOCK=<socket_path>`, `SDL_VIDEODRIVER=x11`, `current_dir(repo_root())`,
  inherited stdout/stderr; then polls `viewport-native-info` **20×100ms** (`:291-298`) — a
  fragile fixed 2s with no child-liveness check.
- `attach_native_viewport` (`:308-341`): reads the XID, sends `attach-native-viewport
  {parentXid,x,y,width,height}`. The ONLY place attach fires, **user-triggered** by a button.
- `resize_native_viewport` (`:343-364`): **re-sends the full `attach-native-viewport`** every
  tick → re-XReparents → flicker. (Phase-1 adds a dedicated `resize-native-viewport` engine
  command that only `XMoveResizeWindow`s; this phase makes the Rust shim call that.)
- `state_snapshot` (`:178-194`): `engine_running` from `engine.as_ref().map(|_| true)` — i.e.
  `Option::is_some`, **never `child.try_wait()`** → reports a dead engine as running (THE bug to
  fix).
- No teardown: nothing kills the child or unlinks the socket on app exit → engine leaked.
- The 8 shims registered at `:393-402`: `start_engine`, `attach_native_viewport`,
  `resize_native_viewport`, `viewport_pointer`, `get_cube_transform`, `set_cube_transform`,
  `set_gizmo_mode`, `set_gizmo_space`. `get_cube_transform`/`set_cube_transform` hardcode the
  first/"Cube" entity (`first_entity` `:196-217`).
- `wt:editor/src-tauri/src/viewport_bridge.rs` (417 lines, GTK4 binary) is DEAD (never spawned)
  + the `[[bin]] viewport-bridge` target + `gdk4/gtk4 0.9`/`glib 0.20` deps in
  `wt:Cargo.toml`. **NONE of this is brought to main** (phase-1 decision; nothing exists on
  main to quarantine).
- `wt:editor/src-tauri/Cargo.toml`: `tauri 2.8.0`, `serde`/`serde_json`, `libc`,
  `raw-window-handle 0.6`, `thiserror`, `base64` (used only by the dead bridge); `[lib] name =
  saffron_editor_lib`, `crate-type [staticlib, cdylib, rlib]`. `wt:main.rs` =
  `fn main() { saffron_editor_lib::run(); }`. `wt:build.rs` = `tauri_build::build()`.
- `wt:tauri.conf.json`: `identifier dev.saffron.engine.editor`, `beforeDevCommand "bun run
  dev"`, `devUrl http://127.0.0.1:1420`, `frontendDist ../dist`, one window 1600×900
  (min 900×620), `security.csp null`, `bundle.active false`.

### Engine control plane (main, the wire contract the client speaks)
- Newline-delimited JSON over a unix socket. Request `{id, cmd, params}`; reply
  `{id, ok, result|error}` (`control_server.cpp:213-236`, `dispatch`).
- Socket path resolution: `SAFFRON_CONTROL_SOCK` → `XDG_RUNTIME_DIR/saffron-control.sock` →
  `/tmp/saffron-control-<uid>.sock` (`control_server.cpp:147-158`). The `se` CLI mirrors this
  (`tools/se/source/main.cpp:21-31`). So passing `SAFFRON_CONTROL_SOCK` from Rust fully
  controls the path → **per-pid sockets are just an env-var choice**.
- `params` are loose: `positionalOr` reads `params[name]` else `params.args[idx]` else null
  (`control_server.cpp:50-61`, decl `command.cppm:54-56`) → the TS client always sends NAMED
  params.
- Every `Uuid` is `{ value: u64 }` (`core.cppm:49-53`), emitted as a raw unsigned that can
  exceed `Number.MAX_SAFE_INTEGER` → **ids are `string` end-to-end in TS, never a JS number**
  (the worktree got this wrong: `wt:lib.rs:72` types `id: u64` and `wt:main.tsx:14` types
  `id: number`).
- `render-stats` is a 21-field flat bag (`control_commands_render.cpp:38-61`):
  `drawCalls/batches/instances/blasCount/pipelines` (u32),
  `clustered/depthPrepass/shadows/ibl/ssao/contactShadows/ssgi/ddgi/rtSupported/rtShadows/restir/hdr`
  (bool), `exposureEv` (f32), `aa` (str). The client's `renderStats()` returns this typed.
- `quit` command (`control_commands_asset.cpp:236-241`): sets `ctx.window.shouldClose = true`
  → the app loop exits, `waitGpuIdle` then `onExit` run (`app.cppm:119-194`). The Rust teardown
  sends `quit` first, then `child.kill()` as a fallback.
- `SAFFRON_EXIT_AFTER_FRAMES=N` bounds a run for headless verification (`app.cppm:52-62`).

### What this phase deliberately does NOT touch
- No new engine `newControlCommands` (phase-2 already added them; phase-4 adds
  `gizmo-pointer`/`billboard-pick`).
- No data panels (hierarchy/inspector/assets/env) — phases 5-8. This phase ships the
  `ViewportPanel` + `LoadingOverlay` + topbar shell stub only.
- No gizmo/billboard rendering or selection round-trip — phase-4.

## Implementation

Two spikes run FIRST (they gate everything and are cheap given the measurements above), then
the scaffold, then the lifecycle. Build/run everything in the toolbox with the host bun on
PATH:
```sh
toolbox run -c saffron-build bash -lc '
  export PATH="/var/home/saffronjam/.bun/bin:$PATH"
  cd /var/home/saffronjam/repos/SaffronEngine/editor
  bun install && bun run check
'
```

### Step 0a — SPIKE: confirm the toolbox frontend build (gates scaffolding)
1. In the toolbox, with the host bun on PATH, run `bun install` in a scratch
   `editor/` then `bun run check` and `cargo build` under `editor/src-tauri` (a bare
   `tauri 2.8` lib). Confirm webkit2gtk-4.1 links (it is present: 2.52.3).
2. Smoke `bun run tauri dev` once the scaffold exists (step 1) under
   `WEBKIT_DISABLE_DMABUF_RENDERER=1` if llvmpipe/webkit DMABUF is flaky on the software GPU.
3. **Record the decision** in this file's Notes: expected outcome — **build in the toolbox,
   host bun on PATH**; host-only build is the documented fallback only if webkit/tauri-cli
   prove unusable in the toolbox (they do not appear to be). This also feeds the phase-10 CI
   choice.

### Step 0b — SPIKE: keyboard/pointer into the reparented child (gates phase-4 input model)
1. With phase-1 in place, launch `SaffronEditor` native-viewport, `se attach-native-viewport`
   it over a test X11 parent, then check: do `onKeyPressed`/SDL key events reach the reparented
   SDL window while the Tauri webview holds focus? Add a temporary `logInfo` on
   `window.onKeyPressed` in the engine and press WASD with the webview focused vs the viewport
   focused (click into the child rect first).
2. Likewise verify raw SDL pointer move/up/wheel reach the child (the worktree only forwarded
   `down` → `click-viewport`, `wt:lib.rs:371-382`; move/up/wheel were dropped).
3. **Record the decision**: the plan DEFAULT (and the safe assumption) is **control-command-
   driven** camera + gizmo — phase-4 drives `get-camera`/`set-camera` + `set-gizmo` +
   `gizmo-pointer` (NDC) rather than relying on native key/pointer reaching the child. If the
   spike proves keyboard reliably reaches the child, native input becomes an optimization, not a
   dependency. Either way phase-3 commits nothing to native input.

### Step 1 — Scaffold the Tauri app under `editor/` (scalable tree)
Create the project (do NOT bring `viewport_bridge.rs`, the `[[bin]] viewport-bridge` target, or
the `gdk4/gtk4/glib` deps):

- `editor/package.json` — same stack as the worktree plus `zustand` (state) and the codegen
  devDep `json-schema-to-typescript`; add scripts: `gen:protocol` =
  `bun run scripts/gen-protocol.ts`, `prebuild`/`predev`-equivalent wiring so `check`/`build`
  run codegen first (Bun: a `"check": "bun run gen:protocol && tsc --noEmit"`, `"build": "bun
  run gen:protocol && tsc && vite build"`). Add `@tauri-apps/plugin-dialog` (used in phases
  7-8; declare now so capabilities are set up once).
- `editor/vite.config.ts`, `editor/tsconfig.json`, `editor/index.html` — copy the worktree's
  (they are correct: `strictPort:true port:1420`, strict TS bundler, `#root` + module script).
- `editor/src-tauri/Cargo.toml` — `tauri 2.8.0`, `serde`/`serde_json`, `libc`,
  `raw-window-handle 0.6`, `tauri-plugin-dialog`. `[lib] name = saffron_editor_lib`, crate-type
  `[staticlib, cdylib, rlib]`. **No gtk/gdk/glib, no `[[bin]]`, no base64/thiserror unless used.**
- `editor/src-tauri/src/main.rs` = `fn main() { saffron_editor_lib::run(); }`.
- `editor/src-tauri/build.rs` = `tauri_build::build()`.
- `editor/src-tauri/tauri.conf.json` — copy the worktree's; keep `identifier
  dev.saffron.engine.editor`, `beforeDevCommand "bun run dev"`, `devUrl
  http://127.0.0.1:1420`, `frontendDist ../dist`, `bundle.active false`.
- `editor/src-tauri/capabilities/default.json` — `core:default` + `dialog:default` + the event
  permissions needed for `app.emit`/listen (`core:event:default`).
- `editor/src/` tree:
  - `editor/src/main.tsx` — ~10-line bootstrap: `createRoot(#root).render(<StrictMode><App/>)`.
  - `editor/src/app/App.tsx` — top shell: topbar stub + `<ViewportPanel/>` + `<LoadingOverlay/>`;
    starts the reconcile poll; subscribes to the `viewport-attached`/`viewport-error` events.
  - `editor/src/app/LoadingOverlay.tsx` — the overlay (step 6).
  - `editor/src/panels/ViewportPanel.tsx` — the placeholder div + bounds-sync glue (step 7).
  - `editor/src/control/client.ts` — the typed client (step 4).
  - `editor/src/control/coalesce.ts` — the generic coalesced-write helper (step 4).
  - `editor/src/state/store.ts` — the Zustand store + reconcile poll (step 4).
  - `editor/src/protocol/index.ts` — GENERATED (step 3), gitignore the generated body or commit
    it deterministically (commit it so `bun run check` works without codegen on a fresh clone;
    the contract test + `gen:protocol` keep it honest).
  - `editor/scripts/gen-protocol.ts` — the codegen step (step 3).

### Step 2 — One generic Rust passthrough + window-handle commands
Rewrite `editor/src-tauri/src/lib.rs` (port the good parts from `wt:lib.rs`; delete the 8
bespoke shims):

- Keep `control_request_with_params` (`wt:lib.rs:118-160`) and `parent_xid` (`:89-97`,
  X11-only), `repo_root` (`:99-105`), `engine_binary` (`:80-87`, honoring
  `SAFFRON_ENGINE_BIN`).
- `EditorState { engine: Mutex<Option<Child>>, viewport: Mutex<ViewportBridge>, socket_path:
  String }` — but compute `socket_path` **per-pid in `XDG_RUNTIME_DIR`**:
  `format!("{}/saffron-editor-{}.sock", runtime_dir, std::process::id())` (fallback `/tmp` if
  `XDG_RUNTIME_DIR` unset). Pass it via `SAFFRON_CONTROL_SOCK` on spawn so the engine binds it
  (`control_server.cpp:149`). This gives two app instances **distinct sockets/engines**.
- ONE generic command:
  ```rust
  #[tauri::command]
  fn control(state: State<'_, EditorState>, cmd: String, params: Option<serde_json::Value>)
      -> Result<serde_json::Value, String> {
      control_request_with_params(&state.socket_path, &cmd, params.unwrap_or(json!({})))
  }
  ```
  Adding any new `se`-command then needs ZERO Rust change. `ok:false` already surfaces as
  `Err(error)` from `control_request_with_params:155-159` → a typed rejection on the TS side.
- Keep ONLY the window-handle commands (they need the Tauri window/process):
  - `start_engine` — spawn (env as above) **then** the hardened readiness poll (step 5).
    Returns an `EngineStatus`-shaped struct, not the worktree `ViewportState`.
  - `attach_native_viewport(window, bounds)` — read `parent_xid`, call
    `attach-native-viewport` via `control_request_with_params` (NOT the generic `control`,
    because it injects the XID).
  - `resize_native_viewport(window, bounds)` — call the **new phase-1
    `resize-native-viewport`** engine command (XMoveResize only, no reparent) instead of
    re-sending the full attach (fixes `wt:lib.rs:343-364` flicker).
  - `quit_engine(state)` — send `quit` over the socket, then `child.kill()` + `child.wait()` +
    `unlink(socket_path)` as fallback.
- Fix `state_snapshot`/liveness: add `fn engine_alive(state) -> bool` using
  `child.try_wait()` (`Ok(None)` ⇒ alive; `Ok(Some(_))`/`Err` ⇒ dead) — NEVER `Option::is_some`
  (the `wt:lib.rs:178-194` bug). Expose an `engine_alive` command the watchdog polls.
- App-exit teardown: register an `on_window_event(CloseRequested)` / `RunEvent::ExitRequested`
  handler that runs `quit_engine` logic (best-effort `quit` → `kill` → `unlink`). No orphan, no
  stale socket.
- `run()` registers `[control, start_engine, attach_native_viewport, resize_native_viewport,
  quit_engine, engine_alive]` and the `.setup()` auto-start hook (step 5).
- Add `tauri_plugin_dialog::init()` to the builder (used later; harmless now).

### Step 3 — Generate `@saffron/protocol` from the phase-2 schemas
- `editor/scripts/gen-protocol.ts` — read every `schemas/control/*.schema.json` (repo root,
  authored in phase-2), run `json-schema-to-typescript` (`compileFromFile`) and write
  `editor/src/protocol/index.ts` with: the shared DTOs (`Vec3`, `Vec4`, `Uuid` = `string`,
  `EntityRef`, `TransformC`, `MaterialC`, `CameraC`, the light DTOs, `InspectResult`,
  `RenderStats`, `Environment`, `AssetEntry`, `Selection`, `GizmoState`, etc.), and a
  `CommandResultMap` mapping each command name → its result type (hand-written aggregator over
  the per-command result schemas, or generated from a `commands.schema.json` index). Enforce
  `additionalProperties:false` handling and `u64`-as-`string` (the schema types every id field
  as `string`).
- Wire it: `bun run check`/`build` run `gen:protocol` first (the package.json scripts in
  step 1). Re-running must be **deterministic** (byte-identical output) — `json-schema-to-
  typescript` is stable given sorted inputs; sort the schema file list.
- `editor/src/protocol/index.ts` is committed (so a fresh clone type-checks) and is the only
  thing the client imports for wire types — never hand-edit it.

### Step 4 — Typed client + coalesce helper + Zustand store + reconcile poll
- `editor/src/control/client.ts`:
  - `async function call<C extends keyof CommandResultMap>(cmd: C, params?: object):
    Promise<CommandResultMap[C]>` — wraps Tauri `invoke('control', { cmd, params })`; the Rust
    layer already turns `ok:false` into a rejected promise (a thrown `Error(message)`), so
    `call` just types the resolve. A `bigint`-safe / string-preserving guard: ids are already
    strings in the schema, but parse JSON defensively so a u64 never round-trips through a JS
    number (use the value as-returned; do not `Number()` any id).
  - One typed method per command, NAMED params only, ids as strings:
    `listEntities()`, `inspect(id)`, `setTransform(id, Partial<TransformC>)`,
    `addComponent`/`removeComponent`/`setComponent`/`setComponentField`, `pick`, `deselect`,
    `getSelection`, `addEntity(preset)`, `copyEntity(id)`, `getGizmo`/`setGizmo`,
    `getCamera`/`setCamera`, `listAssets`, `getThumbnail(id,size)`,
    `importModel`/`importTexture`, `assignAsset`, `renameAsset`,
    `getEnvironment`/`setEnvironment`, `saveProject`/`loadProject`, `saveScene`/`loadScene`,
    `renderStats`, plus the `set-*` render toggles. (Methods whose engine command lands in
    phase-2 are typed now; methods for phase-4 commands like `gizmoPointer`/`billboardPick`
    are added in phase-4.)
  - `start`/`attach`/`resize`/`quit` thin wrappers over the dedicated Rust commands +
    `engineAlive()`.
- `editor/src/control/coalesce.ts` — port + generalize `queueTransform`
  (`wt:main.tsx:104-144`): `makeCoalescer<T>({ throttleMs = 4, send: (latest: T) =>
  Promise<void> })` returning `push(value)`; buffers the latest, throttles, tracks
  `sent/completed/inFlight`. Backs every high-frequency mutation (gizmo echo, scrub fields,
  sliders) in phases 4/6.
- `editor/src/state/store.ts` — a Zustand store:
  ```ts
  { entities: EntityRef[], selectedId: string | null, sceneVersion: number,
    selectionVersion: number, componentsBySelected: InspectResult | null,
    assets: AssetEntry[], environment: Environment | null, renderStats: RenderStats | null,
    engineStatus: { running: boolean; attached: boolean;
                    phase: 'idle'|'starting'|'attaching'|'ready'|'error'; error?: string },
    dragActive: boolean }
  ```
  Plus actions to set each slice and `setPhase`.
- Reconcile poll (focus-gated ~5-10Hz, the liveness watchdog too): each tick, only when the
  window is focused and `phase === 'ready'`:
  - `engineAlive()` → if false, `setPhase('error')` (crash recovery, step 6).
  - `getSelection()` (cheap, frame-stamped) + `renderStats()` every tick.
  - re-fetch `listEntities()` ONLY when `sceneVersion` changed (from the phase-2 sceneVersion
    surfaced in `get-selection`/`render-stats`); re-`inspect(selectedId)` ONLY when
    `selection`/`sceneVersion` changed.
  - writes are gated OFF while `dragActive` (avoid clobbering optimistic local state during a
    drag). The store exposes `setDragActive`.
  - The poll skeleton lands now; the data panels (phases 5-8) consume the slices it fills.

### Step 5 — Auto-start + auto-attach state machine (`.setup()` + React)
Rust `.setup()` (replaces the worktree's title-only `.setup()` at `wt:lib.rs:403-408`):
1. Spawn `SaffronEditor` (env `SAFFRON_EDITOR_NATIVE_VIEWPORT=1`, `SAFFRON_CONTROL_SOCK=<per-
   pid socket>`, `SDL_VIDEODRIVER=x11`, `current_dir(repo_root())`); `engine_binary()` honors
   `SAFFRON_ENGINE_BIN`. `app.emit('engine-phase', 'starting')`.
2. **Child-liveness-aware bounded readiness poll** (replaces `wt:lib.rs:291-298`): up to ~10s
   with exponential backoff (e.g. 50→100→200→400ms, capped), each iteration:
   `child.try_wait()` first — if the child exited, emit `viewport-error` with the exit status
   and STOP (do not keep polling a dead socket); else try `control(viewport-native-info)` —
   on the first success (`status == "engine-window-ready"`), break. `app.emit('engine-phase',
   'attaching')`.
3. The actual `attach` is driven from React (Rust cannot read the div rect): React's
   `ViewportPanel` `useLayoutEffect` + `ResizeObserver` waits for a **non-zero** rect, then
   calls `attach_native_viewport({ bounds: rectInDevicePx })` where
   `rectInDevicePx = cssRect * await getCurrentWindow().scaleFactor()` (HiDPI correction the
   worktree omits — `wt:main.tsx:55-66` sends CSS px). On success `setPhase('ready')` +
   `app.emit('viewport-attached')`; on failure `app.emit('viewport-error', message)` +
   `setPhase('error')`.
4. React state machine in `App.tsx`: subscribe to `engine-phase`/`viewport-attached`/
   `viewport-error` (Tauri `listen`); drive `engineStatus.phase`
   (`idle→starting→attaching→ready` | `error`). Show `<LoadingOverlay/>` until `phase ===
   'ready'`.

### Step 6 — LoadingOverlay + runtime crash recovery
- `editor/src/app/LoadingOverlay.tsx` — absolutely-positioned, rendered while `engineStatus.
  phase !== 'ready'`. CRITICAL: the reparented X11 child ALWAYS paints on top of its rect once
  mapped, so the overlay must occupy the viewport region **while the native window is NOT yet
  mapped** (the engine only `XMapRaised`'s on a successful attach — `wt:control_commands_
  render.cpp:139`). Content: a spinner + "Preparing renderer…"; on `phase === 'error'` show the
  typed error + a **Retry** button (re-run start→attach from `idle`) and a **Restart** button
  (kill + re-spawn + re-attach).
- Runtime crash recovery: the reconcile-poll watchdog (step 4) calls `engineAlive()` each
  tick; on a mid-session death it `setPhase('error')`, which re-shows the overlay over the now-
  dead viewport rect. Restart calls `quit_engine` (best-effort) then `start_engine` →
  re-attach. (The native window is gone when the child died, so the overlay is unobstructed.)
- The MVP manual `Start Engine`/`Attach Viewport` buttons (`wt:main.tsx:257-264`) are
  **removed** — start/attach are automatic; the only buttons are Retry/Restart in the error
  state.

### Step 7 — ViewportPanel bounds-sync glue (generalized)
- `editor/src/panels/ViewportPanel.tsx` — the placeholder `div` (ref) the native window
  reparents over; port the bounds-sync from `wt:main.tsx:156-190` and generalize:
  - `ResizeObserver` on the div + `window 'resize'` listener.
  - Compute bounds from `getBoundingClientRect()`, multiply by
    `await getCurrentWindow().scaleFactor()`, round.
  - Fire `resize_native_viewport({ bounds })` only on a real diff vs the last sent bounds.
  - Add a **resize-end commit** (debounce ~120ms after the last resize event) so a final exact
    bounds is always sent (covers the case where the throttle drops the last frame).
  - This panel re-fires on every dock/panel split-resize (phase-9 layout) so the native window
    stays glued. The native window cannot be drawn over by the webview, so any overlay/popover
    (loading overlay now; context menus/asset-view modal later) must live in a sibling stacking
    layer outside the native rect or while the window is unmapped — documented as the strategy
    in Risks.

### Step 8 — Verify (toolbox)
- `bun run check` passes against the generated `protocol/`.
- `cargo build` under `editor/src-tauri` (toolbox) links webkit2gtk-4.1.
- `bun run tauri dev` (toolbox, host bun on PATH, `WEBKIT_DISABLE_DMABUF_RENDERER=1` if
  needed): the app auto-spawns SaffronEditor, shows "Preparing renderer…", then reveals the
  embedded live scene with NO clicks. Close the window → engine dies, socket gone (`ls
  $XDG_RUNTIME_DIR/saffron-editor-*.sock`).
- Kill the engine mid-session (`pkill SaffronEditor`) → overlay returns with a working Restart.
- Two `bun run tauri dev` instances use distinct pid-scoped sockets without collision.
- `editor-old/` C++ ImGui editor still builds (`cmake --build build/debug -j1`).

## Done when

- [ ] Both spikes are documented with a concrete decision recorded in this file's Notes
      (spike-0a: build in the toolbox with host bun on PATH; spike-0b: native-input vs
      control-command camera/gizmo, defaulting to control-command).
- [ ] Starting the Tauri app auto-spawns SaffronEditor, shows "Preparing renderer…", then
      reveals the embedded live viewport with NO button clicks.
- [ ] Closing the window terminates SaffronEditor (no orphan process, no stale
      `saffron-editor-<pid>.sock`).
- [ ] Killing the engine mid-session flips to an error overlay with a working Restart that
      re-spawns + re-attaches.
- [ ] A simulated spawn failure (e.g. `SAFFRON_ENGINE_BIN=/nonexistent`) shows the typed error
      + a working Retry.
- [ ] `editor/src/protocol/` is generated from `schemas/control` and re-running `gen:protocol`
      reproduces it byte-identically; `bun run check` passes.
- [ ] `client.listEntities()` / `client.renderStats()` return typed results from React; all
      ids are `string` (no JS `number` for any Uuid).
- [ ] The generic `control(cmd, params)` passthrough surfaces `ok:false` as a typed rejection
      (a thrown/`reject`ed `Error(message)`), verified with a deliberately bad command.
- [ ] Two app instances use distinct pid-scoped sockets/engines without collision.
- [ ] `state_snapshot`/`engine_alive` uses `child.try_wait()` (not `Option::is_some`); a dead
      child reports `running:false`.
- [ ] `resize_native_viewport` calls the phase-1 `resize-native-viewport` command (no reparent
      flicker on panel resize).
- [ ] HiDPI: attach/resize bounds are multiplied by `window.scaleFactor()` (verified the
      native window exactly covers the div on a fractional-scale display, or documented as N/A
      at 1.0).
- [ ] `viewport_bridge.rs`, the `[[bin]] viewport-bridge` target, and the gtk4/gdk4/glib deps
      are NOT present in `editor/src-tauri`.
- [ ] `editor-old/` C++ ImGui editor still builds via CMake (`cmake --build build/debug -j1`,
      validation-clean).

## Risks / seams

- **The X11 child paints over its rect.** The loading overlay, and every future element that
  must overlap the viewport (phase-7 asset-view modal, phase-9 dropdowns/popovers), must live
  in a sibling stacking layer that is either outside the native rect or shown only while the
  native window is unmapped. Strategy fixed here: the overlay occupies the viewport region only
  before the engine `XMapRaised`'s (on attach) and again after a crash (window gone); modals/
  popovers later either position outside the rect or temporarily lower/unmap the native window.
- **Readiness poll must not hang on a crashed child** — the `child.try_wait()`-first loop fixes
  the worktree's blind 20×100ms; bound to ~10s and emit `viewport-error` on child exit.
- **HiDPI scaleFactor** — the worktree sends CSS px; this phase multiplies by
  `getCurrentWindow().scaleFactor()`. Mis-sizing shows as the native window not covering the
  div; the resize-end commit re-sends an exact final bounds.
- **Generic passthrough must surface `ok:false` as a typed rejection** — relies on
  `control_request_with_params:152-159` returning `Err(error)`, which Tauri turns into a
  rejected promise; the typed `call<R>` only narrows the resolve type.
- **Spike-0a (build env)** — pre-measured as resolvable in the toolbox (cargo+rustc+webkit2gtk
  present, host bun runs via shared home). Fallback: build the frontend on the host and only
  the engine in the toolbox. Resolve before scaffolding.
- **Spike-0b (input routing)** — if keyboard cannot reach the reparented child, phase-4's
  camera/gizmo MUST be control-command-driven (`get-camera`/`set-camera` + `set-gizmo` +
  `gizmo-pointer`). The plan defaults to that; native input is an optimization only.
- **Per-pid socket vs single engine** — the plan assumes one engine per Tauri window with a
  pid-scoped socket in `XDG_RUNTIME_DIR`; revisit only if a single engine must serve multiple
  windows (not a current requirement).

## Notes

- Toolbox run recipe (already documented in `wt:editor/README.md:20-27`):
  ```sh
  toolbox run -c saffron-build bash -lc '
    export PATH="/var/home/saffronjam/.bun/bin:$PATH"
    cd /var/home/saffronjam/repos/SaffronEngine/editor
    bun install && bun run tauri dev
  '
  ```
- Non-goals (parity-correct; the C++ editor lacks them too, so they are not gaps here):
  undo/redo, scene-graph parenting, multi-viewport, native-Wayland (XWayland-only via the
  `parent_xid` Xlib requirement, `wt:lib.rs:93-96`).
- This phase ships infrastructure only: after it, the embedded viewport shows the live scene
  but has no in-app interaction (phase-4) and no data panels (phases 5-8). The topbar gizmo
  group + world/local in `wt:main.tsx:213-256` are NOT wired here (no `set-gizmo` calls until
  the engine state is unified in phase-2 and the overlay renders in phase-4) — render them as
  inert stubs or omit until phase-4.
