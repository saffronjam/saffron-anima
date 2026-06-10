+++
title = 'Components'
weight = 5
math = false
+++

# Components

Every built-in component exported by `Saffron.Scene`, with its fields, types, and defaults. The [component registry](../../explanations/scene-and-ecs/component-registry/) drives serialization and the inspector.

## `NameComponent`
| Field | Type | Default |
|---|---|---|
| `name` | `std::string` | — |

## `IdComponent`
| Field | Type | Default |
|---|---|---|
| `id` | `Uuid` | `{0}` |

Stable identity, created by `createEntity`. Not removable and not a registered row.

## `TransformComponent`
| Field | Type | Default |
|---|---|---|
| `translation` | `glm::vec3` | `{0,0,0}` |
| `scale` | `glm::vec3` | `{1,1,1}` |
| `rotation` | `glm::vec3` | `{0,0,0}` (Euler XYZ radians) |

## `MeshComponent`
| Field | Type | Default |
|---|---|---|
| `mesh` | `Uuid` | `{0}` (asset id; resolved by the AssetServer) |

## `MaterialComponent`
| Field | Type | Default |
|---|---|---|
| `baseColor` | `glm::vec4` | `{1,1,1,1}` |
| `albedoTexture` | `Uuid` | `{0}` (0 = none → default white; sRGB) |
| `metallicRoughnessTexture` | `Uuid` | `{0}` (0 = none; glTF map rough=G, metal=B; linear) |
| `metallic` | `f32` | `0.0` |
| `roughness` | `f32` | `1.0` |
| `emissive` | `glm::vec3` | `{0,0,0}` |
| `emissiveStrength` | `f32` | `1.0` |
| `unlit` | `bool` | `false` (distinct PSO) |

## `MaterialSetComponent`
A mesh imported with more than one material carries this instead of `MaterialComponent`. Each
[`Submesh.materialSlot`](../../explanations/geometry-and-assets/mesh-and-vertex-layout/) indexes `slots`.

| Field | Type | Default |
|---|---|---|
| `slots` | `std::vector<MaterialSlot>` | `{}` (each slot has the `MaterialComponent` fields) |

## `CameraComponent`
| Field | Type | Default |
|---|---|---|
| `fov` | `f32` | `45.0` (vertical, degrees) |
| `nearPlane` | `f32` | `0.1` |
| `farPlane` | `f32` | `100.0` |
| `primary` | `bool` | `true` (scene renders through the first primary) |
| `showModel` | `bool` | `true` (editor-only camera model) |
| `showFrustum` | `bool` | `true` (editor-only frustum overlay) |
| `frustumMaxDistance` | `f32` | `25.0` (editor-only frustum display cap) |

## `DirectionalLightComponent`
| Field | Type | Default |
|---|---|---|
| `direction` | `glm::vec3` | `{-0.5,-1.0,-0.3}` (travel direction) |
| `color` | `glm::vec3` | `{1,1,1}` |
| `intensity` | `f32` | `1.0` |
| `ambient` | `f32` | `0.15` |

## `PointLightComponent`
| Field | Type | Default |
|---|---|---|
| `color` | `glm::vec3` | `{1,1,1}` |
| `intensity` | `f32` | `5.0` |
| `range` | `f32` | `10.0` |

Positioned at the entity's `TransformComponent.translation`.

## `SpotLightComponent`
| Field | Type | Default |
|---|---|---|
| `direction` | `glm::vec3` | `{0,-1,0}` |
| `color` | `glm::vec3` | `{1,1,1}` |
| `intensity` | `f32` | `5.0` |
| `range` | `f32` | `10.0` |
| `innerAngle` | `f32` | `20.0` (half-angle degrees, full intensity inside) |
| `outerAngle` | `f32` | `30.0` (half-angle degrees, zero past) |

## Related
- [Built-in components](../../explanations/scene-and-ecs/built-in-components/) — what each is for
- [Component registry](../../explanations/scene-and-ecs/component-registry/) — how a component is registered and serialized
- [Light components](../../explanations/lighting-and-brdf/light-components/) — the three light types in the BRDF
