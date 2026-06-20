# Phase 6 — the teardown order (the Drop graph): worker-join, wait_gpu_idle, Ref-drop, Jolt shutdown

**Status:** COMPLETED

**Depends on:** 08-host-and-viewport:phase-4-host-crate-lifecycle-wiring, 05-physics-jolt-bridge:phase-3-world-and-rigidbody-core, 12-scripting

## Goal

Lock the full host teardown sequence — the runtime-UAF surface the feasibility study flags as the
host's hardest non-mechanical concern — once physics and scripting are wired so the complete play
teardown exists. Encode the cross-object ordering (thumbnail-worker join before `wait_gpu_idle`,
`wait_gpu_idle` before any resource drop, GPU `Ref` caches dropped before `destroy_renderer`, Jolt
globals shutdown after the last world) by `App`/`HostLayer` field order plus an explicit `Drop`/`on_exit`
sequence, and prove no use-after-free under the validation layer across a play→quit-mid-play teardown.

## Why this shape (NO LEGACY)

- **Teardown order is a runtime UAF, not a compile error** (feasibility §6). Rust's field-order `Drop`
  handles the *within-object* order for free (renderer drops before window by `App` field order; the
  renderer's own `Drop` frees views → VMA → device per 06-rendering phase-3). The **cross-object** facts
  must be explicit, exactly as the C++ comments demand — this phase is where they are pinned, after the
  pieces they sequence exist.
- **`wait_gpu_idle` before any teardown** (`app.cppm:209`): `run` calls it before `on_detach`/`on_exit`,
  so no in-flight command buffer references a resource about to be freed. This is the single most
  load-bearing ordering fact and it lives in `saffron-app::run` (phase-1), re-asserted here with the
  resources that depend on it now present.
- **The thumbnail worker is joined before `wait_gpu_idle`.** The worker borrows the renderer (shares the
  GPU queue + bindless table via the two `Arc<Mutex>`); `stop_thumbnail_worker` drains+joins it in
  `on_detach`/`on_exit`, sequenced before `run`'s `wait_gpu_idle`. The C++ does this first in `onExit`
  (`host.cppm:1579`) precisely because `run` only calls `waitGpuIdle`/`destroyRenderer` after `onExit`
  returns.
- **The play teardown order is fixed**: stop scripts (the VM never touches the scene) → drop the physics
  world (`Option<PhysicsWorld>::take`/drop — RAII frees its Jolt objects, ragdoll-detach-before-destroy
  inside 05-physics' `Drop`) → null the `sim_tick` seam → unsubscribe the two play-state hooks → drop the
  editor context. Then **Jolt globals shutdown only after the last world is gone** (the
  Factory/registered-types outlive all bodies — `host.cppm:1599-1605`).
- **GPU `Ref` caches dropped before `destroy_renderer`.** `assets.mesh_ref_by_uuid.clear()` /
  `texture_ref_by_uuid.clear()` (`host.cppm:1609-1610`) drop the cached `Arc<GpuMesh>`/`Arc<GpuTexture>`
  before the renderer frees the device/allocator — otherwise the last `Arc` drop would free a GPU
  resource after its allocator is gone. In Rust this is either an explicit clear in `on_exit` or,
  cleaner, the `AssetServer` owns the caches and drops before the renderer by `HostLayer` field order;
  the plan picks the field-order form and asserts it with a leak/UAF test.
- **No `Drop` does GPU work after the device is gone.** The shm publisher's `Drop` (munmap/close/unlink,
  phase-2) is independent of the device and may drop any time; the renderer's resource `Drop`s must all
  precede the device `Drop` (06-rendering owns that, the host only ensures the renderer outlives the
  asset caches and the worker).

## Grounding (real files/symbols)

- `engine-old/source/saffron/app/app.cppm`: `waitGpuIdle(app.renderer)` at 209 (before `onDetach`/
  `onExit`), `destroyRenderer`/`destroyWindow` at 233-234 (the final order), the `SAFFRON_CAPTURE` dump
  at 224 (between teardown and renderer destroy).
- `engine-old/source/saffron/host/host.cppm`: `config.onExit` (1574-1611) — `stopThumbnailWorker` first
  (1579, "before any teardown: it borrows the renderer"), control destroy (1580-1584), play teardown
  (1585-1598: `stopScripts` → `physics.reset()` → `simTick = nullptr` → two `unsubscribe` →
  `destroySceneEditContext`), `shutdownPhysics` after the last world (1599-1605), GPU `Ref` cache clear
  (1606-1610, "before destroyRenderer frees the device/allocator — otherwise … use-after-free").
- `engine-old/source/saffron/rendering/renderer_capture.cpp`: `destroyShmPublishSlots`/`destroyShmPublish`
  (306-348) — the munmap/close/shm_unlink order (independent of the device).

## Acceptance gate

- Cargo workspace compiles; `cargo build -p saffron-host`; `cargo clippy`/`fmt --check` clean.
- Unit / integration `#[test]`s:
  - `teardown_unsubscribes_and_drops_in_order`: a `HostLayer` with both play-state subscriptions live,
    a play session active (script VM + physics world present), torn down via `on_detach`/drop — the
    subscriptions are unsubscribed, the physics world drops before the Jolt-globals shutdown, the
    `sim_tick` seam is cleared, and a recording harness proves the order matches the C++ sequence.
  - `ref_caches_drop_before_renderer`: the asset GPU `Ref` caches are emptied (or their owning field
    dropped) strictly before the renderer's device/allocator drop — a drop-order probe (or a `Drop`
    counter on a stub GPU resource) confirms no resource `Drop` runs after the device `Drop`.
  - `shm_drop_is_device_independent`: dropping the `ShmPublish` after the renderer is gone still
    munmaps/unlinks cleanly (no device access in its `Drop`).
- A headless integration step: boot the host with `SAFFRON_EDITOR_NATIVE_VIEWPORT=1`, enter play (so a
  Jolt world + script VM exist), then quit **mid-play** (control `quit` or `SAFFRON_EXIT_AFTER_FRAMES`),
  and assert a validation-clean teardown with **zero** VK validation errors, no use-after-free reported
  by validation, the worker joined before `wait_gpu_idle`, and exit code 0. This is the host's
  end-of-life proof that the ownership re-architecture is UAF-free.

## What landed

- `HostLayer::teardown_recording(&mut Vec<TeardownStep>)` is the one teardown path; `teardown()` (run
  from `on_detach`) calls it with a throwaway record. The cross-object order is pinned and emitted as
  `TeardownStep`s: `WorkerJoined` → `ControlClosed` → `ScriptsStopped` → `PhysicsWorldDropped` →
  `JoltGlobalsShutdown` → `SimTickCleared` → `PlayHooksUnsubscribed` → `GpuCachesCleared`. The
  `teardown_unsubscribes_and_drops_in_order` test reads the record (the "recording harness").
- `HostLayer` now owns `script: ScriptHost` (so teardown `stop_scripts` it) and tracks
  `physics_init: bool` (the C++ `physicsInit`) — set when the play edge builds the first world (05/12),
  read by teardown to `shutdown_physics()` once, after the last world drops. The build/tick play-edge
  wiring stays 05-physics / 12-scripting:phase-7's concern; this phase owns only the teardown.
- **Substrate added (noted in 05-physics:phase-3):** `saffron_physics::shutdown_physics()` — the safe
  crate had no public global-teardown pair to the implicit `World::new` init; the host needs it to shut
  the Jolt `Factory`/registered types down after the world. `World`'s own `Drop` already frees its
  bodies/ragdolls in the shim's destructor order, so the host never sequences Jolt destruction itself —
  it only drops the `Option<World>` then calls `shutdown_physics()`.
- The shm publisher's `Drop` is device-independent (`shm_drop_is_device_independent`): it munmaps +
  `shm_unlink`s with no GPU access, so it may drop any time relative to the renderer.
