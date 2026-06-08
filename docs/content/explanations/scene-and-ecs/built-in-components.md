+++
title = 'Components'
weight = 2
+++

# Components

A component is a plain value struct: no base class, no virtual methods, no attached behavior — only
data the [systems](../ecs-architecture/) read with `forEach`. Behavior such as serialize, inspect,
and clone is attached separately through the [component registry](../component-registry/), so the
struct depends on nothing but glm.

Keeping a component pure data lets one entity be serialized, inspected, cloned, and rendered by
several unrelated subsystems, none aware of the others. A struct that carried its own virtual
`serialize` would bind itself to nlohmann/json. The per-component closures live in the
[registry](../component-registry/) instead.

## Identity

Every entity from `createEntity` carries three components automatically: an id, a name, and a
transform.

```cpp
struct IdComponent   { Uuid id; };
struct NameComponent { std::string name; };
```

`IdComponent` is the entity's stable [Uuid](../scene-serialization/). entt handles are not stable
across runs, so anything serialized keys off the Uuid. It is not removable and is skipped during
serialization — written as the entity's `id` key, not inside `components`. `NameComponent` is the
label shown in the hierarchy.

## Transform

Rotation is stored as Euler XYZ in radians. The inspector edits these values directly, avoiding the
gimbal clipping that arises when the UI decomposes a quaternion. Matrix composition is its own page;
see [Transforms](../transform-and-matrices/).

```cpp
struct TransformComponent
{
    glm::vec3 translation{ 0.0f };
    glm::vec3 scale{ 1.0f };
    glm::vec3 rotation{ 0.0f };  // Euler XYZ radians
};
```

## Hierarchy

`RelationshipComponent` makes the entity a node in the [scene tree](../scene-hierarchy/): a durable
parent Uuid (0 means root) plus runtime `parentHandle`/`children` caches that never serialize. Every
entity gets a root one from `createEntity`. It is registered non-removable, and parenting is edited
through `setParent` rather than as a raw field.

```cpp
struct RelationshipComponent
{
    Uuid parent;                              // 0 == root
    entt::entity parentHandle = entt::null;   // resolved cache
    std::vector<entt::entity> children;       // derived cache
};
```

## Mesh and material

`MeshComponent` references a mesh asset by [Uuid](../asset-catalog-in-scene/); the
[AssetServer](../../geometry-and-assets/asset-server-and-catalog/) resolves it to a GPU mesh at draw
time. The component holds no GPU handle, so it survives a project reload that rebuilds the caches.

```cpp
struct MeshComponent { Uuid mesh; };

struct MaterialComponent
{
    glm::vec4 baseColor{ 1.0f };
    Uuid albedoTexture;               // 0 == none
    Uuid metallicRoughnessTexture;    // 0 == none; glTF MR map (rough=G, metal=B)
    f32 metallic = 0.0f;
    f32 roughness = 1.0f;
    glm::vec3 emissive{ 0.0f };
    f32 emissiveStrength = 1.0f;
    bool unlit = false;               // skip lighting — a distinct PSO
};
```

`MaterialComponent` is per-entity and applies to the whole mesh. `albedoTexture == 0` means none:
the renderer binds its default white texture, so `baseColor` shows directly. `metallic` and
`roughness` feed the [Cook-Torrance BRDF](../../lighting-and-brdf/cook-torrance-brdf/); `unlit`
selects a separate [PSO permutation](../../materials-and-pipelines/ubershader-and-specialization/).
`metallicRoughnessTexture` is glTF's packed map (roughness in G, metalness in B), sampled and
multiplied into the scalar factors; it is a **linear** texture, unlike the sRGB albedo, and the
default white (when none) leaves the factors unchanged.

A mesh imported with more than one material instead carries a `MaterialSetComponent`: an ordered
table of slots, each with the same fields as `MaterialComponent`. Every
[`Submesh.materialSlot`](../../geometry-and-assets/mesh-and-vertex-layout/) indexes this table, so
each submesh draws with its own material.

```cpp
struct MaterialSlot { /* same fields as MaterialComponent */ };
struct MaterialSetComponent { std::vector<MaterialSlot> slots; };
```

An entity uses one or the other: single-material meshes (and hand-created entities) keep
`MaterialComponent`; a multi-material import gets `MaterialSetComponent`. The
[draw list](../../geometry-and-assets/draw-list/) reads whichever is present.

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

The camera's view comes from the entity's `TransformComponent`, not the component itself:
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
`ambient` floor. Point and spot lights sit at the entity's `TransformComponent` translation, since
the components hold no position of their own, and are
[culled into clusters](../../shadows-and-culling/clustered-light-culling/) by the light system. See
[light components](../../lighting-and-brdf/light-components/) for how `renderScene` packs these into
the GPU light buffer.

## In the code

| What | File | Symbols |
|---|---|---|
| Identity + transform | `scene.cppm` | `IdComponent`, `NameComponent`, `TransformComponent` |
| Hierarchy | `scene.cppm` | `RelationshipComponent` |
| Skeleton | `scene.cppm` | `SkinnedMeshComponent`, `BoneComponent` |
| Renderables | `scene.cppm` | `MeshComponent`, `MaterialComponent`, `MaterialSetComponent` |
| Camera | `scene.cppm` | `CameraComponent`, `primaryCamera` |
| Lights | `scene.cppm` | `DirectionalLightComponent`, `PointLightComponent`, `SpotLightComponent` |
| Where each is registered | `scene_edit_components.cpp` | `registerComponent<...>` |

## Related
- [Component registry](../component-registry/) — how behavior is attached to these structs
- [Transforms](../transform-and-matrices/) — the transform's matrix composition
- [Light components](../../lighting-and-brdf/light-components/) — how lights reach the GPU
