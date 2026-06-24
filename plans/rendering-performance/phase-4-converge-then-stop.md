# Converge-then-stop temporal GI

**Status:** NOT STARTED
**Scope:** Editor-mostly (needs a static view); the probe-budget machinery is Both
**Depends on:** Phase 1 (shared invalidation signal), Phase 3 (tier params)

## Goal

Make the screen-space and probe GI converge to a stable result and then **stop spending GPU** while the
view is static, instead of recomputing from scratch every frame. On a static scene the GI integrates
over a few frames into history, reaches a confidence cap, reports "converged," and the Phase-1 loop goes
quiet holding the converged shared-memory frame — avoiding both the always-on cost and the
mid-converge-freeze artifact a naive idle would cause.

## Design

### Accumulate-until-converged, reset on change-gen

Keep a temporal accumulation buffer + a per-pixel (or per-tile) sample/confidence counter for SSGI (and
the existing TAA history). Each static frame, integrate a few SSGI rays into history and increment the
counter; once it reaches an M-cap (e.g. ~20), **stop issuing new GI work** and sample the converged
history. On any invalidation — the **same** Phase-1 change-generation bump, never a second signal —
reset the counter to 0 so the GI re-integrates. Use the motion vectors Anima already produces (the
`motion` pass) to zero confidence only on **disoccluded** pixels, so a small camera nudge re-traces only
newly revealed pixels rather than the whole frame.

The per-effect `converged` flag is exported up to the Phase-1 named override stack as the
`gi-converging` reason: `needs_redraw` stays true until *all* temporal effects report converged, then
the loop idles. This is the coupling that makes idle correct — without it, idling mid-converge freezes a
noisy frame.

Precedent: Blender Cycles viewport samples (accumulate to a cap, then stop GPU work; adaptive noise
threshold early-out); Blender EEVEE jitter+accumulate with temporal reprojection hiding the reset; UE
Path Tracer (accumulate to samples-per-pixel then denoise; *any* camera/material/object change
invalidates to sample 0); ReSTIR/TAA practice (M-cap saturation + disocclusion confidence reset).

### DDGI / voxel-GI probe budget (Both)

For the world-space GI (`rendering/src/ddgi.rs` — rays buffer + trace set), add a per-frame **probe
budget**: refresh only a budgeted subset of probes per frame and spread updates across N frames, with a
frames-to-converge counter, so a static scene holds a converged probe field instead of re-tracing all
probes every frame. This benefits the exported game too (amortized GI cost under a moving camera, not
just the idle editor). Precedent: UE Lumen `RadianceCache.NumProbesToTraceBudget` +
`LumenScene.*.UpdateFactor`; Godot SDFGI Frames-To-Converge / Frames-To-Update-Light.

## Interaction with earlier phases

- The convergence reset and the redraw-skip share **one** signal (locked decision). Land Phase 1's
  change-gen counter first; route every "converged" flag and every dirty trigger through it.
- Tier params (Phase 3) set the M-cap and per-frame ray budget per tier (a `High` tier converges faster
  with more rays/frame; `EditorIdle` trickles).

## Done when

- A static scene shows SSGI/DDGI cost trending to ~0 as it converges (`pass-timings`), then the Phase-1
  loop idles holding the converged frame — with no visible noise freeze.
- Any edit or camera move resets accumulation (re-traces), visibly re-converging; a small camera nudge
  re-traces only disoccluded regions, not the whole frame.
- DDGI refreshes a bounded probe subset per frame and holds a converged field when static.
- `just engine` + `just prepare-for-commit` clean; e2e converge/reset test green (assert GI cost decays
  on a held view and spikes on an edit); docs GI pages updated to describe convergence + invalidation.
