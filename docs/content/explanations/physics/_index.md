+++
title = 'Physics'
weight = 18
bookCollapseSection = true
+++

# Physics

The engine simulates rigid bodies, collisions, and a motor-driven ragdoll through
**Jolt** (jrouwe/JoltPhysics), wrapped behind the `Saffron.Physics` module. Physics is a
*consumer* of the scene: it reads the components an entity carries, builds a Jolt world while the
game plays, steps it on the same fixed tick the rest of gameplay runs on, and writes results back
into transforms and the animation pose — all before the frame renders, with no new render pass.

The defining choice is that the Jolt world lives **exactly as long as play does**. It is built on
the `Edit → Playing` edge and discarded on `→ Edit`, against the throwaway scene duplicate the
editor already makes for play — so there is no authored data to reset, and stopping play *is* the
restore. Only one translation unit ever includes `<Jolt/…>`; every other part of the engine sees a
small Jolt-free vocabulary of plain enums and an opaque world handle.

This section starts at the bottom: the world's lifecycle and the module boundary the rest is built
on.

## Pages

| Page | Covers | Code |
|---|---|---|
| `physics-world-lifecycle` | the per-play-session Jolt world built on the play edge and discarded with the play duplicate; the cross-platform-deterministic, single-precision build; the Jolt-free module boundary and `physics-state` | `physics.cppm`; `physics.cpp`; `host.cppm` |
| `rigidbody-and-collider` | the split component model (collider = shape/material/sensor, rigidbody = motion/mass/damping/locks), the collider-alone-is-static rule, auto-fit on add, and the component → body → fixed-step → write-back loop | `scene.cppm`; `physics.cpp`; `host.cppm` |
| `collision-shapes` | the five shapes (box/sphere/capsule analytic + convex-hull/mesh cooked from the `.smesh`), the Mesh-on-dynamic loud rejection, deterministic cook ordering, shape-aware auto-fit, and `PhysicsMaterial` friction/restitution | `physics.cpp`; `assets.cppm`; `control_commands_scene.cpp` |
| `collision-layers-and-triggers` | the fixed object-layer set + collision matrix, the three Jolt filter interfaces, sensor (trigger) bodies, and the job-thread-safe contact-event ring drained to `drain-contacts` and to script `on_trigger_enter`/`on_contact` handlers | `physics.cpp`; `control_commands_physics.cpp`; `script_runtime.cpp` |
| `kinematic-bones` | the Kinematic motion type via `MoveKinematic` (not teleport), the three skeleton-binding modes, per-bone kinematic bodies that follow the animated pose (composing the fresh world matrix, not the stale cache), and auto-fit bone capsules | `physics.cpp`; `scene.cppm`; `control_commands_physics.cpp` |
| `character-controller` | the Jolt `CharacterVirtual` kinematic-sweep walker, the component split (capsule collider + controller params, no rigidbody), the per-step gravity + move-character drive with stick-to-floor / WalkStairs, and binding mode a (positions the root, animation independent) | `scene.cppm`; `physics.cpp`; `control_commands_physics.cpp` |
| `scene-queries` | raycast + sphere-cast against the live narrow phase (entity-uuid mapping, read-only off the step), the three surfaces (control command, `se`, the Host-bridged `se.raycast` Lua binding), and why queries refuse in Edit | `physics.cpp`; `control_commands_physics.cpp`; `script_runtime.cpp` |
| `ragdoll` | physics-drives-animation: a Jolt `Ragdoll` built from the reserved `BonePhysicsComponent` (shapes + per-joint constraints), the per-part world → bone-local `PoseOverride` write-back at full weight, import auto-fit, and `enable-ragdoll` | `physics.cpp`; `assets.cppm`; `scene.cppm` |
| `active-ragdoll` | the passive/active/partial spectrum: `SwingTwist` motors driven toward the animation target from the authored PD gains, the per-bone eased weight blend (the recover), `lastPose` as the motor target, import auto-fit + per-bone `set-component-field` editing, and `set-ragdoll`/`get-ragdoll` | `physics.cpp`; `host.cppm`; `control_commands_physics.cpp` |
