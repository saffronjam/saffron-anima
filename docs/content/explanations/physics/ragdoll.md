+++
title = 'Ragdoll'
weight = 8
+++

# Ragdoll

A ragdoll is the inversion of everything before it. A rigidbody reads the scene's transform and moves
a body; a ragdoll lets the **body drive the bone**. A clip-driven character is told to go limp, the
animation stops contributing, and it collapses to the floor under gravity and its joint limits —
landing through the **same** `PoseBuffer.override_`/`weight` blend layer foot IK already uses. That
seam was reserved for exactly this, which is why the ragdoll is mechanical rather than a rewrite.

## Built from the reserved metadata

`BonePhysicsComponent` has been on the rig the whole time — a parallel array to
`SkinnedMeshComponent.bones`, authored-but-inert, carrying per-bone `shapeHalfExtents`, `mass`, a
`joint` type (`Fixed` / `Hinge` / `SwingTwist` / `Free`), `swingTwistLimits`, and PD motor gains. The
ragdoll is its first consumer. On skinned import the component is **auto-fit** (a capsule per bone
sized from the joint-to-child rest distance), so a freshly imported character is ragdoll-ready;
hand-edit the fields after.

`enableRagdoll` maps that metadata onto a Jolt `Ragdoll`:

- a `Skeleton` built from the bone parent chain;
- one `Part` per bone — a Dynamic body with the capsule from `shapeHalfExtents`, mass from `mass`,
  seeded at the bone's **current world transform** so it spawns *on* the animated pose, not at the
  origin;
- a constraint to the parent part chosen by `joint`: `Fixed`→`FixedConstraint`, `Hinge`→`HingeConstraint`,
  `SwingTwist`→`SwingTwistConstraint` (cone + twist limits), `Free`→`PointConstraint`. A zero authored
  limit falls back to a sensible cone so a default ragdoll is floppy, not rigid.

`RagdollSettings::Stabilize()` runs once before `CreateRagdoll` so a long chain (spine→neck→head)
doesn't explode, then the ragdoll is added to the per-play world.

## Physics drives the bone (the inversion)

Each step, after `PhysicsSystem::Update`, `writeRagdollPoses` reads every part's world transform and
converts it to the bone's **local** TRS — `inverse(parentWorld) · world`, the exact inverse of
`jointMatrices`' composition — then writes it into the bone's `PoseOverrideComponent`. Because
`localMatrix` already prefers `PoseOverrideComponent`, the next `updateWorldTransforms` /
`jointMatrices` (same frame, in `renderScene`) renders the collapsed skeleton with **zero rendering
changes** — no new pass, the compute-skinning prepass just sees the new pose.

This is **binding mode c** at full weight: `writeRagdollPoses` runs *after* `tickAnimation` and the
step, so the physics override replaces the animation override for the frame — the clip is silenced on
ragdoll bones. Local-vs-world is the whole bug surface: the conversion must divide out the parent
bone's *ragdoll* world (or the rig entity's world for the root), never the composed pose (which would
be circular).

## Play is the lifetime; disable restores

The ragdoll lives in the per-play world and dies with the discarded play duplicate on stop — no
manual restore of the collapsed pose. `enable-ragdoll {entity, false}` removes it mid-play; the
animation evaluator then strips the now-stale `PoseOverrideComponent` from the inactive rig's bones
and they fall back to the rest/clip pose. A "death" event is just a caller of `enable-ragdoll` — no
special engine path.

> **Passive, not driven.** A bare `enable-ragdoll` leaves the parts pure-passive (`weight = 1`
> everywhere); the per-bone PD gains are parsed but the motors stay off, so the rig goes fully limp.
> Turning the motors on and mixing physics against the animation per bone is the active-ragdoll page.

## What | File | Symbols

| What | File | Symbols |
|---|---|---|
| Build + write-back + teardown | `engine/source/saffron/physics/physics.cpp` | `enableRagdoll`, `writeRagdollPoses`, `disableRagdoll`, `buildJointConstraint` |
| Import auto-fit | `engine/source/saffron/assets/assets.cppm` | `spawnSkinnedModel` (the `BonePhysicsComponent` fit) |
| Enable command | `engine/source/saffron/control/control_commands_physics.cpp` | `enable-ragdoll` |
| The reserved schema | `engine/source/saffron/scene/scene.cppm` | `BonePhysics`, `BonePhysicsComponent` |
