+++
title = 'Collision layers, sensors, and contact events'
weight = 4
+++

# Collision layers, sensors, and contact events

Collisions become *selective* (which bodies test against which) and *observable* (gameplay learns
when bodies touch). Three pieces do this: a fixed layer set with a collision matrix, a sensor flag
that turns a collider into an overlap-only trigger, and a seq-cursored ring of contact events drained
two ways — over the control plane and into scripts.

## The v1 layer set + matrix

Every body lives in one **object layer**. v1 ships a fixed set (a project-authored matrix is
deferred):

| Layer | Holds |
|---|---|
| Static | immovable world geometry — the implicit layer of a collider with no rigidbody |
| Moving | dynamic + kinematic bodies (the default for a rigidbody) |
| Character | the character controller's body |
| Debris | dynamic bodies that collide with the world/characters but not each other |
| Sensor | trigger volumes — overlap-only, never solved |

A body's layer resolves by precedence in `resolve_object_layer`: **`is_sensor` → the moving slot the
rigidbody's `collision_layer` selects (0 = Moving, 1 = Character, 2 = Debris) → implicit Static** (a
lone collider or an explicit static rigidbody). The whole collision policy is one symmetric matrix
(`layers_collide`): a sensor overlaps every solid layer but never another sensor; two static bodies
never collide; debris collides with the world and characters but not other debris; everything else
collides. The same matrix is implemented twice — once in the C++ shim's filters, once in the safe
crate's `layers_collide` — and a test pins them against each other so neither drifts. Two
**broad-phase** layers back this (NonMoving for Static, Moving for the rest), kept to two because
more is a perf micro-opt not worth v1 complexity.

This is a deliberate simplification of UE5's collision channels / object-and-trace responses and
Unity's layer collision matrix — the same idea (a per-pair "do these collide?" table), with a small
fixed set instead of project-authored channels.

## Sensors: overlap without response

Setting `Collider.is_sensor` makes the body a Jolt sensor: it generates contacts but applies no
impulse. The body is placed in the Sensor layer, which the matrix lets overlap every solid layer —
crucially, the "no response" comes from the Jolt sensor flag, **not** from the matrix excluding the
pair (excluding it would suppress the event too). A trigger volume is the foundation for pickups,
zones, and detection.

## The contact-event ring

Jolt invokes its contact callbacks **from job threads during the step**, so the C++ `ContactListener`
must not touch the entity maps or the seq-stamped ring directly. The listener instead buffers the raw
body pairs (`PendingContact` PODs) under a small mutex; immediately after the Jolt update returns,
`World::step` drains that buffer over `saffron_physics_sys::drain_contacts`, maps each `BodyID` to its
entity uuid through `index_by_body_id` (single-threaded-safe there), stamps a monotonic `seq` + the
physics step, and appends to a bounded ring (oldest evicted at `CONTACT_RING_CAP`). v1 emits
**Begin/End** transitions only (`ContactKind::Begin` / `End`) — the trigger model — not a per-frame
"still overlapping" stream.

The ring has two independent consumers, each with its own cursor:

- **`drain-contacts {since}`** — a non-blocking control-plane drain. `World::drain_contacts` returns
  the `ContactDrain`: events with `seq > since`, plus `high_water_seq` / `oldest_seq` / `overflowed`
  so a stale cursor (e.g. held across a play stop/start, which resets the ring) detects it missed
  evictions and resyncs. In Edit it returns empty, never an error.
- **scripts** — each tick, after the step, the host drains the new events and `dispatch_contact`
  routes them: a sensor Begin calls `on_trigger_enter(self, other)`, a sensor End
  `on_trigger_exit(self, other)`, a solid Begin `on_contact(self, other, point, normal)`. A missing
  handler is a silent skip; a failing handler routes to the script-error ring (pause-on-error),
  exactly like `on_update`.

The two cursors are independent: scripts consume eagerly per tick, the control plane consumes lazily
by `since`, both reading the same ring.

## What | File | Symbols

| What | File | Symbols |
|---|---|---|
| Layer set + matrix + resolution | `engine/crates/physics/src/world.rs`, `src/types.rs` | `resolve_object_layer`, `ObjectLayer`, `layers_collide` |
| The Jolt filters + matrix (C++ shim) | `engine/crates/physics-sys/shim/jolt_bridge.cpp` | `ContactListener`, the broad-phase / object-layer filters, `jolt_layers_collide` |
| Contact drain + ring | `engine/crates/physics/src/world.rs`, `src/types.rs` | `World::step`, `World::drain_contacts`, `ContactEvent`, `ContactKind`, `CONTACT_RING_CAP` |
| Control drain | `engine/crates/control/src/commands_physics.rs` | `drain-contacts` |
| Script dispatch | `engine/crates/script/src/runtime.rs` | `dispatch_contact` |
