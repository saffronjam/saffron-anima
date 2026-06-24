# Wheeled vehicles

**Status:** PENDING IDEA

> Inspiration backlog — not yet implementable as written. Needs a codebase pass (the `saffron-physics`
> / `saffron-physics-sys` FFI surface, component registry, inspector) before it becomes a real plan.

Top ROI in the whole catalog. The hard part — the full vehicle constraint solver — is **already
vendored inside Jolt 5.3.0**; the remaining work is FFI, components, and a curve-editor widget. No GPU,
terrain, or particle dependency.

## What it is

Driveable, physics-based ground vehicles: a chassis rigidbody plus per-wheel suspension, tire friction,
and a drivetrain (engine torque → gearbox → differential → wheels). Both an arcade preset (forgiving,
snappy) and a simulation model (slip curves, gearing) sit on the same solver.

- **UE5:** Chaos Vehicles (`UChaosWheeledVehicleMovementComponent`) — suspension, tire config,
  torque/RPM curves, transmission, differential.
- **Unity:** `WheelCollider` (raycast suspension + a simplified slip-based friction model); most serious
  Unity vehicles use a third-party tire model on top.

## Core technique

Per wheel: a raycast (or shapecast) probe finds the ground; a spring-damper produces the suspension
force along the contact normal. Tire friction uses **longitudinal slip ratio** and **lateral slip
angle** fed through a friction curve (Pacejka-style "magic formula" or a simpler combined-slip curve),
clamped to a **friction circle** so a tire can't exceed its grip budget across both axes at once. The
drivetrain integrates an engine torque curve through a gearbox ratio and a differential to deliver wheel
torque. Jolt's `VehicleConstraint` + `WheeledVehicleController` / `MotorcycleController` /
`TrackedVehicleController` implement all of this, including limited-slip differentials, anti-roll bars,
and the wheel collision testers.

## How UE5 / Unity do it (notes worth keeping)

- UE5 separates the *movement component* (the simulation) from the skeletal *vehicle mesh* — wheels are
  bones driven by the sim each frame. We already have **kinematic bone-following**, so wheel-mesh binding
  reuses that path.
- UE5 exposes torque/RPM and steering/speed as editable **curves** — this is why a 1D curve-editor
  widget is a shared enabler (also wanted by time-of-day and post-FX tuning).
- Unity's `WheelCollider` is notoriously fiddly precisely because it hides the tire model; shipping a
  *visible* slip-curve config is a differentiator.

## Build size

- **M** for a driveable wheeled-vehicle sim (suspension + slip-curve tires + drivetrain).
- **S** for an additional arcade controller preset on the same solver.
- Motorcycle / tracked variants are incremental once the base constraint is wired.

## Dependencies (do these first)

- **cxx-FFI surface for the Jolt vehicle module** — the one genuinely new code; the pattern is already
  proven by `saffron-physics-sys` (vendored Jolt via `cxx`).
- **1D curve-editor widget** (shared enabler) — torque/RPM, steering, friction curves. Could ship with a
  numeric fallback first.
- *Nice to have, not required:* `heightfield-terrain` for "drive across landscape" — vehicles themselves
  need no terrain; they drive on any collider today.

## What we reuse / what's missing

**Reuse:** the vendored Jolt vehicle module (the whole solver), the per-play deterministic world,
rigidbody/collider components + auto-fit, raycast/shapecast as wheel collision testers, the contact-event
ring, `sa`/Luau for input, `submit_overlay` for debug (suspension rays, contact points), and the JSON
registry + inspector for config.

**Missing:** the cxx FFI surface for the vehicle classes, the curve-editor widget, and skeletal
wheel-mesh binding (reuse bone-following).

## Notes & references

- Jolt vehicle sample + `VehicleConstraint` docs (jrouwe/JoltPhysics) — the authoritative source for the
  controller types and tester options.
- Pacejka "magic formula" tire model (combined-slip friction circle) — background for the slip curves.
- UE5 Chaos Vehicles docs (dev.epicgames.com) — for the component decomposition (movement vs. mesh).
