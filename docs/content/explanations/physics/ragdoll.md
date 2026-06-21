+++
title = 'Ragdoll'
weight = 8
+++

# Ragdoll

A ragdoll is the inversion of everything before it. A rigidbody reads the scene's transform and moves
a body; a ragdoll lets the **body drive the bone**. A clip-driven character is told to go limp, the
animation stops contributing, and it collapses to the floor under gravity and its joint limits —
landing through the **same** `PoseOverride` blend layer foot IK already uses. That seam was reserved
for exactly this, which is why the ragdoll is mechanical rather than a rewrite.

## Built from the reserved metadata

`BonePhysics` has been on the rig the whole time — a parallel array to `SkinnedMesh.bones`
(in `BonePhysicsComponent`), authored-but-inert, carrying per-bone `shape_half_extents`, `mass`, a
`joint` type (`Joint::Fixed` / `Hinge` / `SwingTwist` / `Free`), `swing_twist_limits`, and PD motor
gains. The ragdoll is its first consumer. On skinned import the component is **auto-fit** (a capsule
per bone sized from the joint-to-child rest distance), so a freshly imported character is
ragdoll-ready; hand-edit the fields after.

`World::enable_ragdoll` maps that metadata onto a `BonePart` per bone and hands it to the C++ shim's
`add_ragdoll`, which builds a Jolt `Ragdoll`:

- a `Skeleton` built from the bone parent chain;
- one `Part` per bone — a Dynamic body with the capsule from `shape_half_extents`, mass from `mass`,
  seeded at the bone's **current world transform** so it spawns *on* the animated pose, not at the
  origin;
- a constraint to the parent part chosen by `joint` (mapped through `joint_raw`):
  `Fixed`→`FixedConstraint`, `Hinge`→`HingeConstraint`, `SwingTwist`→`SwingTwistConstraint`
  (cone + twist limits), `Free`→`PointConstraint`. A zero authored limit falls back to a sensible
  cone so a default ragdoll is floppy, not rigid.

`RagdollSettings::Stabilize` runs once before `CreateRagdoll` in the shim so a long chain
(spine→neck→head) doesn't explode, then the ragdoll is added to the per-play world.

## Physics drives the bone (the inversion)

Each step, after the Jolt update, `World::write_ragdoll_poses` reads every part's world transform and
converts it to the bone's **local** TRS — `inverse(parent_world) · world`, the exact inverse of the
joint-matrix composition — using the `parent_index` map cached at build, then writes it into the
bone's `PoseOverride`. Because the local-matrix composition already prefers `PoseOverride`, the next
`update_world_transforms` / joint-matrix pass (same frame) renders the collapsed skeleton with **zero
rendering changes** — no new pass, the compute-skinning prepass just sees the new pose.

This is **binding mode c** at full weight: `write_ragdoll_poses` runs *after* `tick_animation` and the
step, so the physics override replaces the animation override for the frame — the clip is silenced on
ragdoll bones. Local-vs-world is the whole bug surface: the conversion divides out the parent bone's
*ragdoll* world (or the rig entity's world for the root), never the composed pose (which would be
circular).

## Play is the lifetime; disable restores

The ragdoll lives in the per-play world and dies with the discarded play duplicate on stop — no
manual restore of the collapsed pose. `World::disable_ragdoll` (the `enable-ragdoll {entity, false}`
command) removes it mid-play; the animation evaluator then strips the now-stale `PoseOverride` from
the inactive rig's bones and they fall back to the rest/clip pose. A "death" event is just a caller
of `enable-ragdoll` — no special engine path.

> **Passive, not driven.** A bare `enable-ragdoll` leaves the parts pure-passive (`weight = 1`
> everywhere); the per-bone PD gains are parsed but the motors stay off, so the rig goes fully limp.
> Turning the motors on and mixing physics against the animation per bone is the active-ragdoll page.

## What | File | Symbols

| What | File | Symbols |
|---|---|---|
| Build + write-back + teardown | `engine/crates/physics/src/world.rs` | `World::enable_ragdoll`, `World::write_ragdoll_poses`, `World::disable_ragdoll`, `joint_raw` |
| The ragdoll FFI (C++ shim) | `engine/crates/physics-sys/src/lib.rs`, `shim/jolt_bridge.cpp` | `add_ragdoll`, `BonePart`, `ragdoll_part_transform` |
| Import auto-fit | `engine/crates/assets/src/spawn.rs` | `spawn_model`, `autofit_bone_physics` |
| Enable command | `engine/crates/control/src/commands_physics.rs` | `enable-ragdoll` |
| The reserved schema | `engine/crates/scene/src/component.rs` | `BonePhysics`, `BonePhysicsComponent`, `Joint` |
