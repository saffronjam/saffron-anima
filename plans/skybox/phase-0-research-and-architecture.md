# Phase 0: Research And Architecture

## Goal

Choose the correct engine-level abstraction before implementation starts. This phase should end with agreement on data ownership, renderer API shape, editor UX, and how much control the sky system exposes in the first release.

## Decision

Represent the default sky as `SceneEnvironment` state on `Scene`.

Do not implement the main skybox as a normal entity with `MeshComponent` and `MaterialComponent`.

## Why Scene Environment Wins

The sky is global for a rendered view. It usually has no world position, should not be picked, should not appear in normal hierarchy operations, should not participate in depth prepass, and should not affect mesh batching statistics.

It also feeds lighting. Unreal's Sky Light and HDRI workflows treat sky as an environment lighting source, not just visible geometry. Frostbite's sky work is even more integrated with physically based lighting, atmosphere, clouds, and time-of-day. Saffron should leave room for that now.

The current renderer already has scene-wide frame state:

- `Renderer::clearColor`
- `Renderer::sceneDrawList`
- per-frame light UBOs
- render graph passes for scene, depth prepass, FXAA, tonemap, and UI

The sky should join that same path as per-frame scene environment state.

## Alternatives Considered

### Giant Unlit Mesh Entity

Pros:

- Minimal new renderer work.
- Can use existing mesh/material path.
- Artists can see it in hierarchy.

Cons:

- Wrong selection/picking semantics.
- Pollutes scene draw stats and batching.
- Needs special culling, transform, depth, and scale rules.
- Does not naturally feed ambient/reflection lighting.
- Competes with depth prepass and normal opaque ordering.
- Harder to support equirectangular textures and procedural atmosphere.

Verdict: useful only as a temporary visual prototype or as a future explicit backdrop mesh feature.

### Renderer-Only Global Setting

Pros:

- Simple renderer implementation.
- No scene serialization needed.

Cons:

- Projects cannot save sky settings.
- Editor cannot treat the sky as part of scene authoring.
- Runtime scenes cannot swap environments cleanly.

Verdict: renderer should own GPU resources and per-frame state, but source-of-truth settings belong to `Scene`.

### Entity Component For Sky

Pros:

- Reuses component registry, inspector, serialization, and hierarchy.
- Can support multiple authorable sky actors like Unreal's Sky Atmosphere actor.

Cons:

- Multiplicity rules are awkward: what happens with two sky entities?
- Most skybox controls are global rather than transform-driven.
- Still needs scene-level resolution rules.

Verdict: defer this until there is a real need for atmospheric volumes, cloud layers, or local reflection probes.

## Target Control Surface

First release controls:

- Mode: `Color`, `Texture`.
- Clear/background color.
- Sky texture asset.
- Sky rotation/yaw.
- Sky intensity.
- Visibility toggle.
- Use sky for ambient toggle.
- Ambient color and intensity.

Later controls:

- HDR import and exposure.
- Cubemap source vs equirectangular source.
- Diffuse irradiance intensity.
- Reflection intensity.
- Sun disk toggle.
- Procedural atmosphere parameters.
- Cloud layers.
- Time of day.

## Acceptance Criteria

- The design does not require sky to be represented as a mesh entity.
- Project files can persist environment settings.
- Renderer can render sky independently from normal mesh draws.
- Mesh shader can receive an ambient color derived from the environment.
- Future IBL and atmosphere work does not require replacing the public scene model.

