# Phase 10 — motion vectors, TAA, FXAA, MSAA

**Status:** COMPLETED

**Depends on:** 06-rendering:phase-9-screen-space-gi

## Goal

Port the anti-aliasing modes and the motion-vector prepass they (and SSGI's temporal accumulation) need.
The three AA modes are mutually exclusive: MSAA (multisampled scene targets resolved into offscreen),
FXAA (scene → scratch, compute edge-blur → offscreen), and TAA (motion-vector reprojection + a compute
resolve with two ping-pong history images). This is where the per-view temporal state (`prevViewProj`,
`historyIndex`, `history[2]`) earns its place in `ViewTargets`.

## Why this shape (NO LEGACY)

- **`set_aa(msaa_samples, fxaa, taa)` enforces mutual exclusivity in one place** (`renderer_aa.cpp:67`):
  MSAA wins if `samples > 1`; the sample count is clamped to what the color+depth formats actually
  support (`clampSampleCount`, `:43`). Changing AA waits idle, recreates the MSAA/FXAA/TAA targets, and
  **clears the PSO cache** because the mesh + depth-prepass PSOs bake the sample count (`:94`). There is
  one AA selector, not three independent toggles that can contradict.
- **The motion-vector prepass (`motion`) is shared by TAA and SSGI** — it runs when `taa || do_ssgi`
  (`renderer.cppm:~1425`). It reprojects camera motion (and, with skinning, deformation motion via the
  prev-deformed buffer, phase 12) into `motion` (rg16f). The per-view `prevViewProj` (last frame's
  camera viewProj) drives camera reprojection; it is per-view so a re-activated view reprojects against
  its own last frame (`renderer_types.cppm:1318`).
- **TAA's two history images ping-pong by `historyIndex`; the resolve has two sets (one per parity)**
  (`renderer_types.cppm:1286`,`:1313`). The history is invalid on the first frame / after a resize
  (`historyValid`), which the resolve handles (history weight 0). SSGI's temporal history shares the same
  parity (phase 9). All of this is per-view state in `ViewTargets`.
- **The scene's 1x output target is selected by the AA mode, in the graph import logic** — offscreen
  normally, the FXAA/TAA scratch when those are on (then a compute pass resolves scratch → offscreen),
  the msaaColor + resolve into offscreen when MSAA (`renderer.cppm:1178`–`:1210`). One `sceneOutput`
  handle, branched by mode, exactly as the C++ imports it.
- **The MSAA resolve is the graph's `RgAttachment.resolve` (phase 2), not a hand-coded resolve** — color
  via `eAverage`, depth via `eSampleZero` (`render_graph.cppm:637`/`:659`). The graph already treats it
  as a second write of the matching kind.

## Grounding (real files/symbols)

- `engine-old/source/saffron/rendering/renderer_aa.cpp` — `setAa` (`:67`), `clampSampleCount` (`:43`),
  `setDepthPrepass` (`:57`), `recreateMsaaTargets`/`recreateFxaaTarget`/`recreateTaaTargets`.
- `engine-old/source/saffron/rendering/renderer.cppm` — the `motion`/`taa`/`fxaa` passes + the
  `sceneOutput`/`sceneColorAttachment`/`sceneDepth` import branching in `beginFrameGraph`
  (`:1178`–`:1210`, `:~1425`–`:2155`).
- `engine-old/source/saffron/rendering/renderer_types.cppm` — `ViewTargets` temporal fields
  (`motion`,`motionDepth`,`history[2]`,`historyIndex`,`historyValid`,`msaaColor`,`msaaDepth`,`scratch`,
  `prevViewProj`,`prevViewProjValid`, `:1284`–`:1320`), `Targets` AA caps/toggles (`sampleCount`,
  `maxSampleCount`, `supportedSampleCounts`, `fxaaEnabled`, `taaEnabled`, `:1339`–`:1344`),
  `taaSetLayout`/`fxaaSetLayout` (`Descriptors`).
- Shaders: `motion`, `taa`, `fxaa`.
- README §6.

## Acceptance gate

- `cargo build -p saffron-rendering` and the workspace build are green.
- `cargo test -p saffron-rendering` passes named tests:
  - `set_aa` mutual exclusivity: requesting MSAA + FXAA + TAA together yields MSAA only; `set_aa(0, true,
    true)` yields FXAA only; the PSO cache is cleared on the AA change.
  - `clamp_sample_count` returns the largest supported count ≤ requested, e1 when none.
  - the motion pass runs when TAA or SSGI is on, not otherwise; `prevViewProj` updates per frame per view.
  - TAA history invalidates on resize.
- **Golden-image / metric** tests: a static scene under TAA converges to a committed golden after K
  frames (history accumulates); MSAA 4× and FXAA each produce their committed golden on an edge-heavy
  scene. Validation log clean across all three modes + the MSAA resolve.
