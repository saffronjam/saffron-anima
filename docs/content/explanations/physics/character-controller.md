+++
title = 'Character controller'
weight = 6
+++

# Character controller

A walking character is not a rigid body. A box-on-a-floor tumbles, bounces, and resolves penetration
the way the solver sees fit — none of which is how a player expects to move. The engine uses Jolt's
**`CharacterVirtual`**: a kinematic *sweep* object that pushes a capsule through the world and
resolves penetration and wall-sliding without being a simulated body. It walks across floors, steps
over small ledges, and slides along walls natively.

## The component split

A character entity is a `Transform` + a `Collider` (a capsule) + a `CharacterController` — and **no
`Rigidbody`**. The controller reuses the collider's auto-fit capsule (radius + half-height) rather
than introducing a second capsule, so there is one source of truth for the shape. The component
carries only movement parameters: a horizontal `max_speed`, a `max_step_height` (ledges up to this
are stepped over), a `gravity_factor`, and a `desired_velocity`. The integrated `vertical_velocity`
and the last `on_ground` state are runtime fields, reset on each play.

A `CharacterVirtual` is **not** a body, so `World::populate` skips making a static body for a collider
that also has a `CharacterController` — otherwise the static capsule would block the character's own
sweep. The sweep object is built once with `World::add_character`.

## Stepping and write-back

Each fixed substep, after the rigid-body update settles the world, `World::step_characters` advances
every character: gravity accumulates into `vertical_velocity` (zeroed when grounded and not moving
up), the desired velocity is folded in and clamped to `max_speed`, and Jolt's
`CharacterVirtual::ExtendedUpdate` (the `character_extended_update` FFI) runs the stick-to-floor +
WalkStairs sweep against the world, using the layer filters from the collision matrix (the character
lives in the `Character` layer). The resolved position is then written back into the entity-root
`Transform`, the same frame — so the visible mesh follows, exactly as the rigid-body and animation
write-backs do.

This is **binding mode a**: the controller positions the root and nothing more. Any animation player
on the entity plays *independently on top* — the controller never reads or drives the pose.
Root-motion extraction and locomotion blending are a different coupling, deliberately kept out of
here.

## move-character is the input seam

`move-character {entity, velocity, jump?}` writes the desired horizontal velocity (and an optional
jump impulse) onto the `CharacterController` component; the actual sweep happens on the next physics
step. The command only flips component fields — identical to how the foot-IK command only sets fields
the evaluator consumes next frame. For now this command *is* the input seam; mapping real input to it
is gameplay's job.

> **`CharacterVirtual`, not `Character` or a kinematic rigidbody.** The body-backed `Character` and
> the kinematic-rigidbody path are deliberately not reused — a character is its own thing. The
> controller assumes a root entity (local == world); a parented character is a later refinement.

## What | File | Symbols

| What | File | Symbols |
|---|---|---|
| The component | `engine/crates/scene/src/component.rs` | `CharacterController` |
| Create + step + write-back | `engine/crates/physics/src/world.rs` | `World::add_character`, `World::step_characters` |
| The CharacterVirtual FFI | `engine/crates/physics-sys/src/lib.rs` | `add_character`, `character_extended_update`, `character_on_ground` |
| The move seam | `engine/crates/control/src/commands_physics.rs` | `move-character` |
