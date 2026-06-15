+++
title = 'Picking'
weight = 7
+++

# Picking

Picking maps a point on screen to the scene entity beneath it. A left-click in the viewport casts a
ray from the camera through the cursor and selects the nearest entity its geometry actually
intersects. Each mesh is tested in two phases: a cheap world-AABB **broad phase** rejects meshes the
ray comes nowhere near, then a ray-vs-triangle **narrow phase** finds the true surface hit. The
narrow phase is what makes a click land on the silhouette and not on the empty air inside a loose
bounding box.

`pickEntity` lives in `Saffron.Assets` because it needs the cached meshes the asset server owns. It
covers both static `MeshComponent` and skinned `SkinnedMeshComponent` meshes, so rigged imports are
selectable too.

## From click to ray

The click arrives as a point in normalized device coordinates, `[-1, 1]`, already matching the
rendered image — y-down, like the flipped clip space the renderer draws with, so the top of the
viewport is `y = -1`. The `pick` command produces it from viewport UV (origin top-left) as
`(u*2-1, v*2-1)`. `pickEntity` rebuilds the same view-projection the renderer used — including the
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

## Broad phase: world AABB

For each candidate `pickEntity` builds a world-space AABB from the mesh's eight local-AABB corners
(`worldAabbFromCorners`) and runs the standard ray-AABB slab test (`rayAabbSlab`). Both live in
`Saffron.Geometry`; the same corner helper feeds the renderer's scene-bounds fit and the debug
overlay boxes, so there is one definition of "world AABB of a mesh".

```cpp
glm::vec3 worldMin{ FLT_MAX };
glm::vec3 worldMax{ -FLT_MAX };
worldAabbFromCorners(model, meshRef->boundsMin, meshRef->boundsMax, worldMin, worldMax);
f32 tEnter, tExit;
if (!rayAabbSlab(ray, worldMin, worldMax, tEnter, tExit)) return;  // ray misses the box → skip
```

The box is re-axis-aligned in world space, so a rotated mesh gets a fat, loose fit — a long diagonal
antenna inflates the box with empty air. The broad phase only *rejects*; it never decides the hit, so
that looseness costs nothing but a few extra narrow-phase tests.

## Narrow phase: ray vs. triangle

A mesh whose AABB the ray crosses is tested triangle by triangle. The mesh keeps a CPU copy of its
positions and indices (`GpuMesh::cpuPositions` / `cpuIndices`, filled once at upload), so picking
never reads back from the GPU. Each triangle's three vertices are transformed to world space and run
through a two-sided Möller–Trumbore test (`rayTriangle`):

```cpp
for (triangle in cpuIndices)
{
    f32 t;
    if (rayTriangle(ray, v0, v1, v2, t) && t < best) best = t;  // nearest forward surface
}
```

`rayTriangle` is winding-agnostic (a back face hit counts) and only accepts hits in front of the
origin, so a ray starting inside a mesh reports the surface ahead, not one behind it. The nearest
triangle hit across all meshes wins; a miss everywhere returns `Entity{ entt::null }` and the caller
clears the selection.

## Skinned meshes

A second pass handles `SkinnedMeshComponent`. Its vertices are deformed on the GPU, so picking
reproduces the same skin on the CPU: it builds the joint palette with `jointMatrices`
(`worldMatrix(bone) * inverseBind`) and blends each vertex, `Σ wₖ · (palette[jointₖ] · restPos)`,
exactly as `skin.slang` does. Because the joints already place the vertices in world space, the
deformed positions feed `rayTriangle` directly with no model matrix. The broad-phase AABB unions the
bind-pose box through every joint, mirroring the renderer's skinned scene-bounds fit. Without this
pass rigged models drew but could never be clicked.

> [!TIP]
> Picking depends on the AABB matching the flipped projection. The renderer applies
> `proj[1][1] *= -1` for drawing; `pickEntity` repeats it. The un-flipped
> [`cameraProjection`](../transform-and-matrices/) exists so the gizmo is not mirrored, but
> picking must re-apply the flip or every click would land on the vertically-mirrored object.

## In the code

| What | File | Symbols |
|---|---|---|
| The pick (both phases, both mesh kinds) | `assets.cppm` | `pickEntity` |
| Ray + intersection math | `geometry.cppm` | `Ray`, `rayTriangle`, `rayAabbSlab`, `worldAabbFromCorners` |
| CPU pick geometry | `renderer_types.cppm` | `GpuMesh::cpuPositions`, `cpuIndices`, `cpuSkin` |
| Mesh resolve + local bounds | `assets.cppm` | `loadMeshAsset`, `boundsMin`, `boundsMax` |
| Skinned deform palette | `scene.cppm` | `jointMatrices` |
| Matched projection | `scene.cppm` | `cameraProjection`, `CameraView` |

## Related
- [Transforms](../transform-and-matrices/) — the un-flipped projection picking re-flips
- [Selection](../../ui-and-editor/selection/) — what consumes the picked entity
- [Editor camera](../../ui-and-editor/editor-camera/) — the eye the pick ray shoots from
