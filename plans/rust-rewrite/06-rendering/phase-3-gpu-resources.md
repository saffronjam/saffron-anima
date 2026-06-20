# Phase 3 — GPU resource wrappers (Drop), the Device sub-state, and teardown order

**Status:** COMPLETED

**Depends on:** 06-rendering:phase-1-device-swapchain-bringup

## Goal

Port the move-only RAII GPU resource wrappers — `Image`, `Image3D`, `Buffer`, `GpuMesh`, `GpuTexture`,
`Pipeline`, `AccelerationStructure` — as `impl Drop` types, and lock the `Device` immutable-after-init
sub-state (README §2) plus the teardown order (README §4). This is the ownership backbone every later
phase allocates against. Getting the Drop order right here is what prevents the teardown
use-after-free that the C++ `destroyRenderer` ordering and `waitGpuIdle` carefully avoid.

## Why this shape (NO LEGACY)

- **The hand-written dtor + deleted-copy + defaulted-move boilerplate evaporates.** Each C++ wrapper
  (`renderer_types.cppm:96–544`) is ~40 lines of move-ctor / move-assign / dtor / `reset()`. In Rust the
  type is move-only by default and a single `impl Drop` replaces all of it. The free logic in
  `reset()` (e.g. `vmaDestroyBuffer` then null the handle) becomes the `drop` body; move-out is the
  language's job.
- **`Device` is constructed once, then shared `&Device` (read-only) — never `&mut` after init.** It owns
  the `vk::Device`, the VMA allocator, the resolved PFN tables (`RtDispatch`, calibrated timestamps,
  debug-utils), and the capability flags. Because it is never re-borrowed mutably, many resources and
  passes can hold `&Device` (or an `Arc<Device>`) concurrently — the borrow split that makes the whole
  per-frame architecture legal. Resources hold the allocator/device handles they need to free
  themselves; the `Device` must outlive them (encoded by field order / Drop sequence).
- **The bindless free-list is `Arc<Mutex<Vec<u32>>>`, shared into every `GpuTexture`.** The C++
  `bindlessFreeList` is a `Ref<std::vector<u32>>` each `GpuTexture` holds so its dtor can push its slot
  back under `bindlessMutex()` even off the main thread (`renderer_types.cppm:348,400`). This is one of
  the two explicit shared-mutable sites (README §5): the texture's `Drop` locks the mutex and pushes its
  slot. So `GpuTexture` Drop is thread-safe by construction.
- **`AccelerationStructure` holds the resolved `destroy_accel` PFN, not a static link** — RT entry points
  are function pointers on the `Device` (`renderer_types.cppm:427`). The wrapper clones the fn pointer it
  needs at construction so its Drop is self-contained.
- **`waitGpuIdle` before teardown is a host-loop responsibility (PP-10), not baked into each Drop.** Drop
  frees handles; the guarantee that no handle is freed under a live GPU read comes from the run loop
  calling `wait_idle` before dropping the renderer. This phase asserts the order (device/allocator
  outlive resources) via field/Drop sequencing; the loop wiring is `08-host-and-viewport`.

## Grounding (real files/symbols)

- `engine-old/source/saffron/rendering/renderer_types.cppm`:
  - `Pipeline` (`:96`), `Image` (`:153`), `Image3D` (`:1515`), `Buffer` (`:486`), `GpuMesh` (`:228`,
    incl. vertex/index/skin buffers + CPU-side `cpuPositions`/`cpuIndices`/`cpuSkin` for picking + the
    `Ref<AccelerationStructure> blas`), `GpuTexture` (`:336`, incl. `bindlessIndex` + `bindlessFreeList`),
    `AccelerationStructure` (`:423`, handle + backing buffer + `destroyFn`).
  - The free order in each `reset()` (handle/view before allocation; allocator-then-device).
  - `gpuQueueMutex`/`bindlessMutex` (`:33`/`:42`) — the markers for the `Arc<Mutex>` sites.
- `engine-old/source/saffron/rendering/renderer.cppm` — `destroyRenderer` (`:595`) for the full teardown
  order; `waitGpuIdle` (`:3122`).
- README §4 (Drop order), §5 (the two `Arc<Mutex>` sites).

## Acceptance gate

- `cargo build -p saffron-rendering` and the workspace build are green.
- `cargo test -p saffron-rendering` passes named tests:
  - allocate + drop each wrapper type (`Buffer`, `Image`, `GpuMesh`, `GpuTexture`) and assert
    (via a VMA budget read before/after) that the allocation is fully reclaimed — no leak.
  - a `GpuTexture` dropped on a spawned thread returns its bindless slot to the shared free-list under
    the mutex (the slot reappears in the free list); proves the `Arc<Mutex>` Drop path is `Send`-safe.
  - constructing the full renderer and dropping it runs `wait_idle` then frees resources before the
    allocator/device with **zero validation messages** (the teardown-order gate).
- `cargo clippy -p saffron-rendering` clean; no `unsafe` outside the documented ash/VMA seam methods.
