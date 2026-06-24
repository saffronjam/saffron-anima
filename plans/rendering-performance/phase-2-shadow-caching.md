# Shadow caching: per-light dirty-keyed cube/CSM, static/dynamic caster split

**Status:** CORE COMPLETE (point-shadow cube cache); static/dynamic split deferred
**Scope:** Both (pure waste removed from editor *and* exported game)
**Depends on:** Phase 1 (the scene change-generation counter is the caster-moved signal)

> **Done — the measured win (point-shadow cube cache).** `render_scene` computes a camera-independent
> `point_shadow_content_key` (FNV over the light pos + far plane and every caster's world matrix +
> mesh id), threaded through `set_point_shadow` into `Lighting::point_shadow_key`. The renderer
> (`renderer.rs`) tracks `last_point_shadow_key` + `last_point_shadow_cube` and requests the
> `point-shadow` pipeline only when the key *or* the cube image handle changed — otherwise the 6-face
> pass is skipped and the persistent cube (held `SHADER_READ_ONLY`) is sampled as-is. So orbiting over
> a static light + casters costs ~0 (vs the measured 0.55 ms/frame); moving the light, a caster
> (animation/gizmo/physics), or adding/removing a mesh re-renders; a viewport resize (new cube handle)
> re-renders to seed the fresh cube. Unit-tested by
> `point_shadow_key_is_camera_independent_and_caster_sensitive`; build + clippy green; docs updated
> ([point-light-cube-shadows](../../docs/content/explanations/shadows-and-culling/point-light-cube-shadows.md)).
>
> **Deferred (documented), with rationale:**
> - **Directional CSM / contact-shadow caching** — *not applicable.* Cascaded directional shadows
>   re-fit to the camera frustum every frame (camera-*dependent*), and contact shadows are
>   screen-space; neither can reuse a cube across camera motion. The Phase-1 full-static idle is what
>   spares them, so a dirty-key cache buys nothing. (Spot-light shadows *are* camera-independent and
>   could take the exact same cache; low frequency, left as a trivial follow-on.)
> - **Static/dynamic caster split + `Mobility` component (#10)** and **partial cube-face update (#12)**
>   — genuinely large follow-ons (a new ECS component, two-layer shadow rendering + `min()` sampling;
>   per-face usage tagging). They optimize the *moving-character-over-static-environment* case the
>   point-shadow cache does not (a moving caster invalidates the whole cube). Deferred as their own
>   work; the point-shadow cache covers the common static-light/static-caster case today.

## Goal

Stop re-rendering shadow maps every frame for lights and geometry that did not move. Today a single
casting point light re-renders all six cube faces every frame (~0.55 ms measured) regardless of whether
the light or any caster moved. Cache the shadow result and re-render only on invalidation. This is pure
waste in every render path, so it ships in the exported game too.

## Design

### Per-light shadow dirty key

`lighting.rs:point_shadow_pending` is set unconditionally each frame whenever a casting point light
exists (`lighting.rs` ~line 553; armed again in `assets/src/render_scene.rs`). Replace that with a
**dirty key** per casting light:

```
key = hash(light.transform, light.radius/params, overlapping_caster_generation)
```

where `overlapping_caster_generation` derives from the Phase-1 change-gen counter, narrowed to casters
whose bounds intersect the light's influence sphere (a coarse broad-phase is enough — a moving object
far from the light should not invalidate it). When the key is unchanged since the last shadow render,
**skip `record_point_shadow`** (`scene_pass.rs:record_point_shadow`) entirely and keep sampling the
cached cube. `PointShadowTarget` (`scene_pass.rs`) is already a persistent image, so the cache storage
exists — only the arm/skip decision changes.

Generalize the same dirty-key pattern to:

- **directional CSM** (camera-frustum-fit cascades re-render on camera move *or* caster move in the
  cascade; a static camera + static casters → cached),
- **contact shadows** (screen-space, so they follow the redraw seam from Phase 1 rather than a separate
  cache — noted here for completeness, no per-light cache needed).

Precedent: Unity HDRP per-light `Update Mode: OnEnable` (render once, reuse) with
`cachedShadowTranslation/AngleUpdateThreshold` to avoid jitter re-renders; Blender EEVEE regenerates
shadow maps only when the light or geometry changed; UE virtual shadow maps cache pages across frames.

### Static / dynamic caster split (second step)

Once the dirty-key cache lands, decouple "a character walked" from "re-render the whole environment
cube": render **static** casters once into a cached static cube/CSM, re-render only the **dynamic**
layer when a dynamic object moves, and sample `min()` of the two depths. This needs a `Mobility`
component (`Static` / `Dynamic`) in `saffron-scene` so the renderer knows which casters belong to the
cached layer. Precedent: UE `r.Shadow.Virtual.Cache.StaticSeparate` (default since 5.4) +
per-primitive `ShadowCacheInvalidationBehavior`; Unity Mixed Lighting + Shadowmask bakes static-caster
shadows offline.

### Partial cube-face update (incremental, optional)

Even when a light *is* dirty, do not always render all six faces: track which faces are sampled by
on-screen pixels (a coarse depth-driven usage tag) and skip faces the camera cannot see this frame. The
per-face loop in `record_point_shadow` already iterates six independent face views, so per-face gating
is localized. Precedent: Blender EEVEE-Next usage tagging; UE virtual cubemaps render only sampled
faces/mips. Lower leverage than the cache — do it only after the cache and split land.

## Control surface

Extend the shadow/light readout so `sa` can report whether a light's shadow is cached vs re-rendering
this frame (feeds the Phase-5 observability story and an e2e assertion that a static light stops
re-rendering its cube).

## Done when

- A static point light over static geometry stops re-rendering its cube; `sa pass-timings` shows
  `point-shadow` drop to ~0 ms on idle frames; moving the light or an overlapping caster re-renders it
  the next frame.
- The unconditional `point_shadow_pending = true` re-arm is gone (NO LEGACY) — arming is dirty-keyed.
- Directional CSM caches on a static camera + static casters.
- `Mobility` component exists in `saffron-scene` and the static/dynamic split renders correct shadows
  for a moving object in front of static geometry.
- `just engine` + `just prepare-for-commit` clean; e2e shadow-cache test green; docs/shadow page updated
  to describe the caching model and `Mobility`.
