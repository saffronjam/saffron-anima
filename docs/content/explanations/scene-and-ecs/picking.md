+++
title = 'Picking'
weight = 7
+++

# Picking

Picking maps a point on screen to the scene entity beneath it. A left-click in the viewport casts a
ray from the camera through the cursor, tests it against each entity's world-space mesh bounds, and
selects the nearest hit.

The goal is "which object", not a pixel-exact silhouette. A bounding-box test answers that question
cheaply and is robust enough for click selection. `pickEntity` lives in `Saffron.Assets` because it
needs the GPU mesh bounds the asset server caches.

## From click to ray

The click arrives as a point in normalized device coordinates, `[-1, 1]`, already matching the
rendered image. `pickEntity` rebuilds the same view-projection the renderer used — including the
Vulkan Y-flip that `cameraProjection` leaves out — and inverts it to unproject the click:

```cpp
glm::mat4 proj = cameraProjection(camera, aspect);
proj[1][1] *= -1.0f;  // match the renderer's clip space
const glm::mat4 invViewProj = glm::inverse(proj * camera.view);
const glm::vec4 nearH = invViewProj * glm::vec4(ndc.x, ndc.y, 0.0f, 1.0f);  // GLM 0..1 depth: near = 0
const glm::vec4 farH  = invViewProj * glm::vec4(ndc.x, ndc.y, 1.0f, 1.0f);
const glm::vec3 origin = glm::vec3(nearH) / nearH.w;
const glm::vec3 dir    = glm::normalize(glm::vec3(farH) / farH.w - origin);
```

Two clip-space points share the same xy: one on the near plane (depth 0, the engine's
`GLM_FORCE_DEPTH_ZERO_TO_ONE` convention) and one on the far plane (depth 1). They unproject to a
world-space origin and direction. Reusing the renderer's flip and depth range is what makes the ray
land where the pixel was drawn.

## World AABB per entity

`pickEntity` walks `forEach<TransformComponent, MeshComponent>`, resolves each mesh through
`loadMeshAsset` (skipping anything that fails to load), and builds a world-space AABB by
transforming the mesh's eight local-AABB corners and taking their min/max:

```cpp
const glm::vec3 lo = meshRef->boundsMin;
const glm::vec3 hi = meshRef->boundsMax;
for (u32 corner = 0; corner < 8; corner = corner + 1)
{
    glm::vec3 p = lo;
    if (corner & 1u) p.x = hi.x;
    if (corner & 2u) p.y = hi.y;
    if (corner & 4u) p.z = hi.z;
    const glm::vec3 world = glm::vec3(model * glm::vec4(p, 1.0f));
    worldMin = glm::min(worldMin, world);
    worldMax = glm::max(worldMax, world);
}
```

Transforming eight corners is cheap and keeps the slab test in one coordinate system. The resulting
box is axis-aligned in world space, so a rotated mesh gets a looser fit. That is acceptable for
click selection.

## The slab test

Each candidate runs the standard ray-AABB slab intersection using the precomputed `invDir`:

```cpp
const glm::vec3 t0 = (worldMin - origin) * invDir;
const glm::vec3 t1 = (worldMax - origin) * invDir;
const glm::vec3 tlo = glm::min(t0, t1);
const glm::vec3 thi = glm::max(t0, t1);
const f32 tEnter = glm::max(glm::max(tlo.x, tlo.y), tlo.z);
const f32 tExit  = glm::min(glm::min(thi.x, thi.y), thi.z);
if (tExit < 0.0f || tEnter > tExit) return;   // miss or box behind the ray
f32 t = tEnter;
if (t < 0.0f) t = tExit;                       // origin inside the box
```

`invDir` is `1.0f / dir`. An axis-aligned ray component produces an infinity there, which the
`min`/`max` handle correctly, so no special-casing is needed. The intersection distance `t` is
`tEnter`, or `tExit` when the camera is inside the box. The function keeps the smallest `t` across
all entities, so the nearest object wins. A miss everywhere returns `Entity{ entt::null }`, and the
caller clears the selection.

> [!TIP]
> Picking depends on the AABB matching the flipped projection. The renderer applies
> `proj[1][1] *= -1` for drawing; `pickEntity` repeats it. The un-flipped
> [`cameraProjection`](../transform-and-matrices/) exists so the gizmo is not mirrored, but
> picking must re-apply the flip or every click would land on the vertically-mirrored object.

## In the code

| What | File | Symbols |
|---|---|---|
| The pick | `assets.cppm` | `pickEntity` |
| Mesh local bounds | `geometry.cppm` | `boundsMin`, `boundsMax` |
| Mesh resolve | `assets.cppm` | `loadMeshAsset` |
| Matched projection | `scene.cppm` | `cameraProjection`, `CameraView` |

## Related
- [Transforms](../transform-and-matrices/) — the un-flipped projection picking re-flips
- [Selection](../../ui-and-editor/selection/) — what consumes the picked entity
- [Editor camera](../../ui-and-editor/editor-camera/) — the eye the pick ray shoots from
