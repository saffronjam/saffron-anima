# Phase 13 — kinematic foot IK + reserved physics metadata

**Status:** COMPLETED

## Goal

Prove the Phase-3 blend layer admits an **external pose producer**, and reserve the data the eventual Jolt
powered-ragdoll will need. Build a general **two-bone analytic IK** solver, use it for **foot planting**
against a ground hook (pure kinematics, no physics engine) so animations begin to *react to the
environment* rather than only play out, and reserve **per-bone physics metadata** fields on the skeleton
so that when `Saffron.Physics` (Jolt) lands, wiring a powered ragdoll is mechanical, not a rewrite.

This is the architectural payoff of the README's "one load-bearing decision": IK here, and ragdoll later,
both write `override_[i]` + `weight[i]` into the same `PoseBuffer` blend layer — the sampling graph never
changes.

## What exists to build on

- Phase 1's `PoseBuffer` (`local` / `override_` / `weight`) and Phase 3's blend layer
  (`final[i] = blend(local[i], override_[i], weight[i])`). Phase 4's `PoseDelta` / slerp helpers.
- Phase 3's evaluator `tickAnimation` and the per-entity pose buffers; `worldMatrix`/`worldTranslation`/
  `worldRotation` (`scene.cppm:582-606`) for resolving bone world transforms; `SkinnedMeshComponent.bones`/
  `boneHandles` for joint addressing.
- The skeleton is implicit (bone entities) — per-bone metadata attaches as a parallel array on the rig
  (see §3), serialized like the other `SkinnedMeshComponent` data.

## Work

### 1. Two-bone analytic IK solver (in `Saffron.Animation`)

```cpp
/// Solve a 2-bone chain (e.g. thigh→shin→foot) so the end effector reaches `target`,
/// bending around `poleVector`. Returns local-space rotations for the upper and lower joints.
struct TwoBoneIkResult { glm::quat upper; glm::quat lower; };
auto solveTwoBoneIk(glm::vec3 root, glm::vec3 mid, glm::vec3 end,
                    glm::vec3 target, glm::vec3 poleVector,
                    f32 upperLen, f32 lowerLen) -> TwoBoneIkResult;
```

Standard law-of-cosines solve (ozz-animation / UE two-bone node): compute the knee/elbow angle from the
target distance clamped to `[|upperLen-lowerLen|, upperLen+lowerLen]`, orient the chain plane by the pole
vector, derive the two joint rotations. Pure function, unit-testable (reach an in-range target exactly;
clamp gracefully when over-extended).

### 2. Foot IK as a blend-layer producer

After `sampleClip` fills `pose.local` for a rig configured with foot chains:
- Determine each foot's **ground target**: query a ground hook (v1: a configurable ground-plane height, or
  a CPU raycast against a designated ground entity's mesh; full terrain/collision waits for physics). Adjust
  the planned foot position to sit on the ground and tilt the foot to the surface normal.
- Run `solveTwoBoneIk` for the leg chain; write the resulting joint rotations into `pose.override_[i]` and
  set `pose.weight[i]` to the foot-IK blend amount (e.g. ramp toward 1 when the foot is planted, toward 0
  when swinging) — possibly also a pelvis drop to keep both feet reachable.
- The Phase-3 blend then mixes animation (`local`) with IK (`override_`) per joint — **no change to the
  evaluator's structure**, which is the whole point.

Config: a small `FootIkConfig` (chain joint indices, pole vectors, ground source, enable) on the rig or the
`AnimationPlayerComponent`. Expose an enable toggle over the control plane (`se`), consistent with the
keep-scriptable rule.

### 3. Reserve per-bone physics metadata (Jolt-ahead, no behavior)

Add an optional, serialized per-joint metadata array (parallel to `bones[]`) capturing what a powered
ragdoll needs — authoring this is the expensive part; the Jolt build step is then mechanical:
```cpp
struct BonePhysics {
    glm::vec3 shapeHalfExtents{0.0f};   // capsule/box collider for the body
    f32 mass = 1.0f;
    enum class Joint : u8 { Fixed, Hinge, SwingTwist, Free } joint = Joint::SwingTwist;
    glm::vec3 swingTwistLimits{0.0f};   // radians
    f32 driveStiffness = 0.0f, driveDamping = 0.0f, driveMaxForce = 0.0f;  // PD motor gains
};
```
Store as an optional sidecar or an extension of the skeleton data; serialize via the catalog/component
path. **No runtime use in this phase** — it is purely the schema the Jolt phase will read. Keep it minimal
but UE-PhAT / Jolt-`RagdollSettings`-shaped so the future mapping is 1:1.

### 4. Capture `lastPose` for finite-difference velocities

Have the evaluator snapshot each rig's previous final pose (`lastPose`) — the physics handoff needs
per-bone velocities (finite-differenced) to avoid a pop when a ragdoll takes over. Cheap to keep now,
load-bearing later. (Phase 8's previous-frame palette is the rendering analogue; this is the gameplay one.)

## Validation (done criteria)

- `make engine` + `make prepare-for-commit` clean; the IK solver's unit test passes (exact reach in range,
  graceful clamp out of range).
- Manual: a walking/idle clip on a rig with foot chains keeps feet planted on a raised/lowered ground plane
  instead of clipping/floating — the visible "reacts to the environment" result.
- `make e2e`: toggle foot-IK on/off over the control plane and assert a foot bone's world Y tracks the
  ground target when enabled.
- `docs/`: add `docs/content/explanations/animation/foot-ik-and-physics-ahead.md` — the blend-layer
  producer model, two-bone IK, and the reserved metadata as the Jolt on-ramp (cite UE Physical Animation
  Component / Physics Blend Weight as the destination).

## Notes / gotchas

- Without a physics/collision system, "ground" is limited — scope v1 to a ground plane or a single
  designated mesh raycast and **say so** (the README defers true terrain following to the physics work).
  The value of this phase is *architectural validation* + *the env-reactive taste*, not full locomotion.
- The blend layer must remain the **only** way external pose producers reach the bones — resist letting IK
  write bone `TransformComponent`s directly; that would re-create the trap the layer exists to avoid.
- Deferred beyond this phase (all wait for Jolt): powered/passive ragdoll (`DriveToPoseUsingMotors` reading
  the reserved PD gains), hit reactions (localized weight ramp + impulse), `SkeletonMapper` (coarse ragdoll
  ↔ render rig), motion warping, and motion matching. Each is a producer into — or a consumer of — this
  same pose path.
