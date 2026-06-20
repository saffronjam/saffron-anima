# 08 — Host and viewport (the integration apex)

The host is where every subsystem is wired into a running process: it owns the run loop, holds the
`SceneEditContext` / `AssetServer` / `AnimationRuntime` / `ScriptHost` / optional `PhysicsWorld`, serves
the control plane once per frame, and publishes rendered frames into POSIX shared memory for the Tauri
editor to present. There is no engine UI — the host renders the scene plus a native gizmo overlay into
an offscreen image and the editor composites its React UI over the live viewport on a Wayland
subsurface. PP-10 is one of the three go/no-go gates: a Rust producer must publish frames the
**unchanged** `editor/src-tauri/src/wayland_viewport.rs` reader displays correctly.

This area ports `saffron-app` (the run loop + the `Layer` model) and `saffron-host` (the apex binary)
together, because the host *is* a single `Layer` plus the lifecycle closures (`onCreate`/`onExit`) that
`run` invokes. It also owns the renderer-side shm publisher's host wiring — co-designed with
06-rendering, which records the GPU blit/copy/readback; this area owns the mmap/seqlock/fence and the
byte-exact ABI.

The C++ ground truth is three small modules plus the renderer capture TU: `app.cppm` (191 LOC, the run
loop + `Layer`/`App`/`AppConfig`), `window.cppm` (116 LOC, the SDL window + typed signals — replaced by
`saffron-window` from 00-foundations, see below), `host.cppm` (1615 LOC: ~900 lines of CPU overlay
geometry, the `HostState` lifecycle wiring, the ~12 closures that capture the shared host state), and
`renderer_capture.cpp` (the shm publisher). The shm reader oracle is `wayland_viewport.rs`.

## 1. Crate shape and ownership

`saffron-app` is a thin lib: the `Layer` trait, an `App` struct, an `AppConfig`, and `run`. `saffron-host`
is the apex `bin` that depends on every subsystem. Per the PP-1 foundations contract these are two
crates; `saffron-window` already exists (00-foundations) and replaces `window.cppm` wholesale — the host
consumes it, this area does not re-port it.

### 1.1 The `Layer` trait (PP-1 locked)

The C++ `Layer` is a struct of six optional `std::function` closures (`onAttach`, `onUpdate(TimeSpan)`,
`onRender`, `onUi`, `onRenderGraph(RenderGraph&)`, `onDetach`). PP-1 locks this to **`trait Layer` with
provided (default-empty) methods**, stored as `Box<dyn Layer>` because the layer set is open and
client-extensible. The host implements exactly the hooks it uses (`update`, `ui`); the empties are free.

```rust
pub trait Layer {
    fn name(&self) -> &str { "Layer" }
    fn on_attach(&mut self, _app: &mut App) {}
    fn on_update(&mut self, _app: &mut App, _dt: TimeSpan) {}
    fn on_render(&mut self, _app: &mut App) {}
    fn on_ui(&mut self, _app: &mut App) {}
    fn on_render_graph(&mut self, _app: &mut App, _graph: &mut RenderGraph) {}
    fn on_detach(&mut self, _app: &mut App) {}
}
```

The C++ closures capture `App&` and `state` (the `shared_ptr<HostState>`) by value; in Rust the hook
takes `&mut App` so the layer never aliases the app it runs inside, and the host's per-frame state lives
on the host `Layer` impl itself (no `shared_ptr` capture). The one wrinkle the borrow checker forces:
the C++ hooks freely touch `app.window` and `app.renderer` while iterating `app.layers`. The Rust `run`
loop cannot hand a `Layer` `&mut App` while `App` owns the `Vec<Box<dyn Layer>>` it is iterating. The
locked resolution (§5) is that `run` *moves* the layer vec out for the duration of a hook pass (or holds
window/renderer in `App` and the layers in a sibling field iterated by index with the layers
temporarily `mem::take`-n), so a hook borrows window+renderer mutably without aliasing the layer list.

### 1.2 `HostState` → the host `Layer` struct

`HostState` is a `shared_ptr` captured by 12 closures in C++ precisely because closures cannot otherwise
share mutable state. In Rust that disappears: the host is **one** `struct HostLayer` owning its state
by value (`editor: SceneEditContext`, `assets: AssetServer`, `animation: AnimationRuntime`,
`script: ScriptHost`, `physics: Option<PhysicsWorld>`, the cursors/flags), and the lifecycle is methods
on it. No `Arc`, no `Rc<RefCell>` for the host state itself — it is single-threaded and owned by the
run loop.

The C++ `newSceneEditContext()`/`destroySceneEditContext()` heap-ownership dance (to keep the heavy
entt/json destructor out of the host TU) is **deleted** — Rust has no TU-bloat reason and 03-ecs-and-scene
already removed it (`Drop` is automatic). The two `onPlayStateChanged.subscribe(...)` lifecycle hooks
(script VM build/destroy; physics world build/drop) and their `unsubscribe` in teardown port to
`saffron-signal` subscriptions held as `SubscriptionId` fields on `HostLayer`, dropped in `on_detach`.

The one genuine shared-mutable site is the **thumbnail worker thread** (06-rendering): it shares the GPU
queue + bindless table with the render thread. That is the `Arc<Mutex>` the C++ `gpuQueueMutex()`/
`bindlessMutex()` mark, and it lives in 06-rendering, not here. The host only `start_thumbnail_worker` /
`drain_thumbnail_completions` / `stop_thumbnail_worker` around it (teardown order in §6).

### 1.3 The overlay state (`Rc<RefCell>` per PP-1 bucket 4)

The native gizmo overlay builds per-frame CPU geometry (`Vec<OverlayVertex>`) and submits it to the
renderer. It is single-threaded, frame-scoped, and built freshly each frame — so it needs no shared
ownership at all: it is local `Vec`s in the `on_ui` body, handed to `submit_overlay` by move. PP-1's
`Rc<RefCell>` "host per-frame overlay/gizmo state" bucket applies only if a builder needs to mutate
shared accumulators across helper calls; the C++ passes `std::vector<OverlayVertex>&` by reference into
each builder, which ports directly to `&mut Vec<OverlayVertex>` arguments — no `Rc<RefCell>` needed. We
record this as a measured *non*-use of the bucket.

## 2. The run loop (`run(config)`)

`app.cppm::run` is the locked sequence; it ports faithfully into idiomatic Rust:

1. Create the window (or, in editor mode, skip the window — §3). On failure `return Err`.
2. Create the renderer (`new_renderer(&window)` or headless `new_renderer_headless()` — §3).
3. `config.on_create(&mut app)` — the host attaches its `Layer`, wires signals, enables shm publish,
   sets default AA, registers the one host-owned control command (`get-script-schema`, needs the Lua
   schema reader, see 09/12).
4. `on_attach` for each layer.
5. Loop while `running`:
   - `poll_events(&mut window)` (editor mode: a no-op winit pump or nothing — §3).
   - compute `dt` from `Instant::now()`.
   - `on_update(dt)` for each layer.
   - if not minimized and `begin_frame(&mut renderer)`: `on_render`, `on_ui`, `begin_frame_graph`,
     `on_render_graph(graph)` for each layer, `end_frame`.
   - `frame_count += 1`; honor `SAFFRON_EXIT_AFTER_FRAMES` (the headless verification gate).
   - honor `SAFFRON_MAX_FPS` (sleep-until pacing, catch-up without debt — the editor sets 500).
6. `wait_gpu_idle(&renderer)` **before any teardown** (so no in-flight command buffer references a
   resource about to be dropped). This is the single most load-bearing teardown ordering fact.
7. `on_detach` for each layer; `config.on_exit(&mut app)`.
8. Optional `SAFFRON_CAPTURE` PPM/PNG dump (`capture_viewport`).
9. Drop the renderer, then the window (encoded by `App` field order + `Drop` — §6).

`SAFFRON_EXIT_AFTER_FRAMES` and `SAFFRON_MAX_FPS` parsing ports the `from_chars`-strict semantics
(reject trailing garbage, `MAX_FPS==0` → ignore) to `str::parse::<u64>()` with the same reject-on-error
behavior, logged via `saffron-core` log functions.

## 3. Headless instance in editor mode

The decisive feasibility finding (4.5): in editor mode the SDL window is created **hidden**
(`SAFFRON_EDITOR_NATIVE_VIEWPORT` set → `WindowConfig.hidden`), never presented, and the surface is
load-bearing only for present-capable device selection. The Rust port goes further: in editor mode it
creates **no window and a headless Vulkan instance** (select device by feature, not surface), because the
frame path is shm-publish (offscreen → BGRA8 → staging → memcpy → ring), and the swapchain is never
acquired or presented (`begin_frame` takes the `activeShmPublish(...).enabled` branch — see
`renderer.cppm:962`). A real `winit` window + surface-bound device exists only for the standalone
present-only host (no `SAFFRON_EDITOR_NATIVE_VIEWPORT`, no shm env vars).

The mode is decided at `run` from the same env vars the C++ reads:
- `SAFFRON_EDITOR_NATIVE_VIEWPORT` present → editor/headless mode (no window).
- `SAFFRON_VIEWPORT_SHM_SCENE` / `SAFFRON_VIEWPORT_SHM_ASSET` non-empty → enable that view's shm publish.
- neither set → standalone present-only host with a winit window (developer convenience / smoke).

This makes the device-selection code from 06-rendering phase-1 a *parameter* (surface-bound vs headless),
not a fork — that phase already wrote it that way. `saffron-window` (winit) is only instantiated in the
standalone path. The editor path needs no window crate at all, only ash's headless instance and
`ash-window` is unused there.

## 4. The shm seqlock publisher — the FROZEN ABI (the gate)

The byte layout is **frozen** by `wayland_viewport.rs` and `renderer_capture.cpp`. Reproduced exactly:

- **Magic** `0x5346_5632` ("SFV2"), header **32 bytes** = eight `u32` little-endian fields:
  `[magic, width, height, seq, ring_slots, slot_capacity, 0, 0]`.
- **Ring** of `ring_slots = 4` (`MaxFramesInFlight`) fixed-capacity slots after the header. Frame with
  sequence `s` lands in slot `(s) % ring_slots` — note the C++ computes `next = seq + 1` then writes slot
  `next % ring_slots`, so the **first published frame (seq 1) lands in slot 1, not slot 0**. The reader
  reads `seq % slots`. This off-by-one-vs-zero detail must be reproduced exactly.
- **Slot capacity** floored at `MinShmSlotCapacity = 3840*2160*4` (4K RGBA) so ordinary resizes never
  reallocate; grow-only (recreate the segment only when a frame outgrows the slot).
- **Pixel format** BGRA8 (`VK_FORMAT_B8G8R8A8_UNORM`), which the reader maps to
  `wl_shm::Format::Xrgb8888` (little-endian XRGB = byte order B,G,R,X). The renderer's blit produces
  exactly this; the host only memcpys it.
- **Seqlock publish order** (`renderer_capture.cpp:291-303`): write the slot pixels (memcpy from the
  invalidated VMA staging buffer), then `header[1]=width`, `header[2]=height`, **then** a
  `fence(Release)`, **then** `header[3]=next` (seq) last. The reader reads `magic` + `seq`, copies, and
  trusts that a new `seq` implies the matching w/h + pixels are already visible. In Rust: write pixels
  and w/h with plain stores, `std::sync::atomic::fence(Ordering::Release)`, then store seq — the release
  fence orders the prior non-atomic writes before the seq store the reader observes.
- **`seq = 0` means "no frame yet"**: the segment is created at startup (both views) with seq 0 so the
  presenter's blocking open succeeds for a not-yet-rendered view, but the reader shows nothing until the
  first real publish. This startup-create-both-segments behavior is load-bearing (the presenter blocks on
  each segment's existence — `renderer_capture.cpp:198-207`).

The host owns: `shm_open(O_CREAT|O_RDWR, 0600)` + `ftruncate` + `mmap(MAP_SHARED)` via **rustix** (PP-2:
rustix over nix), the `shm_unlink` on recreate and teardown, and the 32-byte header init. The renderer
side (06-rendering phase-16) records the GPU blit + copy + the host barrier and does the memcpy +
seqlock bump inside `publish_shm_publish_slot`; the exact division (mmap/fence here, blit/copy there)
mirrors the C++ split between `renderer_capture.cpp` (publish) and `renderer.cppm::recordShmPublishCopy`
(record). Per the foundations contract `saffron-host` is one of the three `#![allow(unsafe_code)]`
crates with a justification naming the shm seam; the raw-pointer header writes + mmap are the only unsafe
here. Validation against the unchanged reader is the **gate** (phase-3).

## 5. The native gizmo overlay port (~900 LOC of CPU geometry)

`host.cppm` carries the whole overlay geometry builder set in one TU because it touches `OverlayVertex`/
`submitOverlay`/`Renderer`. It ports to a `mod overlay` in `saffron-host`:

- `OverlayVertex` (`renderer_types.cppm:993`) is a `#[repr(C)]` + `bytemuck::Pod` GPU struct: `position:
  Vec2` (NDC), `color: Vec4`, `edge: Vec4`, `depth: f32`. It lives in 06-rendering (the renderer's
  vertex format); the host imports it. A const size assert mirrors the C++ layout.
- The 2D primitive builders (`addTriangle`/`addLine`/`addQuad`/`addBox`/`addRectOutline`/`addCircleFill`/
  `addCircleOutline`/`addBulbIcon`/`addCameraIcon`) take `&mut Vec<OverlayVertex>` + glam types — a
  direct port; `pixelToNdc` (a renderer helper) maps pixel coords to clip space.
- The world-space builders (`addClippedOverlayLine`/`addWorldAabb`/`addWorldRing`/`addWorldArc`/
  `addWorldOrientedBox`) take a `viewProjection: Mat4` and clip lines to the near plane.
- The composite builders read the `SceneEditContext`: `buildNativeGizmo` (the active gizmo handles),
  `buildSceneEditCameraFrustums`, `buildSceneEditBillboards`, `buildDebugOverlays` (bounds/light
  volumes/whole-scene AABB), `buildColliderOverlays` (reads authored `ColliderComponent`, draws in Edit
  AND Play), `buildSkeletonOverlay` (draws in every play state). The hit-test/projection/drag *math*
  already lives in `saffron-sceneedit` (03-ecs-and-scene phase-11); these builders only consume it to
  emit geometry — they are pure geometry, no edit logic.
- `submit_scene_edit_overlay` assembles two ranges — `depth_tested` (frustums, debug overlays,
  colliders) and `on_top` (billboards, gizmo, skeleton) — and calls `submit_overlay(renderer,
  depth_tested, on_top)`. `editChrome` gates the Edit-only chrome (hidden in Play and in the asset
  preview); colliders + skeleton sit outside it with their own preview guards. This branching ports verbatim.

This is mechanical (~900 LOC of float math + push), low-risk, and pure CPU. It is split into its own
phase so the gate-critical shm/run-loop phases land first and the overlay grows the running spine after.

## 6. Teardown order (`Drop` graph — the UAF surface)

Teardown order is a *runtime* UAF if wrong, not a compile error (feasibility §6). The C++ order, encoded:

1. **`wait_gpu_idle` first** (`app.cppm:209`, in `run`, before `on_detach`/`on_exit`): no command buffer
   may still reference a resource about to be freed.
2. `on_exit` (`host.cppm:1574`): `stop_thumbnail_worker` (the worker borrows the renderer; it MUST be
   drained+joined before `wait_gpu_idle`/`destroy_renderer` — in Rust the worker join happens in the
   host's `on_detach`/drop, sequenced before the renderer drops); then drop the control context (closes
   the socket); then tear down the play state (stop scripts → drop physics world → unsubscribe the two
   play-state hooks → drop the editor context); then shutdown the Jolt globals **after** the last world
   is gone; then **clear the GPU `Ref` caches** (`meshRefByUuid`/`textureRefByUuid`) so cached
   meshes/textures drop **before** `destroy_renderer` frees the device/allocator.
3. The renderer drops, then the window — by `App` field order: in Rust, `struct App { renderer:
   Renderer, window: Option<Window> }` with `renderer` declared first drops first; or an explicit
   `Drop` sequence. The renderer's own `Drop` frees views/resources before the VMA allocator before the
   device (06-rendering phase-3 owns that internal order).

The Rust win: most of this is automatic via field-order `Drop`, but the **cross-object** facts —
worker-join before `wait_gpu_idle`, `wait_gpu_idle` before any resource drop, GPU `Ref` caches dropped
before `destroy_renderer`, Jolt globals shutdown after the last world — are explicit sequencing the host
encodes in `on_detach`/`run`, exactly as the C++ comments demand. This is the host's load-bearing
contribution to safety and it is the reason the host ports **last** (it needs all 13 subsystems live).

## 7. Control-socket wiring

The host calls `poll_control` once per frame in `on_update` (after the parent-death watch, before
animation/tick). 09-control-plane owns the socket server (rustix, `accept4`/`recv(MSG_DONTWAIT)`/
`send(MSG_NOSIGNAL)` flush loop), the registry, and the 153 handlers. The host owns: constructing the
`EngineContext` borrow struct each frame from its own fields (`window`, `renderer`, `sceneEdit`,
`assets`, `physics: Option<&mut>`), calling `poll_control` with it, and registering the one host-owned
command (`get-script-schema`). The C++ `EngineContext { Window&, Renderer&, SceneEditContext&,
AssetServer&, PhysicsWorld* }` ports to a borrow struct of disjoint `&mut` fields assembled in
`poll_control` and never stored — exactly the 09 design. The socket path comes from
`SAFFRON_CONTROL_SOCK` (the editor sets it).

## 8. The parent-death watch

The editor spawns the host as a child and the host watches for the editor vanishing: `editorSpawned =
SAFFRON_EDITOR_NATIVE_VIEWPORT set`; capture `editorPid = getppid()` once; each `on_update`, if
`getppid() != editorPid` the parent reparented us away (died) → set `window.shouldClose = true` (exit).
Ports to `rustix::process::{getppid, Pid}` with the same once-captured-then-compared logic. Escape key →
`shouldClose` (the `onKeyPressed` subscription) survives only in the standalone windowed path; in
editor/headless mode there is no window to take key events, so the watch + control `quit` are the exit
paths.

## 9. Self-tests deleted

`host.cppm:1313-1323` calls ten startup self-tests (`runSceneSerializationSelfTest`,
`runSceneHierarchySelfTest`, `runPlayModeSelfTest`, `runGeometrySelfTest`, `runContainerMetadataSelfTest`,
`runCatalogLinkageSelfTest`, `runBakeModelSelfTest`, `runChunkLoaderSelfTest`, `runInstantiateSelfTest`,
`runExtractSelfTest`). Per the locked ground rules these are **deleted from the host** — each is already
re-expressed as `#[cfg(test)]` units in the crate that owns the subsystem (02-geometry, 03-scene). The
host has **no** startup self-test call; the walking-skeleton + wire-driven e2e are the host's verification.

## 10. The walking-skeleton milestone

The feasibility study and PP-14 place a walking-skeleton milestone right after ECS/scene: the engine
boots headless, publishes a blank shm frame the real editor displays, and answers a control `ping`. This
area's phase-1 (run loop) + phase-2 (shm publisher) + phase-3 (gate) **are** that milestone's host half.
Phase-3 is the PP-10 go/no-go gate: a Rust producer's frames shown live in the unchanged
`wayland_viewport.rs`. Later phases (overlay, full lifecycle wiring) extend the living spine.

## Grounding (real files/symbols)

| What | File | Symbols |
|------|------|---------|
| Run loop, `Layer`, `App`, `AppConfig`, env-frame-limit/max-fps | `engine-old/source/saffron/app/app.cppm` | `run`, `Layer`, `App`, `AppConfig`, `attachLayer`, `frameLimitFromEnv`, `maxFpsFromEnv`, `waitGpuIdle` call site |
| SDL window + typed signals (replaced by `saffron-window`) | `engine-old/source/saffron/window/window.cppm` | `Window`, `WindowConfig`, `newWindow`, `destroyWindow`, `pollEvents`, `onClose`/`onResize`/`onKeyPressed`/`eventSinks` |
| Host apex: lifecycle, state, closures | `engine-old/source/saffron/host/host.cppm` | `runHost`, `HostState`, `config.onCreate`/`onUpdate`/`onUi`/`onExit`, `scriptSubscription`/`physicsSubscription`, `simTick` |
| Native overlay geometry (~900 LOC) | `engine-old/source/saffron/host/host.cppm` | `buildNativeGizmo`, `submitSceneEditOverlay`, `addLine`/`addQuad`/`addWorldAabb`/…, `buildColliderOverlays`, `buildSkeletonOverlay`, `BillboardKind` |
| shm publisher (host side of the seam) | `engine-old/source/saffron/rendering/renderer_capture.cpp` | `recreateShmSegment`, `enableViewportShmPublish`, `publishShmPublishSlot`, `destroyShmPublish`, `ShmMagic`/`ShmHeaderBytes`/`ShmRingSlots`/`MinShmSlotCapacity` |
| shm record + present-only branch (renderer side) | `engine-old/source/saffron/rendering/renderer.cppm` | `recordShmPublishCopy`, `setPresentViewportOnly`, `beginFrame` shm branch (`activeShmPublish`), `endFrame`, `presentViewportToSwapchain` |
| shm types | `engine-old/source/saffron/rendering/renderer_types.cppm` | `ShmPublish`, `ShmPublishSlot`, `OverlayVertex`, `OverlayState`, `activeShmPublish`, `activeView` |
| shm reader oracle (FROZEN) | `editor/src-tauri/src/wayland_viewport.rs` | `SHM_MAGIC`, `SHM_HEADER_BYTES`, `step_view`, `open_shm`, `stat_shm`, `View::{wire,from_wire}` |
| editor spawn contract (env vars, FROZEN) | `editor/src-tauri/src/lib.rs` | `spawn_engine`, `SAFFRON_EDITOR_NATIVE_VIEWPORT`/`SAFFRON_CONTROL_SOCK`/`SAFFRON_VIEWPORT_SHM_SCENE`/`SAFFRON_VIEWPORT_SHM_ASSET`/`SAFFRON_MAX_FPS`, `viewport_shm_name` |
| control socket + EngineContext + per-frame poll | `engine-old/source/saffron/control/command.cppm`, `control_server.cpp` | `EngineContext`, `pollControl`, `controlSocketPath`, `startControlServer` (`socket`/`bind`/`listen(8)`) |
