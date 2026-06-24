# Surface detail & screen effects

**Status:** PENDING IDEA

> Inspiration backlog — not yet implementable as written. Needs a codebase pass (post passes after
> tonemap, a forward decal pass, and a "Decal"/weather domain for the material node-graph).

Small, high-polish items with no new primitives — the fastest way to make the image read as
"finished." Lens/post FX are genuine gaps; decals reuse the material node-graph + Jolt raycast.

> **Architecture note:** the renderer is **forward+**, so there is no G-buffer — **DBuffer decals are
> not available**. Decals must be a bespoke forward screen-space or clipped-mesh pass. SSR is
> **deprioritized/skipped** — it is redundant with the existing ReSTIR + RT reflections + SSGI; only
> worth it as a low-end no-RT fallback.

## What it is

Runtime decals (bullet holes, blood, scorch), lens/camera post effects, surface wetness/snow, and
weather precipitation.

- **UE5:** deferred/mesh decals + Post Process Volumes + Niagara-driven weather.
- **Unity:** the Decal Projector + post-processing stack.

## Core technique

- **Decals:** project a texture onto surfaces — either unproject from depth in a forward screen-space
  pass, or clip a decal mesh to the receiver. Placement uses Jolt raycast (hit point + normal).
- **Lens/post FX:** **bloom** (bright-pass + dual-Kawase or a mip pyramid downsample/upsample), plus
  **chromatic aberration**, **vignette**, and **film grain** — cheap full-screen passes *after* tonemap.
- **Lens-flare ghosts:** sample bright spots and splat scaled/offset ghosts along the optical axis.
- **Surface wetness/snow:** a global weather uniform drives material node types that darken/roughen
  (wet) or blend a snow layer by world-up — no particles needed.
- **Precipitation:** rain/snow are GPU particle effects with splash sub-emitters.

## Build size

- **S–M** lens/post FX (bloom, CA, vignette, film grain) — **highest polish per effort; genuine gaps.**
- **M** runtime projected decals (forward screen-space) / **L** clipped mesh decals.
- **S–M** surface wetness/snow material nodes — **cheapest weather payoff, no particles.**
- **M** screen-space lens-flare ghosts.
- **XL** weather precipitation — gated on the GPU particle system.

## Dependencies (do these first)

- **Lens/post FX, decals, wetness/snow need nothing new** — pure render-graph passes + node types.
- **Precipitation depends on [gpu-particle-vfx](gpu-particle-vfx.md).**
- *Decals on moving entities* want **scene-graph parenting**.

## What we reuse / what's missing

**Reuse:** the render graph (post passes), tonemap (passes slot after it), the material node-graph (a
"Decal" domain + weather nodes), Jolt raycast (decal placement), and TAA (keeps screen-space effects
stable).

**Missing:** the post-FX passes themselves, a forward decal pass (forward+ forbids DBuffer), and the
GPU particle system for precipitation.

## Notes & references

- "Next Generation Post Processing in Call of Duty: Advanced Warfare" (Jimenez) — bloom/DoF references.
- Dual-Kawase blur for cheap bloom; UE5 Post Process Volume docs for the effect set.
- UE5/Unity decal docs — note the forward+ caveat above (no DBuffer for us).
