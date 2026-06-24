# Quality tiers + half-resolution screen-space GI/AO

**Status:** CORE COMPLETE (tier system end-to-end); half-res + SSGI ray-count deferred
**Scope:** Both / Game-first (this *is* the graphics-settings menu; the editor just picks a cheap tier)
**Depends on:** — (self-contained; Phase 4 consumes its params)

> **Done — the tier system, end to end.** A `RenderQuality` struct + `QualityTier` enum
> (`rendering/src/quality.rs`: `low`/`medium`/`high`/`ultra`/`custom`) resolves to per-effect
> parameters the renderer applies via `Ssao::apply_quality` — the SSGI/contact step counts and the
> SSGI/GTAO/contact enable flags, all runtime push-constants (no shader recompile). The five binary
> GI toggle commands collapsed to **one knob**: `set-ssao`/`set-ssgi`/`set-contact-shadows` are
> deleted and replaced by `set-render-quality` + `get-render-quality` (protocol DTOs
> `SetRenderQualityParams`/`RenderQualityResult`, manifest regenerated, `sa` CLI auto-exposes them);
> the tier is saved with the project (`renderSettings.quality`, replacing the three bools), reported
> in `render-stats` (`quality` field), and the editor Render panel shows a Quality dropdown beside
> anti-aliasing (replacing the three checkboxes). Live-validated: `set-render-quality low` → SSGI/GTAO/
> contact all off in `render-stats`; `ultra` → all on; a bogus tier → typed error. `clustered`/`ibl`/
> `ddgi`/RT toggles stay as their own switches (architectural, not quality dials). Engine `cargo
> clippy --workspace` clean + unit tests (`quality.rs`, `render_quality_tier_applies_echoes_and_rejects_unknown`);
> editor `bun run check` (tsc) clean + oxlint 0 errors; docs page
> [render-quality-tiers](../../docs/content/explanations/screen-space-and-post/render-quality-tiers.md) added.
> The editor UI typechecks/lints but is **unverified live** — needs a `just run` to confirm the
> dropdown drives the viewport.
>
> **Deferred (documented), with rationale:**
> - **Half-resolution SSGI + GTAO with bilateral upsample** — the biggest single GPU cut (~0.9 ms),
>   but it needs half-res target allocation + an upsample shader pass (deep GPU/shader work, like
>   Phase 2's static/dynamic split). The tier already dials SSGI *step count* (the cheap runtime win);
>   half-res is the next, larger sub-step.
> - **SSGI ray count as a tier param** — the ray count is a compile-time loop bound in `ssgi.slang`
>   (not yet a push-constant), so dialing it needs a shader specialization-constant. Deferred with
>   half-res (same shader pass).
> - **Editor default tier** left at `high` (no surprise look change). Switching the editor viewport to
>   a cheaper default is a one-line policy change once the look is confirmed acceptable live.

## Goal

Replace the scattered hardcoded GI constants and the binary on/off control commands with a **named
quality-tier system** that resolves to one parameter struct, and run the two most expensive
screen-space passes (SSGI ~1.28 ms, GTAO+blur ~0.48 ms) at **half resolution** by default in the
editor. This is the canonical graphics-settings feature an exported game needs, and it gives the editor
a cheap interactive tier.

## Design

### One `RenderQuality` param struct, named tiers

Today the screen-space effects read hardcoded constants (`rendering/src/ssao.rs`: SSGI/GTAO ray-length
and step-count vec4s; contact shadow `0.2 length, 12 steps`; SSGI ray/step counts also in
`assets/shaders/ssgi.slang`) and the control commands `set-ssgi` / `set-ssao` /
`set-contact-shadows` / `set-ibl` / `set-clustered` (`control/src/commands_render.rs`) are `{0|1}`
toggles. Replace both with:

- A `RenderQuality` struct (home alongside `PerfConfig` in `rendering/src/frame_history.rs`, or a
  sibling) holding per-effect params: SSGI `{enabled, half_res, ray_count, march_steps}`, GTAO
  `{enabled, half_res, step_count}`, contact-shadow `{enabled, march_steps, length}`, TAA `{enabled}`,
  DDGI/ReSTIR budgets (consumed in Phase 4/Phase-6 controllers).
- Named tiers that expand to the struct: `EditorIdle`, `EditorInteractive`, `High`, `Play`, plus
  `Custom` exposing the individual knobs. The editor viewport defaults to `EditorInteractive` (cheaper
  than `Play`); the play edge / exported `saffron-player` requests `Play` (or the project's configured
  tier) through the Phase-1 named override stack so editing-quality GI never leaks into play and
  vice-versa.

Precedent: UE `sg.GlobalIlluminationQuality` / `sg.ReflectionQuality` (0..4 = Low/Medium/High/Epic/
Cinematic, each a named `.ini` block expanding to a bundle of `r.*` cvars; Low *disables* Lumen) +
Viewport Scalability running the editor below game; Unity HDRP per-override `Quality {Low/Medium/High/
Custom}` enums expanding to ray/step/resolution params.

### Half-resolution SSGI + GTAO

SSGI (the single most expensive pass) and GTAO run full-resolution today (`view_target.rs` allocates
full-res SSGI targets). Add a half-res path (1 ray / 4 pixels) gated by the tier's `half_res` flag:

- allocate half-res SSGI/GTAO targets when the flag is set,
- the existing `ssgi_blur.slang` is already a bilateral depth-aware denoise — extend it to a bilateral
  **upsample** back to full res,
- pair with the existing TAA so half-res noise resolves over frames.

Default the editor viewport to half-res + low ray/step; reserve full-res for a `High` tier or a
screenshot/offline-render path. Half-res GTAO is the shipped default in Godot (`use_half_resolution`;
SSAO half-res since PR #49738) and an option in Unity HDRP (SSAO/SSGI "Full Resolution" toggle); UE
Lumen integrates GI at `IntegrateDownsampleFactor=2` (half-res) — low risk.

### Wire ray/step counts into the shaders

SSGI ray count and march steps move from the hardcoded `ssgi.slang` literal and the `ssao.rs` vec4s
into push-constant / params-buffer fields fed from the `RenderQuality` struct, so a tier change takes
effect without a recompile.

## Control surface

Replace the five binary toggles with one `set-render-quality` command taking a tier name or a `Custom`
param set, plus `get-render-quality` (NO LEGACY — the `{0|1}` toggles are deleted, every caller and the
editor UI move to the tier command). Add the matching `sa` CLI subcommand. The editor's render-settings
UI switches from per-effect checkboxes to a tier selector + advanced (Custom) panel.

## Done when

- `sa set-render-quality EditorInteractive` measurably drops SSGI + GTAO cost (`pass-timings`) vs
  `High`; the editor defaults to the cheaper tier and the exported game runs `Play`.
- SSGI/GTAO run half-res on the default editor tier with bilateral upsample; no obvious quality cliff at
  rest (TAA resolves the noise).
- The hardcoded SSGI/GTAO/contact constants and the five binary GI toggle commands no longer exist;
  effects read `RenderQuality`.
- `just engine` + `just prepare-for-commit` clean; e2e tier test green; docs page for the quality-tier
  model added and the GI explanation pages updated to cite the tier params.
