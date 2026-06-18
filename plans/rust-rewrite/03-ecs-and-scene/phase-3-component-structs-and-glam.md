# Phase 3 — Component structs + glam types

**Status:** COMPLETED

**Depends on:** 03-ecs-and-scene:phase-1-scene-crate-skeleton-and-ecs-adapter, 02-math-and-geometry:glam-adoption

## Goal

Port every scene component struct and the supporting value types (environment, atmosphere, asset
catalog) as plain Rust structs with `glam` fields. No serde, no registry yet — just the data model, the
`Default` impls matching the C++ in-struct defaults, and the runtime-only-vs-serialized distinction.
This makes the component set real so the registry (phase-5) and serde (phase-6) have concrete types to
bind.

The 24 serialized components: `Name`, `Transform`, `Relationship`, `ComponentOrder`, `Mesh`, `Material`,
`MaterialSet` (+`MaterialSlot`), `MaterialAsset`, `ModelInstance`, `Script` (+`ScriptSlot`), `Camera`,
`DirectionalLight`, `PointLight`, `SpotLight`, `ReflectionProbe`, `SkinnedMesh`, `Bone`, `AnimationPlayer`,
`FootIk` (+`FootChain`), `BonePhysics`-component (+`BonePhysics`), `Rigidbody`, `Collider`
(+`PhysicsMaterial`), `KinematicBones`, `CharacterController`. The runtime-only (never serialized, never
copied): `Relationship`'s `parent_handle`/`children` caches, `WorldTransform`, `PoseOverride`,
`SkinnedMesh.bone_handles`, plus `Id`/`ComponentOrder` left unregistered.

## Why this shape (NO LEGACY)

- **glam fields, with the `Vec3` (12 B) pin.** Every `glm::vec3`/`vec4`/`quat`/`mat4`/`bvec3` →
  `glam::Vec3`/`Vec4`/`Quat`/`Mat4`/`BVec3`. The geometry area (02) pins `Vec3` (12 B), never `Vec3A`
  (16 B), so the std430/byte layouts downstream stay correct; scene components inherit that pin.
- **The data-carrying enums become Rust enums.** `SkyMode`, `AnimationPlayer::Wrap`/`Transition`,
  `Rigidbody::Motion`, `Collider::Shape`, `BonePhysics::Joint`, `AssetType`, `Colorspace` are C++
  `enum class` → Rust `enum` (the win the feasibility study flags: enums replace the manual switch). The
  *string* wire spelling lives in phase-6's serde, not on the enum's repr.
- **Defaults carried exactly.** The C++ in-struct member initializers (e.g. `Rigidbody.linearDamping =
  0.05`, `Spot.outerAngle = 30`, `Camera.fov = 45`, `Collider.halfExtents = {0.5}`) become `Default`
  impls / `#[serde(default = …)]` later — but the *values* are fixed here so phase-6's
  default-on-missing reads match. A wrong default silently changes loaded data, so this is load-bearing.
- **Runtime-only components are a distinct, unregistered set.** `PoseOverride` (the animated TRS
  override, `scene.cppm:128`) uses a `Quat` directly (no Euler round-trip); `WorldTransform` is a cached
  `Mat4`; `Relationship`'s `parent` (Uuid) is the only durable field, with `parent_handle: Option<Entity>`
  and `children: Vec<Entity>` as rebuilt caches. These never serialize and never copy — encoded by simply
  not registering them (phase-5) and by the registry's `copy_to` skipping them.
- **`ScriptSlot.overrides` stays opaque JSON.** It is a `serde_json::Value` (defaulted `{}`), passed
  through verbatim — the editor fills it; the engine never interprets it (`scene.cppm:342`).

## Grounding (real files / symbols)

- `engine-old/source/saffron/scene/scene.cppm`: all component structs (lines 32–408), `transformMatrix`
  (410, used by phase-4 but the `Transform` struct + Euler-XYZ convention is here), `AssetType`/`Colorspace`
  (421/433), `AssetEntry`/`AssetCatalog` (441/458) + `findAsset`/`putAsset`/`renameAsset`/`uniqueName`
  (465–529), `SkyMode` (534), `AtmosphereSettings` (545), `SceneEnvironment` (563).
- Runtime-only markers: `RelationshipComponent` (52, "never serialized or copied"), `WorldTransformComponent`
  (61), `PoseOverrideComponent` (128), `SkinnedMeshComponent.boneHandles` (90). The `scene/AGENTS.md`
  "runtime-only components" rule.

## Acceptance gate

- Cargo workspace compiles; `saffron-scene` builds with all component structs + value types present.
- `cargo test -p saffron-scene`: a `#[test]` constructs each component via `Default` and asserts a sample
  of the non-trivial defaults (e.g. `Rigidbody::default().linear_damping == 0.05`,
  `SpotLight::default().outer_angle == 30.0`, `Collider::default().half_extents == Vec3::splat(0.5)`,
  `Camera::default().fov == 45.0`), so a drifted default is caught here, not at load time.
- `AssetCatalog` ops (`find_asset`/`put_asset`/`rename_asset`/`unique_name`) have a round-trip `#[test]`
  including the `" (2)"`/`" (3)"` collision-suffix behavior.
- Workspace build green; prior phases still pass.
