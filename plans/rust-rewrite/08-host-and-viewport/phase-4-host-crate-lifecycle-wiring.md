# Phase 4 — saffron-host: the HostLayer, lifecycle wiring, control poll, and parent-death watch

**Status:** COMPLETED

**Depends on:** 08-host-and-viewport:phase-1-app-crate-run-loop-and-layer, 08-host-and-viewport:phase-2-shm-seqlock-publisher, 03-ecs-and-scene, 04-animation, 09-control-plane:phase-1-socket-server-and-dispatch

## Goal

Port `runHost` and `HostState` into the `saffron-host` bin: build the `HostLayer` that owns the editor
context / asset server / animation runtime, run the per-frame `on_update` (parent-death watch →
`poll_control` → thumbnail drain → preview-prune → `tick_animation` → `tick_play` → gizmo-drag-step →
edit-smoothing → fly-cam), the `on_ui` (set catalog, sync gizmo, `render_scene` + overlay submit), and
the `on_create`/`on_detach` lifecycle including the play-state subscriptions and the
`wait_gpu_idle`-respecting teardown. This wires the runnable spine into a real editor host driven over
the control plane. The native overlay *geometry* is phase-5; this phase calls
`submit_scene_edit_overlay` as a seam (a stub that submits empty ranges until phase-5 fills it).

## Why this shape (NO LEGACY)

- **`HostState` (shared_ptr captured by 12 closures) → one `HostLayer` owning state by value.** The
  closures shared state only because C++ closures cannot otherwise; a Rust `Layer` impl carries the
  fields directly (`editor: SceneEditContext`, `assets: AssetServer`, `animation: AnimationRuntime`,
  `physics: Option<PhysicsWorld>`, `contact_cursor: u64`, `script_vm_active`/`script_error_pending`/
  `shm_publish`/`preview_active: bool`, the two `SubscriptionId`s). No `Arc`, no `Rc<RefCell>` — the host
  is single-threaded and owned by the run loop. `ScriptHost` + `PhysicsWorld` are wired in their own
  phases (12-scripting, 05-physics); this phase carries the `Option`/seam fields and leaves the
  build/drop bodies to those areas (`simTick`'s physics+script composition is filled when they land).
- **`newSceneEditContext`/`destroySceneEditContext` heap dance deleted.** 03-ecs-and-scene removed the
  raw-pointer ownership; `HostLayer` owns `SceneEditContext` by value and `Drop` frees it.
- **The two `onPlayStateChanged.subscribe` hooks → `saffron-signal` subscriptions held as fields.** The
  script-VM-on-play and physics-world-on-play lifecycle hooks port to closures subscribed in `on_create`
  and `unsubscribe`d in `on_detach`, holding `SubscriptionId`. The C++ `simTick` `std::function` seam
  (host-filled `Box<dyn FnMut(&mut Scene, f32)>`) keeps physics/script deps out of `saffron-sceneedit`
  (the 03 design); this phase installs it (initially the no-physics path; 05/12 fill the rest).
- **`EngineContext` is a per-frame disjoint-borrow struct, assembled in `poll_control`, never stored**
  (the 09 design). The host builds it from its own `&mut` fields each frame: `{ window, renderer,
  scene_edit, assets, physics: Option<&mut> }`. This is why the host ports after control's phase-1 — it
  is the caller that supplies the borrow.
- **The one host-owned control command (`get-script-schema`) registers here**, not in saffron-control,
  because it needs the Lua schema reader and the host is the only crate that may depend on
  saffron-script (12-scripting). Until scripting lands it can register a stub that errors "scripting not
  yet wired" — but the registration site is the host (NO LEGACY: one registration place per the 03/07
  registry rule). [Carried: if PP-14 orders scripting after this phase, the command is a stub here and
  filled by 12; flagged in open questions.]
- **Parent-death watch via `rustix::process::getppid`.** `editor_spawned = env SAFFRON_EDITOR_NATIVE_VIEWPORT
  set`; capture `editor_pid = getppid()` once in `on_create`; each `on_update`, `if editor_spawned &&
  getppid() != editor_pid { app.window_should_close = true }`. The Escape-key exit subscription survives
  only in the standalone windowed path.
- **Self-tests deleted.** None of the ten `host.cppm:1313-1323` startup self-tests is ported; they are
  `#[cfg(test)]` units in their owning crates. The host has no startup self-test call.

## Grounding (real files/symbols)

- `engine-old/source/saffron/host/host.cppm`: `runHost` (983), `HostState` (51-67), `config.onCreate`
  (995-1572: editor/control/assets construction, `clipLoader` install, `get-script-schema` registration
  1009-1034, `setPresentViewportOnly(true)` 1037, `enableViewportShmPublish` 1043-1052, `setAa(4)` 1055,
  `registerBuiltinComponents` 1059, the two `onPlayStateChanged.subscribe` 1064-1137, `simTick` 1142),
  `layer.onUpdate` (1462-1521: parent-death 1464-1468, `pollControl` 1472, `drainThumbnailCompletions`
  1475, preview-prune 1479-1484, `tickAnimation` 1493, `tickPlay` 1496, `scriptErrorPending`→`pausePlay`
  1500-1505, `stepNativeGizmoDrag` 1510, `stepEditSmoothing` 1514, fly-cam 1518-1520), `layer.onUi`
  (1527-1559: `live.catalog` set 1533, `setViewportDesiredSize` non-publish 1536-1539, `syncNativeGizmo`
  1540, `renderCameraView` 1541, `renderScene` 1551, `submitSceneEditOverlay` 1556), the Escape
  subscription (1562-1571), `config.onExit` (1574-1611: `stopThumbnailWorker` 1579, control destroy
  1580-1584, play teardown 1585-1598, `shutdownPhysics` 1601-1605, GPU `Ref` cache clear 1609-1610),
  `editorSpawned`/`editorPid` (1449-1454), `startThumbnailWorker` (1458).
- `engine-old/source/saffron/control/command.cppm`: `EngineContext` (31-38), `pollControl` (148),
  `registerCommand<Params,Result>` (58-76).
- `engine-old/source/saffron/control/control_server.cpp`: `controlSocketPath` (`SAFFRON_CONTROL_SOCK`,
  160-171), `newControlContext`/`destroyControlContext` (329/348), `pollControl` (361).

## Acceptance gate

- Cargo workspace compiles; `cargo build -p saffron-host`; `cargo clippy`/`fmt --check` clean.
- Unit `#[test]`s (CPU-only, no GPU):
  - `host_layer_constructs_without_gpu`: a `HostLayer::new` builds editor + assets + animation runtime
    (clip loader installed) without a renderer; the play-state subscriptions are held as live
    `SubscriptionId`s; dropping the layer unsubscribes them (no dangling subscription).
  - `parent_death_sets_should_close`: with `editor_spawned=true` and a faked `getppid` mismatch, the
    update step requests window close; with a match, it does not.
  - `preview_prune_clears_runtime`: toggling `previewing` clears the animation runtime's
    transitions/last_pose exactly once on the transition edge (matching `previewActive` tracking).
  - `update_order_is_animation_then_tick_play`: a recording seam proves `tick_animation` runs before
    `tick_play` (a play/step command this frame takes effect this frame), and the fly-cam look-delta is
    drained (reset to zero) each update.
- A headless integration step: the host boots with `SAFFRON_EDITOR_NATIVE_VIEWPORT=1` + a control socket
  + `SAFFRON_EXIT_AFTER_FRAMES=N`, answers a `ping` over the socket, publishes frames (phase-2/3 path),
  and tears down with `wait_gpu_idle` before any resource drop, the GPU `Ref` caches cleared before the
  renderer drops, and a validation-clean log. No startup self-test runs (grep the binary's log for the
  removed self-test names → absent).
