+++
title = 'Components'
weight = 2
+++

# Components

A component is a plain value struct: no base class, no virtual, no attached behavior — just data
the [systems](../ecs-architecture/) read with `forEach`. Behavior like serialize, inspect, and
clone is attached separately through the [component registry](../component-registry/), so the
struct stays a struct that depends on nothing but glm. The engine ships ten of them.

## Identity

Every entity from `createEntity` carries three components automatically: an id, a name, and a
transform.

```cpp
struct IdComponent   { Uuid id; };
struct NameComponent { std::string name; };
```

`IdComponent` is the entity's stable [Uuid](../scene-serialization/). entt handles are not stable
across runs, so anything serialized keys off the Uuid. It is not removable and is skipped during
serialization (written as the entity's `id` key, not inside `components`). `NameComponent` is the
label shown in the hierarchy.

## Transform

Rotation is stored as Euler XYZ in radians. The inspector edits these directly, which avoids the
gimbal clipping you would hit if rotation were a quaternion the UI tried to decompose. Matrix
composition is its own page — see [Transforms](../transform-and-matrices/).

```cpp
struct TransformComponent
{
    glm::vec3 translation{ 0.0f };
    glm::vec3 scale{ 1.0f };
    glm::vec3 rotation{ 0.0f };  // Euler XYZ radians
};
```

## Mesh and material

`MeshComponent` references a mesh asset by [Uuid](../asset-catalog-in-scene/); the
[AssetServer](../../geometry-and-assets/asset-server-and-catalog/) resolves it to a GPU mesh at
draw time. The component holds no GPU handle, so it survives a project reload that rebuilds the
caches.

```cpp
struct MeshComponent { Uuid mesh; };

struct MaterialComponent
{
    glm::vec4 baseColor{ 1.0f };
    Uuid albedoTexture;            // 0 == none
    f32 metallic = 0.0f;
    f32 roughness = 1.0f;
    glm::vec3 emissive{ 0.0f };
    f32 emissiveStrength = 1.0f;
    bool unlit = false;            // skip lighting — a distinct PSO
};
```

`MaterialComponent` is per-entity and applies to the whole mesh. `albedoTexture == 0` means
"none" (the renderer binds its default white texture, so `baseColor` shows directly);
`metallic`/`roughness` feed the [Cook-Torrance BRDF](../../lighting-and-brdf/cook-torrance-brdf/);
`unlit` selects a separate [PSO permutation](../../materials-and-pipelines/ubershader-and-specialization/).

## Camera

```cpp
struct CameraComponent
{
    f32 fov = 45.0f;       // vertical, degrees
    f32 nearPlane = 0.1f;
    f32 farPlane = 100.0f;
    bool primary = true;   // scene renders through the first primary camera
};
```

The camera's view comes from the entity's `TransformComponent`, not the component itself —
`primaryCamera` inverts the transform's model matrix. The component carries only projection
parameters. The scene renders through the first camera flagged `primary`.

## Light types

```cpp
struct DirectionalLightComponent
{
    glm::vec3 direction{ -0.5f, -1.0f, -0.3f };  // way the light travels
    glm::vec3 color{ 1.0f };
    f32 intensity = 1.0f;
    f32 ambient = 0.15f;
};

struct PointLightComponent { glm::vec3 color; f32 intensity; f32 range; };

struct SpotLightComponent
{
    glm::vec3 direction{ 0.0f, -1.0f, 0.0f };
    glm::vec3 color{ 1.0f };
    f32 intensity = 5.0f;
    f32 range = 10.0f;
    f32 innerAngle = 20.0f;  // full intensity inside this half-angle (deg)
    f32 outerAngle = 30.0f;  // zero past this half-angle (deg)
};
```

The directional light is the sun; the scene shades through the first one and carries a flat
`ambient` floor. Point and spot lights sit at the entity's `TransformComponent` translation (the
components hold no position of their own) and are
[culled into clusters](../../shadows-and-culling/clustered-light-culling/) by the light system.
See [light components](../../lighting-and-brdf/light-components/) for how `renderScene` packs
these into the GPU light buffer.

## Why no behavior on the struct

Pure data is what lets one entity be serialized, inspected, cloned, and rendered by four
unrelated subsystems with none of them knowing the others exist. A component carrying its own
virtual `serialize` would tie the struct to nlohmann/json, and its own UI to ImGui. The
per-component closures live in the [registry](../component-registry/) instead.

## In the code

| What | File | Symbols |
|---|---|---|
| Identity + transform | `scene.cppm` | `IdComponent`, `NameComponent`, `TransformComponent` |
| Renderables | `scene.cppm` | `MeshComponent`, `MaterialComponent` |
| Camera | `scene.cppm` | `CameraComponent`, `primaryCamera` |
| Lights | `scene.cppm` | `DirectionalLightComponent`, `PointLightComponent`, `SpotLightComponent` |
| Where each is registered | `editor_components.cpp` | `registerComponent<...>` |

## Related
- [Component registry](../component-registry/) — how behavior is attached to these structs
- [Transforms](../transform-and-matrices/) — the transform's matrix composition
- [Light components](../../lighting-and-brdf/light-components/) — how lights reach the GPU
