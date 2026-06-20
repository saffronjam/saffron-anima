# Phase 3 — the shm-ABI go/no-go gate: a Rust frame shown live in the unchanged reader

**Status:** COMPLETED

**Depends on:** 08-host-and-viewport:phase-2-shm-seqlock-publisher, 06-rendering:phase-16-capture-shm-profiler

**This phase is a GATE (PP-10 go/no-go).** It does not relax on failure; a failure escalates per the
feasibility study's spike sequence (the renderer/shm bring-up is one of the three gates the whole rewrite
is conditioned on).

## Goal

Prove the frozen frame transport end-to-end: a headless Rust host renders a validation-clean offscreen
frame, blits it to BGRA8, publishes it through the phase-2 seqlock segment, and the **unchanged**
`editor/src-tauri/src/wayland_viewport.rs` reader picks it up and presents it correctly on a Wayland
subsurface. This is the host half of the walking-skeleton milestone (boot headless → blank/then-real shm
frame the real editor shows → control `ping` answered, the last from 09-control-plane phase-1).

## Why this shape (NO LEGACY)

- **The reader is the oracle, not a stub.** The acceptance is byte-level agreement with the *actual*
  `wayland_viewport.rs` (`step_view`/`open_shm`/`stat_shm`), not a hand-written mirror. Two checks layer
  up: (1) an in-test reader replicating `step_view`'s exact field reads + slot index + accept/reject
  rules consumes the Rust producer's segment headlessly; (2) the real editor (or a headless harness
  importing the unchanged `wayland_viewport.rs` module) opens the segment and a presented-frame counter
  advances. The frozen ABI means the editor needs zero changes — proving that is the gate.
- **Headless device + present-only path, no swapchain.** The frame is produced via 06-rendering's
  present-only branch (`begin_frame` takes the `active_shm_publish().enabled` path, the swapchain is
  never acquired/presented) and the recorded `record_shm_publish_copy` readback into the per-frame VMA
  staging buffer, then the phase-2 host memcpy + seqlock. The whole chain runs under the Vulkan
  validation layer with zero errors — a missing barrier or wrong layout in the readback path is the kind
  of silent failure this gate exists to catch.
- **seq monotonicity + slot rotation are observable.** Over N frames the reader sees seq increase
  monotonically, slots rotate `seq % 4`, and the displayed dimensions track `set-viewport-size`. The
  `next = seq+1` first-frame-in-slot-1 detail is exercised, not just unit-asserted.
- **Two-process boundary kept.** Per feasibility 4.5 the engine stays a separate process from the editor;
  the gate runs the Rust host as a child (the editor spawn contract) or as a standalone process the
  reader attaches to. No in-process collapse.

## Grounding (real files/symbols)

- `editor/src-tauri/src/wayland_viewport.rs`: `step_view` (the full read/accept/attach logic),
  `open_shm`/`stat_shm` (segment open + remap probe), `SHM_MAGIC`/`SHM_HEADER_BYTES`, `View::from_wire`
  (`"scene"`/`"assetPreview"` tokens) — unchanged; the gate proves it accepts the Rust producer.
- `editor/src-tauri/src/lib.rs`: `spawn_engine` (the env contract: `SAFFRON_EDITOR_NATIVE_VIEWPORT=1`,
  `SAFFRON_CONTROL_SOCK`, `SAFFRON_VIEWPORT_SHM_SCENE=viewport_shm_name("scene")`,
  `SAFFRON_VIEWPORT_SHM_ASSET=viewport_shm_name("assetPreview")`, `SAFFRON_MAX_FPS=500`).
- `engine-old/source/saffron/rendering/renderer.cppm`: `setPresentViewportOnly` (line 2318), the
  `beginFrame` publish branch (962-972), `recordShmPublishCopy` (2389-2449); 06-rendering phase-16 owns
  the Rust equivalents this gate consumes.
- `engine-old/source/saffron/host/host.cppm`: `setPresentViewportOnly(app.renderer, true)` (1037), the
  two `enableViewportShmPublish` calls (1045/1050), the `setViewportDesiredSize` publish-mode handling
  (1536-1539) — the host wiring this gate exercises.

## Acceptance gate

- Cargo workspace compiles; `cargo clippy`/`cargo fmt --check` clean across `saffron-host` +
  `saffron-rendering`.
- A headless integration test / e2e harness step (in the toolbox under headless weston) runs the Rust
  host with `SAFFRON_EDITOR_NATIVE_VIEWPORT=1` + `SAFFRON_VIEWPORT_SHM_SCENE=<name>` +
  `SAFFRON_EXIT_AFTER_FRAMES=N` and asserts:
  - **Validation-clean**: the Vulkan validation layer logs zero errors across the offscreen render +
    readback + publish chain.
  - **In-test oracle agreement**: a reader replicating `wayland_viewport.rs::step_view` accepts every
    published frame (magic matches, `pixel_bytes <= capacity`, ring fits `total`), reads consistent
    w/h + pixels after the seq advances, and observes seq monotonic over N frames with slot = `seq % 4`.
  - **Both segments exist from startup**: even when only the scene view renders, the asset-preview
    segment exists with seq 0 (the presenter's blocking open would otherwise stall) — `stat_shm` finds
    both names.
- **The frozen-wire parity check (the gate)**: the unchanged `wayland_viewport.rs` reader (the editor,
  or a harness linking the module verbatim) displays the Rust-published frame — a presented-frame
  counter advances and the displayed dimensions match the rendered size. Run on a Wayland session
  (headless weston in-toolbox); GPU-hardware-only goldens are out of scope here (this gate is about the
  transport, not pixel goldens).
- The walking-skeleton assertion: with 09-control-plane phase-1 landed, the same booted host also
  answers a control `ping` over `SAFFRON_CONTROL_SOCK` — boot + blank/real shm frame + ping in one run.
