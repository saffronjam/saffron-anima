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

## Boot fix — editor/headless mode creates no present swapchain (2026-06)

The assembled `saffron-host` SIGSEGV'd on startup under headless weston + Mesa lavapipe, before the
control socket existed (caught by the e2e). The backtrace pinned it to lavapipe's WSI:

```
#0  0x0 in ?? ()
#1  wsi_create_native_image_mem ()            libvulkan_lvp.so
#2  wsi_create_image ()
#3  wsi_headless_surface_create_swapchain ()
#4  wsi_CreateSwapchainKHR ()
#7  ash …swapchain::Device::create_swapchain
#8  saffron_rendering::swapchain::Swapchain::new   swapchain.rs:88
#9  saffron_rendering::renderer::Renderer::new     renderer.rs
```

**Root cause:** `Renderer::new` unconditionally built a present `Swapchain` against the
`VK_EXT_headless_surface`, and lavapipe's headless-surface swapchain WSI is unimplemented — it null-derefs
in `wsi_create_native_image_mem` at *create* time. The C++ editor host created a real (hidden) SDL/Wayland
swapchain and simply never acquired/presented it in shm mode; the faithful Rust form (PP-10 "the window is
never presented") is to create **no** present swapchain at all in editor mode.

**Fix (one path, no legacy):**

- `Renderer.swapchain: Option<Swapchain>` — built only for `SurfaceSource::Window` (the standalone
  windowed host). `SurfaceSource::Offscreen` (editor + the smoke + every offscreen test) carries `None`.
  The present helpers (`render_frame`/`record_clear`/`submit_and_present`/`run_pending_window_capture`)
  run only when a swapchain exists; `render_frame` guards on it. The thumbnail target format now follows
  `device.surface_format` (it is read back over the control plane, never presented).
- The editor frame path renders **offscreen + publishes to shm**, never presenting: `FrameHost::begin_frame`
  waits the frame slot's fence (`Renderer::begin_offscreen_frame`, the C++ `beginFrame` fence-wait, run
  before the hooks so the per-frame skinning descriptor-pool reset is under an idle slot);
  `HostLayer::on_ui` runs `render_scene_offscreen` then `Renderer::read_active_view_bgra8` →
  `ViewportShmPublisher::publish`. The offscreen's resolved exit layout is tracked through a render-graph
  external slot so the BGRA8 read-back's entry barrier is correct.
- Editor-mode rendering correctness exposed by the now-live offscreen path was fixed alongside: the sky PSO
  rebuilds on an MSAA change (`Sky::set_sample_count`), the mesh's static ReSTIR set 7 binds whenever the
  RT-device layout has it, the directional/spot shadow maps are declared `SampledRead` on the scene pass
  so the graph derives the DepthWrite→ShaderReadOnly transition, and `clear_asset_caches` drops the
  editor-camera `SystemMeshVisual` (a cached GPU `Ref`) so it cannot outlive the allocator at teardown.
- Host bring-up now loads the project from the editor-set environment in `HostLayer::on_attach`
  (`SAFFRON_PROJECT` / `SAFFRON_AUTO_EMPTY_PROJECT` / a working-dir `project.json`), via the one
  project-bring-up path the lifecycle commands share (`ControlContext::bootstrap_project_from_env`), and
  the run loop feeds the frame delta into `render-stats` (`Renderer::observe_frame_delta`).

Verified under headless weston + lavapipe: the host boots without SIGSEGV, creates the control socket,
answers `ping`, publishes shm frames (header magic `SFV2`, `seq>0`), exits cleanly under
`SAFFRON_EXIT_AFTER_FRAMES`, with a validation-clean log. The windowed present path (`SurfaceSource::Window`)
is unchanged and still covered by `tests/swapchain_present.rs`.

## Device selection prefers the discrete GPU (2026-06)

On real hardware (Mesa llvmpipe + an NVIDIA RTX 3070 Ti via `VK_ADD_DRIVER_FILES`), `select_physical_device`
picked the software rasterizer: it returned the *first* qualifying device, and the loader listed llvmpipe
first. The C++ used `vk-bootstrap`, whose `PhysicalDeviceSelector` defaults to preferring a discrete GPU;
the hand-rolled Rust port dropped that preference. Two coupled fixes restored it (one path, no legacy):

- **Score, don't first-match.** `select_physical_device` now ranks every *qualifying* device by
  `DevicePreference` (`Discrete > Integrated > Virtual > Cpu`, the `vk-bootstrap` default order) and keeps
  the best, first-seen winning a tie. This is preference, never exclusion: when the only qualifying device
  is llvmpipe (the CI toolbox with no hardware ICD) it still scores `Cpu` and is selected, so the headless /
  software path the e2e + parity rig run on does not regress. Optional features (RT) still never gate.
- **Present support gates only the windowed host.** The discrete GPU was rejected with "no graphics+present
  queue family" because NVIDIA's queues reported *no* present support on Mesa's `VK_EXT_headless_surface` (a
  surface its ICD did not create). The offscreen host renders offscreen and reads back — it never presents —
  so `Device::new` passes `require_present = false` there and gates on a graphics queue only; the windowed
  host (`SurfaceSource::Window`, a real swapchain) still requires present. The offscreen path also takes the
  preferred `B8G8R8A8_UNORM`/sRGB format directly (used only for the read-back target) and reports no window
  capture, since it has no surface to query. (See the next section: the offscreen host now creates **no**
  surface at all, which is what unblocks the editor under the NVIDIA ICD.)

`Device::new` logs the chosen GPU + type (`vulkan ready — gpu '…' (discrete|cpu|…)`), and
`SAFFRON_VK_VERBOSE` traces each candidate's verdict (the diagnostic that pinned the silent rejection).
Proof, both bring-up paths, on the real machine:

- Headless (`SAFFRON_EDITOR_NATIVE_VIEWPORT=1`) **with** the NVIDIA ICD →
  `vulkan ready — gpu 'NVIDIA GeForce RTX 3070 Ti' (discrete)`, no "software rasterizer detected", exit 0,
  validation-clean. **Without** the ICD → `vulkan ready — gpu 'llvmpipe …' (cpu)` + "software rasterizer
  detected", three frames, exit 0.
- Windowed (no `SAFFRON_EDITOR_NATIVE_VIEWPORT`) **with** the ICD → the 3070 Ti + `3 swapchain images`;
  **without** → llvmpipe + `4 swapchain images`. Both open a window and present cleanly.

## Windowed present-only host shows the scene — offscreen → swapchain blit (2026-06)

The standalone windowed host (`just run-engine`, no `SAFFRON_EDITOR_NATIVE_VIEWPORT`) opened a window on
the GPU but stayed **blank**: `FrameHost::begin_frame` ran the old `render_frame` (acquire → *clear* →
present), while the host's `on_ui` rendered the scene into the per-view **offscreen** (the editor path).
Nothing carried that offscreen onto the acquired swapchain image — the window only ever showed the clear
color. The C++ standalone host blits the post-processed offscreen straight onto the swapchain image and
presents (`presentViewportToSwapchain`, `renderer.cppm:2355`); the Rust port now does the same.

**Fix (one path, no legacy — the clear+present `render_frame` is no longer on the windowed host path):**

- `present.rs` owns the windowed present path: a `PresentSync` ring (one blit command pool/buffer +
  a "scene-finished" semaphore + a present fence per in-flight slot, built only alongside the swapchain)
  and `record_present_blit` — the exact barrier sequence the C++ runs: offscreen
  (`COLOR_ATTACHMENT`/`SHADER_READ_ONLY` → `TRANSFER_SRC`), swapchain (`UNDEFINED` → `TRANSFER_DST`),
  `vkCmdBlitImage2` (RGBA16F → BGRA8, nearest), swapchain (`TRANSFER_DST` → `PRESENT_SRC_KHR`), sync2
  throughout.
- The frame splits across the loop: `FrameHost::begin_frame` (windowed) → `Renderer::begin_present_frame`
  waits the slot's prior present fence, **acquires** the swapchain image (the slot's image-available
  semaphore); the host's `on_ui` → `render_scene_offscreen` records + submits the scene into the offscreen
  and (in present mode) **signals** the slot's scene-finished semaphore; `FrameHost::end_frame` (windowed)
  → `Renderer::present_active_view_to_swapchain` records the blit, submits it **waiting** on both
  image-available (image owned) and scene-finished (scene rendered), signals render-finished, and presents.
  The editor / headless host (no swapchain) is unchanged — it still publishes the BGRA8 read-back to shm and
  never blits.
- Semaphore reuse is ordered by the slot's present fence (`begin_present_frame` waits it before re-acquiring
  into the slot's image-available semaphore and before the offscreen re-signals scene-finished), so
  `VUID-vkAcquireNextImageKHR-semaphore-01779` stays clean.

**Default content project for `run-engine`.** The editor-less `run-engine` recipe used to bootstrap an
empty project (no `SAFFRON_PROJECT`), so even with the blit the window showed an empty scene. The recipe now
points the engine at the repo-root `appdata` (the editor's project store) and resolves the most-recent
project **with mesh entities** (preferring `recent-projects.json` order), matching the C++ host opening the
last/default project rather than an empty one. A `SAFFRON_PROJECT` already in the environment always wins;
with no content project found it falls back to the engine's own env resolution. The engine's
`bootstrap_project_from_env` resolution itself is unchanged and still C++-faithful
(`SAFFRON_PROJECT` → `SAFFRON_AUTO_EMPTY_PROJECT` → working-dir `project.json`).

**Proof.** `present_blit_carries_a_non_blank_scene` (a renderer unit test, runs headless on llvmpipe) brings
up a full `Renderer`, renders a visible procedural sky into the offscreen, runs `record_present_blit` into a
BGRA8 image, reads it back, and asserts the result is **non-uniform** (the sky gradient → many distinct
colors; a uniform clear would be one) and validation-clean. The full windowed swapchain present path is
covered by `present_only_blit_shows_a_non_blank_scene` in `tests/swapchain_present.rs` (acquire →
offscreen-render → blit → present, then a window-capture read-back asserted non-blank), which runs on a real
Wayland surface (a display / the toolbox weston) — on the NVIDIA RTX 3070 Ti it brings up `3 swapchain
images`, captures the presented window, and is validation-clean.

## Editor offscreen host creates NO surface — boots under the NVIDIA-only ICD (2026-06)

The editor spawns the host with `VK_ICD_FILENAMES=<nvidia_icd>` (the editor's `lib.rs:335`), which
**replaces** the loader's ICD search rather than adding to it (vs `VK_ADD_DRIVER_FILES`). So only the
NVIDIA driver loads — and NVIDIA implements **no** `VK_EXT_headless_surface` (a Mesa-only extension). The
old offscreen path (`SurfaceSource::Headless`) requested `VK_EXT_headless_surface` in `create_instance` and
created a headless surface, so the host failed at bring-up with
`create_instance failed: ERROR_EXTENSION_NOT_PRESENT` — no renderer, no shm, the editor viewport waited
forever. The C++ engine works under this exact env because its editor mode created a surface from a real
(hidden) SDL **window**, using the platform `VK_KHR_*_surface` extension NVIDIA *does* support — never the
Mesa headless surface.

**Root cause:** the Rust offscreen host renders offscreen and reads back into shared memory — it has no
window and needs no surface at all. Requesting any surface extension under the NVIDIA-only ICD is what
broke it. (The boot fix above had already made the swapchain `None` in this mode; the surface itself was
the remaining unnecessary dependency.)

**Fix (one path, no legacy — `SurfaceSource::Headless` is gone, replaced, not kept alongside):**

- `SurfaceSource` now has two variants split on whether a surface exists: `Window` (the windowed host —
  `VK_KHR_surface` + the platform surface extension + a real surface + the present swapchain, unchanged)
  and **`Offscreen`** (the editor native-viewport host, the headless smoke, and every offscreen
  render-and-read-back test). `Offscreen` enables **no** surface instance extension, creates **no** surface
  object (`Device.surface` / `surface_loader` are `Option`, both `None`), and the logical device enables
  **no** `VK_KHR_swapchain` (which requires the instance-level `VK_KHR_surface`; enabling it without that
  trips `VUID-vkCreateDevice-ppEnabledExtensionNames-01387`). `require_present = surface.is_some()`, so the
  offscreen device gates on a graphics queue only and selects the discrete GPU; the offscreen render →
  read-back → shm-publish is otherwise byte-identical (the frozen BGRA8 seqlock ABI is unchanged).
- `record_present_blit` (the windowed-only offscreen→swapchain blit) takes the destination's final layout
  as a parameter: the windowed present passes `PRESENT_SRC_KHR`, and the headless content-correctness
  stand-in passes `TRANSFER_SRC_OPTIMAL` (it runs on the offscreen device, where `PRESENT_SRC_KHR` is now
  invalid without `VK_KHR_swapchain`). One function, no duplication; the real `PRESENT_SRC_KHR` handoff is
  still proven on a real surface in `tests/swapchain_present.rs`.

**Proof, the editor's exact env on the real machine** (`VK_ICD_FILENAMES=<nvidia_icd>` +
`SAFFRON_EDITOR_NATIVE_VIEWPORT=1`):

```
[saffron:host] viewport shm publish enabled
[saffron:rendering] vulkan ready — gpu 'NVIDIA GeForce RTX 3070 Ti' (discrete)
=== exit: 0 ===
```

No `create_instance` error, validation-clean, exits 0; the shm segment `/dev/shm/saffron-vp-scene` exists
with header `[magic=0x53465632 "SFV2", width=1600, height=900, seq=1, ring_slots=4, …]` (a real published
frame). The other two paths still work: **llvmpipe offscreen** (`VK_ADD_DRIVER_FILES` unset) →
`gpu 'llvmpipe …' (cpu)` + "software rasterizer detected", shm `SFV2`, seq advances, exit 0,
validation-clean; **windowed** (`SurfaceSource::Window`) unchanged — selects the GPU + opens the swapchain,
covered by `tests/swapchain_present.rs` on a real Wayland surface.
