# Phase 4 — Hierarchy + transform math

**Status:** COMPLETED

**Depends on:** 03-ecs-and-scene:phase-3-component-structs-and-glam

## Goal

Port the hierarchy and transform core — the pure-CPU math the renderer, animation, and gizmo all sit on
top of: local/world matrix composition, the parent-uuid → handle cache rebuild, the per-frame world
transform write, the skinning joint palette, the sanctioned reparent, and the numerically-stable ZYX
euler extraction. This is the most numerically delicate non-FFI work in the area, and the C++
`runSceneHierarchySelfTest` is the oracle to port into `#[test]`s.

Functions: `transform_matrix` (T·R·S), `local_matrix` (pose-override-aware), `compose_world_matrix`
(exact parent-chain walk), `world_matrix`/`world_translation`/`world_rotation` (cached + scale-divided
rotation), `relink_hierarchy` (cache rebuild + sanitize + skin joint resolve), `update_world_transforms`
(roots-first write), `joint_matrices` (world·inverseBind palette), `set_local_from_matrix` (TRS decompose),
`set_parent` (the only reparent), `quat_to_euler_zyx`, `primary_camera`/`camera_projection`,
`model_root_of`/`animatable_descendant`, and `destroy_entity`'s subtree gather (moved here from phase-1's
stub since it needs the children caches).

## Why this shape (NO LEGACY)

- **`quat_to_euler_zyx` is hand-ported, not delegated to glam.** The C++ uses
  `glm::extractEulerAngleZYX` because `glm::eulerAngles` is unstable at yaw ±90° (`scene.cppm:984`). glam
  has no ZYX matrix-extraction helper, so this is a faithful hand-port of the Rz·Ry·Rx extraction from the
  rotation matrix, with the degenerate-case branch preserved. The feasibility study explicitly flags
  "ZYX euler stability glam doesn't give for free" — getting it wrong silently corrupts every
  gizmo-rotate and reparent-rebase that round-trips a quaternion to the `Transform`'s Euler.
- **`set_parent` stays the single sanctioned reparent.** It refuses self-parenting and cycles (walks the
  new parent's ancestry), `keep_world` rebases the child local TRS so the world pose is unchanged, sets
  the durable `parent` uuid (not the handle), and calls `relink_hierarchy`. No second reparent path —
  the editor and control both route through it (`scene.cppm:1016`).
- **`relink_hierarchy` rebuilds *all* runtime caches and sanitizes.** One O(N) pass: defaults a root
  `Relationship` onto entities missing one, resets dangling / self / cycle parents to root with a warning
  (the caches always form a forest), and resolves `SkinnedMesh.bone_handles` from joint uuids. The
  warnings go through saffron-core's `log_warn`. Called after every structural change.
- **`update_world_transforms` is roots-first recursive, full-mat4.** ECS iteration order is never relied
  on for ordering (the recursion provides it); full `Mat4` composition preserves non-uniform parent scale
  so the downstream `normal_matrix = transpose(inverse(mat3(world)))` stays correct (`scene.cppm:920`).
- **`set_local_from_matrix` uses glam's decompose, returning a bool on failure.** glam's
  `Mat4::to_scale_rotation_translation` is the `glm::decompose` analogue; on a non-decomposable matrix the
  C++ returns false and leaves the transform untouched — the Rust port returns `false`/`Option::None`
  identically (TRS-only; shear is dropped, accepted).

## Grounding (real files / symbols)

- `engine-old/source/saffron/scene/scene.cppm`: `transformMatrix` (410), `localMatrix` (858),
  `composeWorldMatrix` (870), `worldMatrix`/`worldTranslation`/`worldRotation` (889/898/904),
  `relinkHierarchy` (762), `updateWorldTransforms` (920), `jointMatrices` (957), `quatToEulerZYX` (984),
  `setLocalFromMatrix` (995), `setParent` (1016), `primaryCamera`/`cameraProjection` (1093/1116),
  `modelRootOf`/`animatableDescendant` (707/673), `destroyEntity` (637).
- The oracle: `runSceneHierarchySelfTest` (`scene.cppm:1854`) — parent/child/grandchild composition,
  cycle + self-parent guards, `keep_world` rebase under axis-aligned and generic rotations, recursive
  destroy with no dangling cache handle, parented-camera view, and the bind-pose/moved-joint
  `joint_matrices` checks (the "research gate CPU half").

## Acceptance gate

- Cargo workspace compiles; `saffron-scene` exposes the hierarchy/transform surface.
- `cargo test -p saffron-scene` ports `runSceneHierarchySelfTest` as `#[test]`s and they pass:
  `child_world == parent_world * child_local`, grandchild world translation, cycle + self-parent
  rejection, `keep_world` preserves world transform (axis-aligned and generic rotation) while rebasing the
  local, recursive destroy removes the subtree and leaves no dangling child-cache handle, two roots after
  reparent+destroy, parented primary camera views from its world position, bind-pose joint matrices are
  identity, and a tip-bound vertex follows a moved joint (all to the C++ `1e-4` tolerance).
- A dedicated `quat_to_euler_zyx` `#[test]` covers the yaw ±90° degenerate case against C++-matching
  values.
- Workspace build green; prior phases still pass.
