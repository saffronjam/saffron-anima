# Converge-then-stop temporal GI

**Status:** CORE COMPLETE (loop-side convergence); per-pixel disocclusion + DDGI budget deferred
**Scope:** Editor-mostly (needs a static view); the probe-budget machinery is Both
**Depends on:** Phase 1 (shared invalidation signal), Phase 3 (tier params)

> **Done — converge-then-stop at the loop level.** The `RedrawController` (Phase 1) now renders a
> **convergence window** after activity, not just a wall-clock keep-warm: the host reports whether a
> temporal effect is accumulating (`set_temporal_active` = TAA on or SSGI on), and while it is the
> loop keeps rendering for `CONVERGE_FRAMES` (24) frames after the last invalidation so the TAA/SSGI
> history settles to its final image before the viewport idles **on the converged frame** (never a
> noisy mid-accumulation one). The reset is the *same* Phase-1 invalidation (camera/scene/command),
> so there is one signal, not two (the locked decision). A `converged()` accessor is exported for the
> Phase-5 stats readout. With no temporal effect on, convergence is immediate (the keep-warm alone
> applies). Unit-tested (`temporal_convergence_renders_a_frame_window_past_keep_warm`); build + clippy
> clean; docs (the [main-loop reactive-pacing](../../docs/content/explanations/app-lifecycle-and-window/main-loop-and-run.md)
> section) updated.
>
> **Why this is the right core:** because Phase 1 stops rendering entirely once converged, the SSGI/GI
> passes already *stop issuing work* on a static scene at the frame level — the "stop tracing" goal is
> met by the loop, no per-pixel shader gate needed. What remains is intra-frame optimization for the
> *moving* case:
>
> **Deferred (documented), with rationale:**
> - **Per-pixel disocclusion reset via motion vectors** — re-trace only newly-revealed pixels when the
>   camera nudges, instead of the whole frame. A deep SSGI-shader change (confidence buffer + motion-
>   vector reprojection); reduces cost *during motion*, which the loop-level convergence does not. The
>   larger sub-step, like Phase 2's static/dynamic split and Phase 3's half-res.
> - **DDGI / voxel-GI per-frame probe budget (#13)** — spread probe updates across frames. DDGI is off
>   by default (`ddgi: false`), so this is low-priority; deferred with the disocclusion work (shares
>   the convergence machinery).

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
