# Phase 8 — IBL bake chain, visible sky, reflection probes, atmosphere

**Status:** COMPLETED

**Depends on:** 06-rendering:phase-7-lighting-and-shadows

## Goal

Port image-based lighting and the sky. The IBL bake produces an environment cube → a diffuse irradiance
cube + a roughness-mipped prefiltered specular cube + a split-sum BRDF LUT, sampled as the mesh ambient
(set 3). The visible sky is a fullscreen pass before the scene. Reflection probes capture + prefilter
local environments. The atmosphere path (Hillaire 2020 LUT chain) is an alternative environment source.
The bakes are editor-time events (waitIdle, off the per-frame hot path), re-armed when the sky inputs
change.

## Why this shape (NO LEGACY)

- **The IBL re-bake is deferred to a GPU-idle point (`begin_frame_graph` start), not per-frame.** A
  `rebake_pending` flag (set by `request_env_bake` when source/panorama/params change) triggers
  `bake_environment` at the next `begin_frame_graph`, so the visible sky + IBL relight together
  (`renderer.cppm:1117`). The bake itself waits idle — an editor event, not hot. This is one of the few
  places `wait_idle` is legitimate mid-session; it is isolated to the bake method.
- **Three environment sources behind one `EnvSource` enum + match** — Procedural (`ibl_skygen` from
  `SkygenParams`), Equirect (`ibl_equirect` projecting a user panorama, holding the `Arc<GpuTexture>`
  alive across the bake), Atmosphere (the `atmos_*` LUT chain into `atmos_skygen`)
  (`renderer_types.cppm:1348`). The C++ `EnvSource` enum becomes a Rust `enum`; the bake dispatches on it.
- **`SkygenParams`/`AtmosphereParams` are plain aggregates compared memberwise to gate the re-bake**
  (`renderer_types.cppm:1358`/`:1376`). The renderer does not import the scene; `submit_sky` /
  `request_env_bake` carry the resolved settings as POD, the same decoupling the C++ uses. In Rust these
  derive `PartialEq` so the "did the inputs change" check is `!=`, not a hand-written memberwise compare.
- **Reflection probes: a `MaxReflectionProbes = 8` array, captured on demand, seeded with the global IBL
  cubes so every slot binds validly** (`ReflectionProbes`, `renderer_types.cppm:1451`). A
  `ReflectionProbeUpload` POD per dirty probe (`:1387`) drives capture in `begin_frame_graph`
  (gated on the cull PSO existing). Overflow past 8 is logged once, not an error.
- **The sky pass owns the color clear; the scene pass loads instead of clearing** when the sky is visible
  (`renderer.cppm:~1992`). One sky pass, three modes (Color/Texture/Procedural) by the `Sky.mode` int —
  matching `SkyMode`'s values, carried as a plain int since the renderer does not import the scene.

## Grounding (real files/symbols)

- `engine-old/source/saffron/rendering/renderer_detail_ibl.cpp` — `bakeEnvironment`,
  `captureReflectionProbe`, the irradiance/prefilter/BRDF convolution chain, the atmosphere LUTs.
- `engine-old/source/saffron/rendering/renderer_types.cppm` — `Ibl` (`:1402`), `EnvSource` (`:1348`),
  `AtmosphereParams` (`:1358`), `SkygenParams` (`:1376`), `ReflectionProbe` (`:1430`), `ReflectionProbes`
  (`:1451`), `ReflectionProbeUpload` (`:1387`), `Sky` (`:1468`), `MaxReflectionProbes` (`:1425`).
- `engine-old/source/saffron/rendering/renderer.cppm` — the rebake/probe-capture block at the top of
  `beginFrameGraph` (`:1117`–`:1156`), the `sky` pass, `requestEnvBake` (`:2013`), `submitSky` (`:2004`),
  `setIbl` (`:2805`).
- Shaders: `ibl_skygen`, `ibl_equirect`, `ibl_irradiance`, `ibl_prefilter`, `ibl_brdf`,
  `atmos_transmittance`, `atmos_multiscatter`, `atmos_skyview`, `atmos_skygen`, `sky`.
- README §6 (the IBL/sky/probe row).

## Acceptance gate

- `cargo build -p saffron-rendering` and the workspace build are green.
- `cargo test -p saffron-rendering` passes named tests:
  - `SkygenParams` equality gates the re-bake: identical params → no re-bake armed; a changed sun
    direction → `rebake_pending` set.
  - the probe array seeds all 8 slots with the global IBL cubes at init (every slot binds validly before
    any capture).
  - `EnvSource` round-trips through the bake dispatch for all three variants.
- **Golden-image** tests: a procedurally-lit sphere with IBL on matches a committed golden; the
  atmosphere source produces a distinct, committed-golden sky; one reflection probe captures and the
  reflective surface near it matches its golden. Validation log clean (incl. the off-graph bakes).
