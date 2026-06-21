+++
title = 'Physics inspector'
weight = 7
+++

# Physics inspector

The [Inspector](../inspector/) edits physics the same way it edits everything else — data-driven, no per-component code — but physics carries three field shapes the generic widget set could not draw: an enum (a rigidbody's motion type, a collider's shape), a per-axis boolean lock (a `BVec3`), and a nested struct (a collider's physics material). Each becomes a new generic `FieldKind`, so the rigidbody, collider, character controller, and kinematic-bones components author cleanly from the same machinery the rest of the components use, never a bespoke physics panel.

The model is Unity's and Godot's **split body/shape**: a `Rigidbody` owns motion (type, mass, damping, gravity factor, axis locks, layer), a `Collider` owns geometry plus its physics material and sensor flag, and a collider with **no** rigidbody is an implicit static body. The Add-Component menu lists Rigidbody, Collider, and Character Controller on any entity; the two rig sidecars (Kinematic Bones, Bone Physics) appear only on a skinned entity, since they index its skeleton.

## The three new widgets

`renderField` resolves a field to a widget from the `FIELD_HINTS` table, then dispatches by kind through `renderByHint` — factored out so a struct can render its sub-fields without re-keying. Physics adds three kinds:

- **`enum`** — a dropdown (`EnumField`, a shadcn `Select`) over a hint's option list. The wire value is the lowercase string the serde emits (`"static"`, `"box"`); the menu shows a Sentence-case label. Used for `Rigidbody.motion` and `Collider.shape`. A `numeric` enum variant backs `Rigidbody.collisionLayer`, whose wire value is an integer slot index: the dropdown names the fixed moving-slot mapping (0 = Moving, 1 = Character, 2 = Debris; Static and Sensor derive from the motion type and sensor flag, so they are not offered).
- **`lockAxes`** — an X / Y / Z toggle grid (`LockAxesField`) for a `{x,y,z}` boolean, tinted with the viewport gizmo's axis colors (X red, Y green, Z blue). This is Unity's *Freeze Position / Rotation* constraint grid and Godot's axis lock, over the bvec3 the wire carries. Used for `Rigidbody.lockPosition` and `lockRotation`.
- **`struct`** — a bordered card that renders each sub-field of a nested object via its own hint. `Collider.material` draws `friction` and `restitution` as 0–1 sliders inside it.

Every physics write rides the existing **read-modify-write** path: a single-field edit (including a nested material sub-field or one axis lock) rebuilds the full component DTO with that one value patched and sends the whole thing through `set-component`. The nested object stays whole, so editing friction never drops restitution.

One unit conversion lives at the widget boundary, same as `Transform.rotation`: `CharacterController.maxSlopeAngle` is radians on the wire but shown in degrees, driven by the hint's `convertRadians` flag (a scalar path, where rotation uses the vector one). A 45° slope reads as `45`, not `0.785`.

## Fit to mesh, and why there are no handles

Saffron's gizmo translates, rotates, and scales the entity transform — there is no interactive collider-resize handle. So a collider is sized **numerically** (`halfExtents`, `offset`) plus a **Fit to mesh** button on the Collider section that re-fits the shape to the entity's mesh AABB over the `fit-collider` command (the same auto-fit that runs when the collider is first added). The engine bumps the scene version, and the reconcile poll re-reads the fitted values. When the entity has no rigidbody, the Collider section shows a *"No Rigidbody — static body"* note, so the author knows why the object does not fall.

The Bone Physics section is read-only: its per-bone bodies are auto-fit on skinned import and its ragdoll blend is driven from the Physics panel, not edited as a field grid. Mass, damping, gravity factor, and locks are solver-ignored for static and kinematic bodies; the inspector still shows them (the generic grid does no conditional visibility) and the documentation notes it.

## Fields at a glance

**Rigidbody** — how a body moves under the solver:

| Field | Meaning |
|---|---|
| Motion | `Dynamic` moves under forces/gravity; `Kinematic` is moved only by script/animation and pushes dynamics (infinite mass, ignores gravity); `Static` never moves. |
| Mass (kg) | Inertia of a Dynamic body. Ignored for Static/Kinematic. |
| Linear / Angular damping | Per-second velocity / spin decay (drag). 0 = none, 1 = heavy. |
| Gravity factor | Scales world gravity on this body: 0 = floats, 1 = full, 2 = double. |
| Lock position / rotation X·Y·Z | Freezes that translation / rotation axis (Unity's Constraints). E.g. lock rotation X·Z to keep an upright character. |
| Collision layer | The moving slot the body lives in — `Moving`, `Character`, or `Debris` — which decides what it collides with (Debris ignores other Debris). Static/Sensor are derived from the motion type and the collider's sensor flag, not chosen here. |

**Collider** — the collision shape + surface:

| Field | Meaning |
|---|---|
| Shape | `Box` / `Sphere` / `Capsule` (analytic, sized from Half extents), or `Convex hull` / `Mesh` (cooked from a Source mesh). |
| Half extents | Half-size of the shape. Box: half-width per axis. Sphere: radius in X. Capsule: radius in X, half-height in Y. |
| Offset | Local-space shift of the shape from the entity origin. |
| Material — Friction | 0 = ice, 1 = rubber-grippy. |
| Material — Restitution | Bounciness: 0 = no bounce, 1 = perfectly elastic. |
| Is sensor | A trigger volume: reports overlaps (the contact feed) but is never solid — nothing collides with it. |
| Source mesh | The mesh cooked into a Convex hull / Mesh shape (ignored for Box/Sphere/Capsule). |

**Character Controller** — the walking-capsule movement params (a Jolt `CharacterVirtual`, not a rigid body):

| Field | Meaning |
|---|---|
| Max speed | Horizontal walk-speed cap (m/s); seeds the Physics-panel test slider. |
| Max slope angle | Steepest ground still treated as walkable floor (degrees in the UI, radians on the wire); steeper counts as a wall. |
| Max step height | Ledges/stairs up to this height are stepped over rather than blocking. |
| Gravity factor | Scales the gravity the controller integrates each step. |

## Code

| What | File | Symbols |
|---|---|---|
| The three new field kinds + dispatcher | `editor/src/components/fieldRenderer.tsx` | `FieldKind` (`enum`/`lockAxes`/`struct`), `renderByHint`, the physics `FIELD_HINTS` block |
| Enum + lock widgets | `editor/src/components/EnumField.tsx` · `LockAxesField.tsx` | `EnumField`, `LockAxesField` |
| Collider Fit-to-mesh + static note, Bone Physics readout | `editor/src/panels/InspectorPanel.tsx` | `componentBody` (`Collider`/`BonePhysics` bodies), `onFitCollider` |
| Addability + section order | `editor/src/lib/componentOrder.ts` · `InspectorPanel.tsx` | `COMPONENT_ORDER`, the `missing` memo + `RIG_ONLY` |
| Typed component shapes | `engine/crates/protocol/src/dto.rs` → (`xtask gen-protocol`) → `editor/src/protocol/sa-types.ts` | `Rigidbody`, `Collider`, `KinematicBones`, `CharacterController`, `BVec3`, `PhysicsMaterial` |
