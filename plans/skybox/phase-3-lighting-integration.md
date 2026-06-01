# Phase 3: Lighting Integration

## Goal

Connect scene environment to mesh lighting without doing full physically based IBL yet.

## Current Lighting Limitation

The mesh shader currently uses:

- one directional light,
- scalar ambient,
- punctual light list,
- clustered or non-clustered punctual loop.

The current ambient path is scalar:

```cpp
LightUbo.directionAmbient = glm::vec4(glm::normalize(direction), ambient);
```

Shader side:

```hlsl
float3 lit = directional + globals.directionAmbient.w;
```

This makes colored skylight impossible.

## Step 1: RGB Ambient

Change light globals to support colored ambient.

Suggested GPU layout:

```cpp
struct LightUbo
{
    glm::vec4 direction;        // xyz travel direction
    glm::vec4 colorIntensity;   // rgb color, a intensity
    glm::vec4 ambientIntensity; // rgb ambient color, a ambient intensity
    glm::uvec4 counts;          // x punctual count
};
```

Shader:

```hlsl
float3 ambient = globals.ambientIntensity.rgb * globals.ambientIntensity.a;
float3 lit = directional + ambient;
```

Public API:

```cpp
void setSceneLighting(
    Renderer& renderer,
    glm::vec3 direction,
    glm::vec3 color,
    f32 intensity,
    glm::vec3 ambientColor,
    f32 ambientIntensity,
    const std::vector<GpuLight>& lights);
```

Keep old overloads temporarily if useful.

## Step 2: Environment-Derived Ambient

In `renderScene`:

- Read first `DirectionalLightComponent` as today.
- Resolve `Scene.environment`.
- If `useSkyForAmbient`, use `environment.ambientColor * environment.ambientIntensity`.
- If not, use current light ambient fallback.

Migration:

- Existing `DirectionalLightComponent::ambient` can stay for now as legacy/direct ambient intensity.
- Longer term, move ambient off directional light and into `SceneEnvironment`.

## Step 3: Sky Texture Approximation

Before true irradiance convolution exists:

- Let user set `ambientColor` manually.
- Optionally sample average color CPU-side during import/cache later.

Do not sample the sky texture per fragment as ambient. It is noisy, expensive, and not physically correct.

## Step 4: Diffuse IBL

Later, generate low-order spherical harmonics or a small irradiance cubemap from the sky texture.

Options:

- CPU SH projection from decoded equirectangular pixels.
- GPU convolution into a cubemap.

First practical target:

```cpp
struct SkyIrradiance
{
    glm::vec4 sh[9];
};
```

Shader:

```hlsl
float3 irradiance = sampleSH(globals.skySh, normal);
```

This becomes the diffuse ambient term for lit materials.

## Step 5: Specular IBL

Only after material model has roughness/metalness:

- Convert equirectangular source to cubemap.
- Generate prefiltered mip chain by roughness.
- Generate BRDF LUT.
- Sample reflection vector by roughness in material shader.

## Implementation Steps

1. Expand light UBO to RGB ambient.
2. Update C++ buffer writes.
3. Update `mesh.slang`.
4. Update `DirectionalLightComponent` handling in `renderScene`.
5. Wire `SceneEnvironment` ambient controls.
6. Verify clustered lighting still works.
7. Add tests/screenshots for colored ambient.

## Verification

- Red/blue ambient visibly tints shadowed areas.
- Directional light behavior is otherwise unchanged.
- Point and spot lights still render in clustered and non-clustered modes.
- Existing projects preserve approximately the same look after migration.

