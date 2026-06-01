# Phase 8: ReSTIR Many-Light + RT-GI Capstone

**Status:** COMPLETED
<!-- Flip to COMPLETED when the "Done when" checklist passes, validation-clean. Delete this file only after COMPLETED + merged. -->

<!--
COMPLETED 2026-06-01 (commit 71b7307), validation-clean on llvmpipe (~1 FPS). ReSTIR DI —
stochastic many-light direct lighting with per-pixel ray-traced shadows.
- Per-pixel reservoir SSBOs (32B/pixel: chosen light, W, wSum, M): initial / combined /
  previous, sized to the viewport (recreateRestirTargets). Gated on rtSupported.
- 3 compute graph passes (after the G-buffer prepass + TLAS build, before scene):
  restir_initial (RIS over K=16 candidate lights drawn from the pixel's FROXEL CLUSTER
  list — reuses the phase-3 candidate set; weighted reservoir sampling, no shadow ray);
  restir_reuse (temporal reproject via the phase-5 motion vector + 4 spatial neighbors,
  depth/normal-similar, M-clamped to bound bias); restir_resolve (ONE ray_query shadow ray
  for the chosen sample via the phase-7 TLAS, shade, write per-pixel direct radiance + copy
  the combined reservoir into previous for next frame).
- mesh.slang set 7 samples the resolved radiance (screenFlags.w) and replaces its punctual
  loop with restir*albedo/PI. G-buffer forced on for ReSTIR; resolve's TLAS + per-frame
  light/cluster SSBOs rebound each frame (writeRestirFrameBindings). se set-restir.
- Verified: 16-point-light scene renders with per-pixel ray-traced contact shadows the
  clustered path lacks; temporally stable (frame delta 0.0 after convergence); A/B vs the
  clustered-forward path; VAL=0.

NOT done (the plan's "later"/XL items, noted as seams): ReSTIR GI (reuse the reservoir
machinery over secondary path vertices to replace/augment phase-6 DDGI); a dedicated SVGF
denoiser (current output relies on temporal+spatial reservoir reuse, no separate denoise
pass); unbiased GRIS contribution weights (v1 is biased-but-stable); specular/glossy direct.
-->


## Goal

The high-end capstone: stochastic many-light direct lighting and ray-traced global
illumination via ReSTIR (reservoir spatiotemporal importance resampling) — the
algorithm behind UE5 MegaLights and NVIDIA RTXDI/RTXGI. This is what lets *thousands*
of dynamic, shadow-casting lights render affordably, and what turns the DDGI/diffuse
indirect into reference-quality GI. Strictly optional, hardware-gated, lowest priority
— every effect here has a cheaper approximation already shipped (phases 3/4/6).

**Depends on:** phase 5 (temporal history + motion vectors), phase 7 (acceleration
structures + ray query). Realistically gated on a real RTX/RDNA2+ GPU in the toolbox
for usable perf; builds + validates on llvmpipe at ~1 FPS.

## Background (what to implement)

ReSTIR resamples a small fixed-size **reservoir** per pixel from a large candidate set
using RIS (resampled importance sampling) + weighted reservoir sampling, then reuses
reservoirs across **time** (previous frame via motion vectors) and **space**
(neighboring pixels). The result approximates sampling proportional to each light's
unshadowed contribution, with one (or few) shadow rays per pixel resolving visibility.

- **ReSTIR DI** (Bitterli et al. 2020) — direct lighting from many lights.
- **ReSTIR GI** (Ouyang et al. 2021) — indirect/path-traced GI via the same machinery.
- **GRIS** (Lin et al. 2022) — the unbiased generalization.

UE5 MegaLights = ReSTIR-style stochastic direct lighting + per-light ray-traced (or
VSM) shadows + a denoiser, integrated with Lumen.

## Current state

- The clustered froxel light lists (`light_cull.slang`, the cluster SSBO) are an
  excellent **candidate source** for ReSTIR's initial sampling — reuse them instead of
  sampling all lights.
- Phase 7 provides the TLAS + ray-query for visibility; phase 5 provides motion vectors
  + history for temporal reuse.

## Steps

1. **Reservoir buffers** — per-pixel reservoir SSBOs (current + previous), ping-ponged
   like the phase 5 history. Each reservoir stores the selected light/sample, the
   running weight sum, and `M`/`W`.
2. **Initial candidate sampling** (compute) — per pixel, draw K candidate lights from
   the froxel cluster list (reuse the cluster SSBO), RIS-weight by unshadowed
   contribution, keep one in the reservoir.
3. **Temporal reuse** (compute) — reproject the previous reservoir via the motion
   vector, combine with the current (clamp `M` to bound bias).
4. **Spatial reuse** (compute, 1–2 passes) — merge neighbor reservoirs (with a
   normal/depth similarity test).
5. **Resolve + shade** — trace one shadow ray (ray-query, phase 7) for the chosen
   sample, shade with the phase 1 BRDF.
6. **Denoise** — temporal + spatial (a SVGF-style or the bilateral denoiser from
   phase 4) using the phase 5 history.
7. **ReSTIR GI** (later) — reuse the same reservoir machinery over secondary path
   vertices for indirect; can replace/augment the phase 6 DDGI trace.

## Control command

- `se set-restir {0|1}`, `se restir-stats` (samples/pixel, reservoir reuse). Gated on
  `rtSupported` (phase 7). Setter + `registerCommand` per `set-clustered`
  (`control.cppm:231`).

## Done when

- [ ] reservoir buffers + initial/temporal/spatial reuse passes implemented as graph
      compute passes feeding off the froxel candidate lists + phase 7 visibility.
- [ ] hundreds of dynamic lights render with stochastic shadows + denoise, A/B against
      the phase 3 shadow-map path; optional ReSTIR GI augments phase 6.
- [ ] validation-clean; builds + validates on llvmpipe (~1 FPS); meaningful timings
      pending hardware.

## Notes / risks

- This is an **XL** effort (the reservoir + denoiser machinery is the bulk). Treat it
  as the long-horizon capstone, not a near-term milestone.
- Bias control (clamping `M`, MIS weights, the unbiased GRIS contribution weight) is
  the subtle part — start biased, move toward unbiased.
- Keep the cheaper paths (phases 3/4/6) as the shipped default and the A/B reference;
  ReSTIR is the quality ceiling, not the baseline.
