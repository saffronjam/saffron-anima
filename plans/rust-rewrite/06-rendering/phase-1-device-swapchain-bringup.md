# Phase 1 — ash device + swapchain bring-up: a validation-clean clear+present

**Status:** COMPLETED

**Depends on:** 00-foundations:phase-1-workspace-scaffold, 00-foundations:phase-2-core-crate, 00-foundations:phase-5-window-crate (saffron-window)

## Goal

Stand up `saffron-rendering` against `ash`: create the Vulkan instance (with validation in debug),
hand-roll the device + swapchain selection (`vk-bootstrap` has no Rust equivalent), create the VMA
allocator, and reach a **validation-clean** loop that acquires a swapchain image, clears it, and
presents. This is the foundation every later phase builds on; it proves the ash seam, the no-exceptions
`Result` mapping, and the feature-probe chain.

This phase ports the surface-bound bring-up. The headless variant (no surface, select device by feature)
is wired by `08-host-and-viewport` (PP-10) — the device-selection code here is written so that path is a
parameter, not a fork.

## Why this shape (NO LEGACY)

- **ash, not vulkano/wgpu.** ash sits at the exact abstraction level of Vulkan-Hpp `NO_EXCEPTIONS`: raw
  handles, explicit everything, `Result<T, vk::Result>` matching the C++ `checked(...)` seam one-to-one.
  vulkano auto-syncs (fights the hand-derived barrier graph in phase 2); wgpu hides the bindless/RT/
  timeline control the engine is built on. This is decided in the feasibility study and not re-litigated.
- **The C++ `checked(...)`→`Result<T>` pattern becomes `?` over a typed `Error`.** Every ash call returns
  `VkResult` (or `Result<T, vk::Result>`); the crate wraps it with a `thiserror` `Error::Vk(vk::Result)`
  variant and propagates with `?`. There is no separate check-then-propagate step — `?` *is* the check.
- **Hand-rolled device/swapchain selection, branch-for-branch from `vk-bootstrap` usage.** No maintained
  Rust `vk-bootstrap` exists; the ~150-LOC feature-probe / degradation chain (`enable_*_if_present` for
  RT, ray-query, descriptor-indexing, dynamic-rendering, sync2, timeline semaphores, calibrated
  timestamps, debug-utils) is ported directly off how `newRenderer` builds `VulkanContext`. The probe
  results land in the `Device` sub-state's capability flags (`rt_supported`, `fill_mode_non_solid`, the
  resolved PFN tables) — exactly the C++ `VulkanContext` fields.
- **`#![allow(unsafe_code)]` at the crate root with a one-line justification naming the ash seam.** This
  is one of the three FFI crates; every other crate denies unsafe. The unsafe is wrapped in safe methods
  (`Device::new`, `Swapchain::new`) so callers never touch raw handles.
- **The allocator is a thin wrapper over whichever crate PP-2 pins** (`vk-mem-rs` or `gpu-allocator`);
  the create/map/destroy call sites are identical for both, so this phase does not hard-depend on the pick.

## Grounding (real files/symbols)

- `engine-old/source/saffron/rendering/renderer.cppm` — `newRenderer` (`:127`) builds `VulkanContext`
  (instance → surface → physical device → device → queue → allocator), the feature probes, and the
  initial swapchain; `destroyRenderer` (`:595`) the teardown order.
- `engine-old/source/saffron/rendering/renderer_types.cppm` — `VulkanContext` (`:1036`), `Swapchain`
  (`:1055`), `FrameSync`/`FrameData` (`:1068`/`:85`), `RtDispatch` (`:1019`),
  `CalibratedTimestampsDispatch` (`:1030`), `RgDebugLabels` (in `render_graph.cppm:168`), the format
  constants `DepthFormat`/`OffscreenColorFormat` (`:49`/`:54`), `MaxFramesInFlight` (`:60`).
- `engine-old/source/saffron/rendering/AGENTS.md` — the `checked(...)`/no-`vk::raii` rule.
- README §2 (the `Device` immutable-after-init sub-state) and §4 (Drop / teardown order).

## Acceptance gate

- `cargo build -p saffron-rendering` and the full workspace build are green.
- A headless smoke (`SAFFRON_EXIT_AFTER_FRAMES`-style, driven from a `#[test]` or the e2e harness on a
  weston backend) runs N frames of acquire→clear→present with **zero Vulkan validation-layer messages**
  (the validation-clean gate; the log is asserted clean).
- `cargo test -p saffron-rendering` passes a unit test that the feature-probe chain degrades correctly
  on a device lacking the RT extensions: `rt_supported == false` is handled without error, the device is
  still created, and the clear+present still runs. (Note this is *not* "any software device" — the
  toolbox's Mesa lavapipe advertises the acceleration-structure + ray-query extensions and probes
  `rt_supported == true`; the degraded path is exercised against a device that genuinely lacks them.)
- `cargo clippy -p saffron-rendering` clean; the crate root carries `#![allow(unsafe_code)]` + the seam
  justification, and every `unsafe` block is inside a safe `Device`/`Swapchain` method.
