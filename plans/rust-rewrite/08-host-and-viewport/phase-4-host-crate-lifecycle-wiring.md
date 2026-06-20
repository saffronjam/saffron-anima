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

## The play-edge world build + sim_tick composition (WIRED)

The play loop is wired. Entering Play builds the Jolt world from the play scene, starts the script VM,
installs the bridge, and points the editor's `sim_tick` seam at the play-loop closure; the per-frame
`update_session` ticks it and writes physics back into the play scene; leaving Play drops the world,
stops the VM, and clears the seam in the `#98` teardown order. The e2e confirms it live:
`physics-falling-box.test.ts` is green (`physics-state.active == true` during Play, the box falls from 5
and settles, `stop` restores the authored y=5, validation-clean), as are the broader physics e2e
(triggers/character/bone/query/ragdoll/world) and the non-coroutine script cases.

The shape that closed it (the structural obstacle was that `sim_tick` is a `Box<dyn FnMut(&mut Scene,
f32)>` stored *inside* `SceneEditContext`, invoked while the editor is borrowed by `tick_play`, so its
closure cannot capture `&mut self`):

- **The play-session mutable state the closure + bridge reach moved behind single-thread
  `Rc<RefCell<…>>` cells** (conventions §3 bucket 4, the Rust shape of the C++ shared `HostState`):
  `physics: SharedPhysics` (`Rc<RefCell<Option<World>>>`), `script: Rc<RefCell<ScriptHost>>`, the
  `contact_cursor`/`script_error_pending` flags (`Rc<Cell<…>>`), the gameplay-input snapshot
  (`Rc<RefCell<ScriptInputState>>`), and the per-frame ragdoll `pose_targets` snapshot
  (`Rc<RefCell<Vec<PoseTarget>>>` filled from `AnimationRuntime::last_poses` after `tick_animation`).
  `poll_control` lends `physics.borrow_mut().as_mut()` (a guard held across `control.poll`) into the
  `EngineContext::physics: Option<&mut World>` borrow.
- **`sa.log` and contained script errors route through sink cells, not the editor.** While a tick runs
  the editor is borrowed by `tick_play`, so the C++ `pushScriptLog(*state->editor, …)` aliasing is
  forbidden in Rust. `script_bridge.rs`'s `log_sink` appends to a `SharedScriptSink` buffer (replacing
  the `SharedEditor` cell), and the `sim_tick` closure buffers errors into a `script_error_sink`; the
  host drains both into the editor's rings after `tick_play` releases the editor borrow, then flips the
  deferred pause.
- **The Edit↔Playing build/teardown is host-side edge detection, not the published hooks.**
  `publish_transition` is `&mut self` on the editor, so a subscribed closure would run while the editor
  is borrowed and could not reach the play scene / project root / registry it must build with. The host
  detects the edge itself in `reconcile_play_edge` (run from `update_session` right after `poll_control`
  releases the editor borrow) and builds the world (`populate` + `build_bone_bodies` + `add_character`)
  + starts the VM + installs `HostScriptBridge` there. The two `on_play_state_changed` subscriptions
  stay as the lifecycle seam markers (the "play hooks are live" invariant), torn down on detach.
- **The `sim_tick` closure runs the faithful sequence per fixed step:** `drive_ragdolls_to_pose →
  advance_ragdoll_blend → step → write_ragdoll_poses → drain_contacts → dispatch_contact → derive input
  edges → tick_scripts`, releasing the world borrow before any script runs (a handler may `sa.raycast`
  back through the bridge).

A unit test (`play_edge_builds_a_world_and_steps_the_box`, CPU-only) mirrors the falling-box e2e end to
end without a renderer.

Running `tick_scripts` for real surfaced one latent `saffron-script` scheduler bug (fixed in
12-scripting): `sa.wait` in a bare `on_update` errored *"attempt to yield across a C-call boundary"*
because mlua's Luau backend runs `Function::call` on an auxiliary thread, so the prelude's
`coroutine.running()` `ismain` guard read false. `scheduler.rs` now tracks the scheduler coroutine it is
resuming (`_sa_active`) and yields only from that coroutine, so a bare-`on_update` `sa.wait` is the
documented ignored no-op. With that, the full `script.test.ts` (20 cases) and `script_logs.test.ts` pass
live against the Rust host.
