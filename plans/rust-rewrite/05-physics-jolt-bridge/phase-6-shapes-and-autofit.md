# Phase 6 — The five collision shapes, the mesh-cook seam, and auto-fit

**Status:** COMPLETED
**Depends on:** 05-physics-jolt-bridge:phase-5 (gate green), 02-math-and-geometry (`Mesh`)

## Goal

Complete the collider shape surface: the five shapes (Box already in phase 3 — add Sphere, Capsule,
ConvexHull, Mesh), the mesh-cook trait seam that keeps the asset reader out of the FFI crate, and the
collider + bone-capsule auto-fit logic. Runs after the gate so the cook-order determinism and the
shape-build paths sit on a proven base.

## Why this shape (NO LEGACY)

`buildColliderShape` (`physics.cpp:367`) is a per-shape switch; each branch maps to a shim shape-builder.
The cook source — `MeshCookSource = std::function<Result<Mesh>(Uuid)>` (`physics.cppm:84`) — is the seam
that lets ConvexHull/Mesh shapes read baked meshes without `Saffron.Physics` importing `Saffron.Assets`;
in Rust it is a `&mut dyn FnMut(Uuid) -> Result<Mesh>` the host passes to `populate` (the host binds it
to the asset reader, `host.cppm:1117`). Jolt-free `Mesh` (`saffron-geometry`) crosses the seam, never a
Jolt type. Cook inputs are fed in **index order** (`physics.cpp:404`, `:437`) so the cooked hull/mesh is
reproducible run-to-run — load-bearing for determinism, hence it lands deliberately after the gate but
keeps the ordering. A `Mesh` shape on a Dynamic body is a typed error (Jolt restriction,
`physics.cpp:417`), the caller skips it. Auto-fit lives in the control layer in C++
(`fitColliderToMesh`/`fitBoneCapsules`, `control_commands_scene.cpp:255`/`:330`) but is physics-shaped
geometry, so it ports as `saffron-physics` helpers the control crate calls — one place, no duplication
(the world-scale bake into the fitted extents, `:294`, is preserved).

## Grounding (real files/symbols)

- `engine-old/source/saffron/physics/physics.cpp:340-457` — `createShape` (virtual
  `ShapeSettings::Create`, null-on-error), `wrapOffset` (`RotatedTranslatedShape` for a non-zero
  offset), `buildColliderShape`: Box (he clamp + convex radius), Sphere (radius in `.x`), Capsule
  (radius `.x`, half-height `.y`), ConvexHull (cook → `Array<Vec3>` in index order), Mesh (cook →
  `VertexList` + `IndexedTriangleList`, Dynamic-rejected).
- `engine-old/source/saffron/physics/physics.cppm:84` — `MeshCookSource`.
- `engine-old/source/saffron/control/control_commands_scene.cpp:255-326` — `fitColliderToMesh`: AABB
  from the entity mesh, world-scale bake (`:294`), per-shape extent derivation (Box `:310`, Sphere
  `:314`, Capsule radius/halfHeight `:321`).
- `engine-old/source/saffron/control/control_commands_scene.cpp:330-390` — `fitBoneCapsules`: per-bone
  capsule sizing from rest-pose bone lengths into `BonePhysics.shapeHalfExtents` (`:380`).
- `engine-old/source/saffron/scene/scene.cppm:214-229` — `ColliderComponent` shape enum + fields.

## Work

- Shim: shape-builders for Sphere, Capsule, ConvexHull (`ConvexHullShapeSettings` from a `Vec3` slice),
  Mesh (`MeshShapeSettings` from vertex + indexed-triangle lists), and `wrapOffset`
  (`RotatedTranslatedShape`). Mesh-on-Dynamic returns a null shape (the safe side maps to a typed
  error).
- `saffron-physics`: extend `populate` to build all five shapes; the cook closure feeds vertices/indices
  in index order. `Error::MeshShapeOnDynamic`, `Error::CookFailed(String)`, `Error::NoCookSource` typed
  variants for the failure paths (the C++ `logError` + skip becomes a typed error the caller logs/skips).
- `fit_collider_to_mesh(scene, entity) -> bool` and `fit_bone_capsules(scene, entity) -> bool` helpers
  (the world-scale bake, the AABB→shape derivation, the per-bone rest-length capsule sizing); the
  control crate (area 09) calls these from add-component + the `fit-collider` command.

## Acceptance gate

- `cargo build -p saffron-physics`/`-p saffron-physics-sys` succeed.
- A `#[test]` per shape: a Dynamic Sphere/Capsule rests on a floor; a ConvexHull cooked from a cube mesh
  behaves like a box; a static Mesh floor catches a falling box.
- A `#[test]` `mesh_on_dynamic_errors` asserts a Mesh shape on a Dynamic body yields the typed error and
  the body is skipped (the world still builds).
- A `#[test]` `autofit_box` asserts `fit_collider_to_mesh` produces half-extents matching a known mesh
  AABB with the entity world scale baked in; `autofit_capsule` checks radius/half-height.
- The determinism trace (phase 5) re-run with ConvexHull/Mesh bodies still produces a stable hash across
  two runs (cook order is reproducible).
