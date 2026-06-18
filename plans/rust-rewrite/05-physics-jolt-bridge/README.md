# 05 — Physics: the Jolt FFI bridge and the deterministic gameplay layer

This area ports `Saffron.Physics` — the Jolt-backed per-play physics world — to Rust as **two crates**:
`saffron-physics-sys` (the FFI seam: vendored Jolt 5.3.0 + a C++ shim, built by `build.rs` with the
determinism flags) and `saffron-physics` (the safe wrapper that orchestrates the world, written in
idiomatic borrow-checker-clean Rust). The split is the foundations contract: the unsafe boundary lives
in `-sys`, and `#![deny(unsafe_code)]` holds for `saffron-physics`.

> [!CAUTION]
> **This is the highest-risk area of the whole rewrite (pre-plan PP-11, a go/no-go gate).** The entire
> value of keeping Jolt rather than switching to rapier is one thing: **bit-exact-across-machines
> determinism** for the lockstep/replay premise. That bit-exactness is *not* a property of Jolt the
> library — it is a property of *how Jolt is built*: `CROSS_PLATFORM_DETERMINISTIC` on, single
> precision, `-ffp-model=precise -ffp-contract=off`, and a confined `-mavx2`. An off-the-shelf Jolt
> crate (`joltc-sys`, `rolt`) is built with none of these, pins the wrong Jolt version (5.0.0), and
> lacks ~40% of the API this engine uses (CharacterVirtual, Ragdoll, Skeleton, SwingTwist+motors,
> RotatedTranslatedShape, ExtendedUpdate). So we vendor 5.3.0 and own its build. **The determinism gate
> (the final phase) is BLOCKING: if x86 and ARM traces are not bit-identical through the new bridge, the
> lockstep premise collapses and the rewrite decision must be reconsidered before any gameplay ports on
> top.** Every phase before the gate is structured so the gate can run as early as possible.

---

## 1. The shape of the port (NO LEGACY)

The C++ subsystem is exemplary for FFI: a single TU (`physics.cpp`) is the *only* place `<Jolt/...>`
appears, hidden behind a `pimpl` (`PhysicsWorldImpl` in `physics.cpp`, opaque `struct` forward-declared
in `physics.cppm`). Everything the rest of the engine sees is **Jolt-free POD**: `PhysicsBodyInfo`,
`PhysicsRayHit`, `ContactEvent`, `RagdollState`, `glm::vec3`. The seam is already drawn exactly where
Rust wants it.

The Rust port keeps that seam but moves it to a crate boundary:

- **`saffron-physics-sys`** is the `pimpl` made physical. It vendors Jolt 5.3.0, compiles it + a small
  C++ shim with the determinism flags via `build.rs`, and exposes a `cxx`-bridged API surface. It owns
  every `unsafe` line. The shim carries the three virtual subclasses `cxx` cannot synthesize
  (`BroadPhaseLayerInterface`, `ObjectVsBroadPhaseLayerFilter`, `ObjectLayerPairFilter`) and the
  `ContactListener`, routing their callbacks back to Rust.
- **`saffron-physics`** is the `physics.cppm` public surface, re-expressed idiomatically. It holds the
  entity↔BodyID maps, the contact ring, the ragdoll/character bookkeeping, the fixed-step accumulator,
  and the orchestration logic (`step`, `populate`, `enable_ragdoll`, `write_ragdoll_poses`, …). It
  depends on `saffron-geometry` (glam, `Mesh`), `saffron-scene` (the components + `forEach`/world
  helpers), and `saffron-animation` (`JointPose`).

There is **one** physics world type, **one** code path per operation, and **no** retained C++ logic
beyond Jolt itself and the unavoidable shim. The GLM `(w,x,y,z)` ↔ Jolt `(x,y,z,w)` quaternion swizzle
(`toJolt`/`fromJolt` in `physics.cpp:158`) is *deleted*: glam's `Quat` is `xyzw`, the same storage order
as Jolt's `Quat`, so the conversion is a field copy with no reorder. That is a NO-LEGACY simplification,
not a port — the swizzle existed only to bridge GLM's layout.

---

## 2. Why `cxx`, and what the shim must contain

`cxx` is chosen over `bindgen`/`autocxx` because the Jolt API surface this engine uses is *narrow and
hand-pickable* (a few dozen calls), heavily virtual (filters + listener), and template-heavy
(`Ref<T>`, collision collectors) — exactly where raw `bindgen` produces unusable output and `autocxx`'s
template handling is fragile. `cxx` lets us hand-author a flat, POD-only bridge header that we control,
with the C++ shim doing the Jolt-specific work on the C++ side of the wire. This matches the C++
engine's own design: the POD boundary already exists; `cxx` just makes it the FFI ABI.

**`cxx` cannot synthesize a C++ virtual subclass from Rust.** Jolt requires four virtual interfaces to
be subclassed:

| Jolt interface | C++ class today (`physics.cpp`) | What it decides |
|---|---|---|
| `BroadPhaseLayerInterface` | `BroadPhaseLayerImpl` (`:91`) | object-layer → broad-phase layer (`broadPhaseFor`, `:85`) |
| `ObjectVsBroadPhaseLayerFilter` | `ObjectVsBroadPhaseImpl` (`:113`) | coarse cull: may a layer test a broad-phase tier |
| `ObjectLayerPairFilter` | `ObjectLayerPairImpl` (`:125`) | the v1 collision matrix (`layersCollide`, `:591`) |
| `ContactListener` | `ContactListenerImpl` (`:476`) | buffers `OnContactAdded`/`OnContactRemoved` raw pairs |

These four become **C++ shim classes in `saffron-physics-sys`**, authored once. The first three encode
pure, fixed v1 policy (the two-broad-phase split, the symmetric collision matrix) — there is no reason
to route them back to Rust per-call, so the shim implements them directly in C++ (the matrix is a
~10-line switch, ported verbatim from `layersCollide`). The `ContactListener` shim, however, must feed
Rust: it pushes `PendingContact { a, b, point, normal, begin }` POD records into a C++-side mutex-guarded
buffer that a `cxx` `drain()` call hands to Rust as a `Vec`. This mirrors `ContactListenerImpl::drain`
(`physics.cpp:501`) exactly — Jolt invokes the callbacks from job threads, so the buffer must be
mutex-guarded on the C++ side and drained on the sim thread, never touched from a callback. (Routing the
callback all the way into Rust is rejected: it would put a Rust `FnMut` on a Jolt job thread, fighting
`!Send` and adding a re-entrancy hazard for zero benefit — the data is POD and the drain is the natural
seam.)

---

## 3. The determinism build (the heart of the gate)

`saffron-physics-sys/build.rs` reproduces `cmake/Dependencies.cmake:68-109` exactly. The flags are not
optional and not defaults — they are the contract:

- **`CROSS_PLATFORM_DETERMINISTIC` ON** (`Dependencies.cmake:75`) — the master switch; forces Jolt onto
  its deterministic math paths.
- **single precision** (`DOUBLE_PRECISION OFF`, `:76`) — `JPH_DOUBLE_PRECISION` undefined; `Real == float`.
- **`-ffp-model=precise -ffp-contract=off`** — Jolt's determinism build adds these; they forbid the
  compiler from contracting `a*b+c` into an FMA (which differs bit-for-bit across micro-architectures).
  Under clang 21 `-Werror` they trip `-Woverriding-option`, so Jolt's own TUs build with `-Wno-error`
  (`Dependencies.cmake:93`) — `build.rs` must do the same (it is third-party code).
- **`-mavx2`** confined to the Jolt + shim TUs (`Dependencies.cmake:103`, `:108`) — ABI-relevant SIMD
  width; it must be identical to what the C++ engine used or the trace diverges. It is applied *only* to
  the `cc`/`cxx-build` TUs in `-sys`, never to the rest of the workspace (the C++ engine isolated it
  from `import std` for BMI reasons; we isolate it because it must not leak into other crates' codegen).
- **`-pthread` dropped from compile, kept at link** (`Dependencies.cmake:109`) — in C++ it was a
  module-langopt hazard; in Rust it is simply "link `Threads`," which `cc`/cargo handle at link time.

The matching `JPH_*` preprocessor defines (`JPH_CROSS_PLATFORM_DETERMINISTIC`, the absence of
`JPH_DOUBLE_PRECISION`, the AVX2 feature defines Jolt derives) must reach **both** the Jolt TUs and the
shim TU, because they change Jolt's struct layouts (`Vec3`, `Quat`, `RVec3`) — an ABI mismatch between
the shim and Jolt is a silent memory-corruption bug, not a link error. `build.rs` passes them uniformly.

> [!IMPORTANT]
> The single most likely cause of a failed determinism gate is a **flag drift**: a default `cc` build,
> a missing `-ffp-contract=off`, a `-march=native` that picks AVX-512 on one machine, or an FMA the
> compiler contracted. The gate phase's job is to *catch* that, and the build phase's job is to make the
> flags a single audited list in `build.rs` that mirrors `Dependencies.cmake` line-for-line. Pin Jolt at
> exactly `v5.3.0`; treat a Jolt version bump as a replay-format migration, never a silent dependency
> update.

---

## 4. The orchestration layer (ported 1:1, idiomatic)

Everything in `physics.cpp` outside the four shim classes is pure orchestration over the Jolt-free POD
boundary, and ports to safe Rust. The decision-locked mapping:

- **`PhysicsWorld` → an owned struct with `Drop`.** `PhysicsWorldImpl` (`physics.cpp:546`) becomes the
  `saffron-physics` `World` struct holding the `cxx` `UniquePtr<JoltWorld>` (the shim's world handle)
  plus the Rust-side bookkeeping (`bodies: Vec<BodyEntry>`, `index_by_body_id: HashMap<BodyId, usize>`,
  `contact_ring: VecDeque<ContactEvent>`, `characters`, `ragdolls`, the accumulator + counters). The
  C++ teardown order discipline (`ContactListener` and characters/ragdolls declared so they outlive
  `system`; `~PhysicsWorldImpl` calls `RemoveFromPhysicsSystem` on every live ragdoll before its bodies
  destruct, `physics.cpp:571`) becomes an explicit `Drop` impl in the **shim's** world destructor — the
  shim owns the Jolt teardown order, so the Rust `Drop` is just "drop the `UniquePtr`," and the C++
  destructor does the ragdoll-detach-then-destroy dance. This keeps the order correct at the only place
  that can see Jolt types.
- **`BodyEntry` / `RagdollEntry`** (`physics.cpp:518`, `:530`) → plain Rust structs in `saffron-physics`,
  stored in **creation order** (a `Vec`, never a `HashMap` iteration) so the sim stays reproducible — the
  ordering is load-bearing for determinism, called out in the C++ comment at `:517`.
- **shapes + auto-fit** (`buildColliderShape`, `physics.cpp:367`; the auto-fit lives in
  `control_commands_scene.cpp:255` `fitColliderToMesh` and `:330` `fitBoneCapsules`) — the five shapes
  (Box/Sphere/Capsule/ConvexHull/Mesh) map to shim shape-builders; the cook source
  (`MeshCookSource = std::function<Result<Mesh>(Uuid)>`, `physics.cppm:84`) becomes a Rust trait object
  / `FnMut(Uuid) -> Result<Mesh>` the host supplies (keeps the asset reader out of the FFI crate). The
  ConvexHull/Mesh vertex feed is in **index order** for reproducibility (`physics.cpp:404`).
- **sensors/triggers + the contact ring** — the `ContactEvent` POD + `ContactDrain` cursor model
  (`physics.cppm:113`, `:131`; ring drain in `stepPhysics`, `physics.cpp:1059`) ports verbatim: a bounded
  `VecDeque` of cap 256 (`ContactRingCap`, `physics.cpp:459`), seq-stamped, with the `overflowed`
  detection (`drainContacts`, `:1085`). The BodyID→entity mapping happens on the sim thread after the
  drain, exactly as today.
- **kinematic bone-following** (`buildBoneBodies`, `physics.cpp:844`; `MoveKinematic` in the step loop,
  `:979`) — a Kinematic capsule per driven joint, moved toward the animated `worldPose` each fixed step.
- **`CharacterVirtual`** (`addCharacter`, `physics.cpp:924`; `ExtendedUpdate` in the step loop, `:990`) —
  the controller (gravity integration, stick-to-floor, WalkStairs) ports 1:1; the shim exposes
  `ExtendedUpdate` + the ground-state query.
- **motor-driven SwingTwist ragdoll** (`enableRagdoll`, `physics.cpp:1216`; `driveRagdollsToPose`,
  `:1384`; `advanceRagdollBlend`, `:1431`; `writeRagdollPoses`, `:1318`; `setRagdollBlend`, `:1451`) —
  the full passive/active/partial blend layer, writing per-bone local TRS into `PoseOverrideComponent`
  blended by the eased per-bone weight. The world→local conversion (`glm::inverse(parentWorld) *
  partWorld`, then decompose; `:1349`) ports to glam `Mat4::inverse` + `to_scale_rotation_translation`.

The per-frame ordering the host imposes (`host.cppm:1142` `simTick`: drive ragdolls → advance blend →
step → write ragdoll poses → drain contacts to scripts) is a **host** concern (area 08), but the
`saffron-physics` functions it calls are designed to compose in exactly that order. The `sa.raycast`
host-callback POD bridge (`host.cppm:1200`) is preserved as the seam (area 12 wires it).

---

## 5. The two crates and the dependency edges

```
saffron-physics-sys   (no engine deps; build.rs builds vendored Jolt 5.3.0 + the C++ shim)
                      → exposes a cxx bridge: opaque JoltWorld + POD value types + the shim classes
                      → #![allow(unsafe_code)] with a top-of-file justification: "the Jolt FFI seam"

saffron-physics       → saffron-core, saffron-geometry, saffron-scene, saffron-animation,
                        saffron-physics-sys
                      → #![deny(unsafe_code)]; the safe wrapper + orchestration
```

`saffron-physics-sys` deliberately depends on **no** engine crate (not even `saffron-core`): it speaks
only POD across the `cxx` wire (plain `f32`/`u32`/`u64` and `cxx`-shared structs), so the FFI surface is
auditable in isolation and the determinism build has no transitive engine coupling. `saffron-physics`
does the glam/scene/animation translation on the safe side.

---

## 6. Error model

`saffron-physics` defines `enum Error` (thiserror) with a `Result<T>` alias, per the foundations
contract. The fallible C++ surface (`createPhysicsWorld`, `addCharacter`, `enableRagdoll`,
`setRagdollBlend` — all `Result<T>` / `Err(std::string)`) maps to typed variants
(`Error::WorldCreate`, `Error::NoWorld`, `Error::RagdollMismatch { expected, got }`,
`Error::BoneOutOfRange(i32)`, …; a `String` payload only where the failure is a bare message). The
no-op-on-null-world pattern (most mutators early-return when `impl == nullptr`) is the same in Rust:
those functions take `&mut World` so "no world" is a type-level impossibility — the `Option<World>`
lives in the host, and the host only calls these while a world exists (`host.cppm:1144`).

---

## 7. Grounding table

| What | File (engine-old / cmake) | Symbols |
|---|---|---|
| Public physics API | `engine-old/source/saffron/physics/physics.cppm` | `PhysicsWorld`, `createPhysicsWorld`, `initPhysics`, `stepPhysics`, `populatePhysicsWorld`, `enableRagdoll`, `driveRagdollsToPose`, `MeshCookSource`, `ContactEvent`, `ContactDrain` |
| Jolt-free POD types | `engine-old/source/saffron/physics/physics_types.cppm` | `MotionType`, `ObjectLayer`, `layersCollide`, `PhysicsFixedStep`, `PhysicsWorldStats`, `PhysicsBodyInfo`, `PhysicsRayHit` |
| The sole Jolt TU (pimpl + shims + orchestration) | `engine-old/source/saffron/physics/physics.cpp` | `PhysicsWorldImpl`, `BroadPhaseLayerImpl`, `ObjectVsBroadPhaseImpl`, `ObjectLayerPairImpl`, `ContactListenerImpl`, `BodyEntry`, `RagdollEntry`, `buildColliderShape`, `buildJointConstraint`, `boneMotorSettings`, `worldPose`, `toJolt`/`fromJolt`, `stepPhysics`, `writeRagdollPoses`, `driveRagdollsToPose`, `advanceRagdollBlend` |
| Jolt build flags | `cmake/Dependencies.cmake` | `CROSS_PLATFORM_DETERMINISTIC`, `DOUBLE_PRECISION`, `SAFFRON_JOLT_COMPILE_OPTIONS`, `-Wno-error`, `-pthread` removal, `v5.3.0` pin |
| Per-TU flag re-apply | `engine-old/CMakeLists.txt` | `SAFFRON_JOLT_COMPILE_OPTIONS` on `physics.cpp` (`:67-72`) |
| Physics components | `engine-old/source/saffron/scene/scene.cppm` | `RigidbodyComponent`, `ColliderComponent`, `PhysicsMaterial`, `KinematicBonesComponent`, `CharacterControllerComponent`, `BonePhysics`, `BonePhysicsComponent`, `PoseOverrideComponent` |
| Auto-fit | `engine-old/source/saffron/control/control_commands_scene.cpp` | `fitColliderToMesh` (`:255`), `fitBoneCapsules` (`:330`) |
| Host wiring (lifecycle + simTick) | `engine-old/source/saffron/host/host.cppm` | the `onPlayStateChanged` physics hook (`:1093`), `simTick` (`:1142`), the `raycast`/`sphereCast` callbacks (`:1200`) |
| Physics control commands (wire) | `engine-old/source/saffron/control/control_commands_physics.cpp` | the 12 `registerCommand` handlers |
| Physics DTOs | `engine-old/source/saffron/control/control_dto.cppm` | `PhysicsStateResult`, `PhysicsBodiesResult`, `ApplyImpulseParams/Result`, `FitColliderParams/Result`, `ContactEventDto`, `DrainContactsParams/Result`, `SetKinematicBonesParams`, `MoveCharacterParams/Result`, `RaycastParams`, `ShapecastParams`, `RaycastResult`, `EnableRagdollParams`, `SetRagdollParams`, `GetRagdollParams`, `RagdollResult` |
| Feasibility verdict | `plans/rust-rewrite-feasibility.md` | §4.3 (determinism), §5 dep matrix row `JoltPhysics 5.3.0`, §spike step 2 |

---

## 8. Phases

The phases are ordered so the determinism gate runs as early as the bound features allow. Build/bridge
and the shim land first; then world bring-up; then the two hard features the gate needs (one
SwingTwist-motor ragdoll, one `CharacterVirtual::ExtendedUpdate`); then the **gate**; then the remaining
gameplay surface (shapes/auto-fit, sensors/contact ring, kinematic bones, the full ragdoll blend layer,
queries). Putting the bare ragdoll + character *before* the gate matches the spike's pass condition
("the two hard features bound" + bit-identical traces); the richer gameplay surface lands after the gate
is green so a failure is caught before that investment.

1. `phase-1-sys-crate-and-jolt-build.md` — `saffron-physics-sys`: vendor Jolt 5.3.0, `build.rs` with the
   determinism flags, `initPhysics`/`shutdownPhysics` globals.
2. `phase-2-cxx-bridge-and-filter-shims.md` — the `cxx` bridge surface + the four C++ shim classes
   (3 filters + the contact buffer).
3. `phase-3-world-and-rigidbody-core.md` — `saffron-physics` `World`, body creation from
   collider/rigidbody, the step loop + dynamic write-back, stats/list/impulse.
4. `phase-4-character-and-bare-ragdoll.md` — `CharacterVirtual` + a passive SwingTwist ragdoll (the two
   gate features), no blend layer yet.
5. `phase-5-determinism-gate.md` — **BLOCKING.** Cross-arch (x86 + ARM) bit-exact trace diff vs the C++
   engine for a fixed stacking + ragdoll + character scenario.
6. `phase-6-shapes-and-autofit.md` — the five collision shapes, the mesh-cook trait seam, collider +
   bone-capsule auto-fit.
7. `phase-7-sensors-and-contact-ring.md` — sensor/trigger layers, the seq-stamped contact ring + drain
   cursor.
8. `phase-8-kinematic-bones.md` — kinematic bone bodies following the animated pose via `MoveKinematic`.
9. `phase-9-ragdoll-blend-and-motors.md` — the active/partial ragdoll: motor drive-to-pose, eased
   per-bone weight, `writeRagdollPoses` into `PoseOverrideComponent`, `setRagdollBlend`.
10. `phase-10-queries.md` — `raycastWorld` + `sphereCastWorld`, the `sa.raycast` host-callback POD seam.
