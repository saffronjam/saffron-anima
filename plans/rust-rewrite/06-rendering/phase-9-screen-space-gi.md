# Phase 9 — thin G-buffer + GTAO, contact shadows, and SSGI

**Status:** COMPLETED

**Depends on:** 06-rendering:phase-8-ibl-sky-probes

## Goal

Port the screen-space effects that ride on a thin G-buffer (view normal + view-Z): GTAO ambient
occlusion (+ bilateral denoise), directional contact shadows, and one-bounce SSGI (+ bilateral denoise +
temporal accumulation + the prev-color history copy). The mesh fragment multiplies AO into the ambient
term and adds the gathered SSGI radiance, sampled via set 4. These are per-view, viewport-sized targets
in `ViewTargets`.

## Why this shape (NO LEGACY)

- **One thin G-buffer prepass feeds all three effects** (`gbuffer` pass; `Ssao` sub-state,
  `renderer_types.cppm:1488`). It writes view normal (rgb) + view-Z (.a) into `gNormal`; the effects
  share it + the scene's view/proj. The prepass runs when any of GTAO / contact / SSGI / ReSTIR is on
  (`has_gbuffer` flag, `renderer.cppm:~1452`).
- **The per-view sets that bind these images live in `ViewTargets`, not in `Ssao`** — `Ssao` owns the
  device-shared layouts/samplers + the camera transforms; the SETS (`gtaoSet`, `aoBlurSet`,
  `contactSet`, `ssgiSet`, `ssgiBlurSet`, `ssgiAccumSets`, `copyColorSet`, `meshSet`) are per-view so a
  view switch never binds another view's images (`renderer_types.cppm:1302`). This is the README §2
  per-view borrow split applied to compute sets.
- **The compute passes declare `StorageImageRWCompute` / `SampledReadCompute` usages; the graph derives
  every GENERAL ↔ ShaderReadOnly transition.** No hand barriers — the chain
  gbuffer→gtao→ao-blur→contact→ssgi→ssgi-blur→ssgi-accum is a sequence of declared accesses
  (`renderer.cppm:~1452`–`:1660`). Two shared compute layouts (2-binding sampler+storage, 3-binding
  sampler+sampler+storage) back the lot (`Ssao.compute2Layout`/`compute3Layout`).
- **SSGI temporal accumulation + the prev-color history copy are real passes, ping-ponged by
  `historyIndex`.** `ssgi-accum` blends denoised SSGI with the previous frame via motion (so it depends
  on the motion pass, phase 10 — when motion is off, SSGI reads the denoised result directly). The
  `ssgi-history` / `ssgi-history-restore` copy captures linear-HDR scene color into `prevColor` before
  tonemap turns it display-referred (`renderer.cppm:~2167`). Per-view history validity (`historyValid`)
  resets on resize.
- **Each effect is one toggle (`useSsao`/`useContact`/`useSsgi`), no duplicate variants.** The bool gates
  the pass set; the `ready` flag means sets/views are built after targets exist.

## Grounding (real files/symbols)

- `engine-old/source/saffron/rendering/renderer.cppm` — the `gbuffer`/`gtao`/`ao-blur`/`contact`/`ssgi`/
  `ssgi-blur`/`ssgi-accum`/`ssgi-history` passes in `beginFrameGraph` (`:~1452`–`:2200`),
  `setSsaoCamera` (`:3054`), `setSsao`/`setContactShadows`/`setSsgi` (`:2815`/`:2825`/`:2835`).
- `engine-old/source/saffron/rendering/renderer_types.cppm` — `Ssao` (`:1488`, layouts/samplers/camera +
  the toggles), the `ViewTargets` screen-space images (`gNormal`,`gDepth`,`aoRaw`,`aoMap`,`contactMap`,
  `ssgiMap`,`ssgiDenoised`,`ssgiResolved`,`prevColor`,`ssgiHistory[2]`) + their per-view sets (`:1273`–
  `:1316`), `FrameGraphState` (`hasAo`/`hasContact`/`hasSsgi`/`hasGbuffer`, `:1702`).
- Shaders: `gbuffer`, `gtao`, `ao_blur`, `contact`, `ssgi`, `ssgi_blur`, `ssgi_accum`, `copy_color`.
- README §2 (per-view sets), §6.

## Acceptance gate

- `cargo build -p saffron-rendering` and the workspace build are green.
- `cargo test -p saffron-rendering` passes named tests:
  - the G-buffer prepass runs only when an effect needs it (`has_gbuffer` matches the toggle set).
  - switching the active view rebinds the per-view sets (the bound image views match the new view's
    targets) — no cross-view aliasing.
  - SSGI history validity resets on a view resize.
- **Golden-image** tests: GTAO on a creased mesh darkens the contact crevices to a committed golden;
  contact shadows produce their golden; SSGI produces a one-bounce color bleed golden. Validation log
  clean across the whole screen-space chain.

## Post-integration fix (e2e exposed)

Wiring the live scene pass through the e2e (`toggles.test.ts`) exposed a **descriptor set-4 unbound**
error (`VUID-vkCmdDrawIndexed-None-08600`) whenever the screen-space prepass did not run that frame
(the default off state, or toggling SSGI/SSAO back off). The übershader's pipeline layout always
declares set 4 (AO/contact/SSGI samplers) + set 5 (DDGI), so both must be bound on every scene draw —
the C++ binds `activeView().meshSet` + `ddgi.meshSet` unconditionally, gating the reads in-shader.
The Rust `add_screen_space_passes` returned `mesh_set = null` on its early-return (no prepass this
frame), leaving set 4 unbound. Fixed: the early return now binds the per-view set 4 whenever the
view's screen-space sets are built (`screen_space_ready()`) — the always-allocated set written to the
neutral init-transitioned maps. Verified: a headless SSGI/DDGI/SSAO toggle cycle is validation-clean.
