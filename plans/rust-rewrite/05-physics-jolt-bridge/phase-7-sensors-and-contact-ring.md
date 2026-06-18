# Phase 7 — Sensors/triggers and the seq-stamped contact ring

**Status:** COMPLETED
**Depends on:** 05-physics-jolt-bridge:phase-6

## Goal

Wire the sensor/trigger layer and the contact-event pipeline end to end: bodies marked `isSensor` go to
the Sensor object layer; the `ContactListener` shim's buffered pairs (phase 2) are drained each step,
mapped BodyID→entity on the sim thread, seq-stamped, and pushed into a bounded ring; `drain_contacts`
serves a non-blocking cursor with overflow detection. This is what feeds script contact handlers (area
12) and the `drain-contacts` control command.

## Why this shape (NO LEGACY)

The C++ `stepPhysics` drains `contactListener.drain()` after `Update` and converts each `PendingContact`
into a seq-stamped `ContactEvent`, evicting the oldest at `ContactRingCap = 256`
(`physics.cpp:1059-1082`). In Rust the shim already buffers under a C++ mutex (phase 2); `World::step`
calls `jolt_drain_contacts`, looks up each body in `index_by_body_id` (sim-thread-only, the map is
stable for the play session), and pushes a `ContactEvent` into a `VecDeque<ContactEvent>` (cap 256,
`pop_front` on overflow). The `sensor` flag is `(a is sensor) || (b is sensor)` from the `BodyEntry`
records. `drain_contacts(since)` returns events with `seq > since` plus `high_water_seq`/`oldest_seq`/
`overflowed` so a stale cursor detects evictions (`physics.cpp:1085-1105`) — the same drain-cursor shape
the alarms/script-errors rings use. `ObjectLayer::Sensor` and the matrix entry (sensor overlaps every
solid but not another sensor) are already in the shim's `layers_collide` (phase 2). One ring, one drain,
no second path.

## Grounding (real files/symbols)

- `engine-old/source/saffron/physics/physics.cpp:459-512` — `ContactRingCap = 256`, `PendingContact`,
  `ContactListenerImpl` (the buffered Begin/End pairs; persisted ignored).
- `engine-old/source/saffron/physics/physics.cpp:1059-1082` — the drain → seq-stamp → ring-with-eviction
  loop in `stepPhysics` (entityA/B from `indexByBodyId`, `sensor` flag, `point`/`normal`, `tick =
  stepCount`).
- `engine-old/source/saffron/physics/physics.cpp:1085-1105` — `drainContacts`: events with `seq > since`,
  `highWaterSeq = contactSeq`, `oldestSeq`, `overflowed = oldestSeq > 0 && since + 1 < oldestSeq`.
- `engine-old/source/saffron/physics/physics.cppm:113-139` — `ContactEvent` (seq, Kind Begin/End,
  entityA/B, sensor, point, normal, tick), `ContactDrain`.
- `engine-old/source/saffron/physics/physics.cpp:802-805` — sensor body creation (`resolveObjectLayer`
  sensor precedence, `mIsSensor`).
- `engine-old/source/saffron/control/control_dto.cppm:466-485` — `ContactEventDto`, `DrainContactsParams`,
  `DrainContactsResult` (the wire shape the cursor serves).

## Work

- `saffron-physics`: `ContactEvent` (glam `Vec3`, the `Kind` enum, the fields above) and `ContactDrain`
  port. `World` gains `contact_ring: VecDeque<ContactEvent>`, `contact_seq: u64`, `step_count: i64`.
- In `World::step`, after `Update`: call `jolt_drain_contacts`, map each pair's bodies via
  `index_by_body_id`, seq-stamp, set `sensor` from the `BodyEntry` flags, push into the ring with
  cap-256 `pop_front` eviction, `tick = step_count`.
- `World::drain_contacts(&self, since: u64) -> ContactDrain` — the cursor query with overflow detection.
- Sensor bodies are already created in phase 3's `populate` (the `isSensor` → Sensor layer + `mIsSensor`
  path); confirm a sensor generates Begin/End but no solid response.

## Acceptance gate

- `cargo build -p saffron-physics` succeeds.
- A `#[test]` `solid_contact_begin_end` drops a box onto a floor and asserts a `Begin` event fires with a
  plausible point/normal, and an `End` fires when separated; `drain_contacts(0)` returns them in seq
  order.
- A `#[test]` `sensor_overlap` moves a body through a sensor volume and asserts the events carry
  `sensor = true` and no velocity change occurred (overlap-only, never solved).
- A `#[test]` `ring_overflow` generates >256 events and asserts `drain_contacts(stale)` reports
  `overflowed = true` and `oldest_seq` advanced.
