# Inspector field exposure

**Status:** IN PROGRESS

Analysis of what every inspectable component *should* expose in the editor Inspector,
versus how it renders today. The driver: animation/model/skinning components render their
uuid references as raw uint64 digit boxes, their derived arrays/matrices as `JSON.stringify`
blobs, and their enums as free-text inputs.

## Done (editor-only pass)

All edits are in `editor/` (TS only; no engine/DTO changes — the fix was entirely on the
data-driven render side). Verified with `bun run check` (tsc) + `bun run lint` clean.

- **Material AO drift bug fixed** — `FIELD_HINTS` keyed the stale `Material.ormTexture`; the
  wire field is `occlusionTexture`. Renamed → fixes the AO map in the Material grid *and*
  every MaterialSet slot. Added the four missing shared hints (`normalStrength`,
  `heightScale`, `alphaClip`, `alphaCutoff`).
- **`model` + `animation` AssetKinds** added to `AssetKind`/`PickerAssetKind` (the catalog
  already carries those `type` strings). `ModelInstance.modelId` → model picker;
  `AnimationPlayer.clip` → clip picker. No more raw-uint64 boxes for either.
- **AnimationPlayer** fully hinted: `wrap` and `transitionMode` are now Selects (exact wire
  strings `once|loop|pingpong` / `crossfade|inertialize`), `loopBlend` a 0..1 slider,
  `speed` a tuned NumberDrag, `clip` the picker.
- **`COMPONENT_ORDER`** gained `ModelInstance`, `SkinnedMesh`, `AnimationPlayer`, `FootIk`,
  `MaterialAsset` (no longer the unordered "extra" tail). Addability/removability gated:
  `ModelInstance`/`SkinnedMesh` are non-addable + non-removable (import identity); the rig
  sidecars + `AnimationPlayer`/`FootIk` are skinned-only adds (`RIG_ONLY`).
- **Read-only rig bodies** (`SkinnedMesh`, `FootIk`, `KinematicBones`): import-derived data
  resolved to names client-side — mesh/root-bone by catalog/entity name, IK chains and
  driven set by joint name, a joint count in place of the inverse-bind matrices. No more
  JSON blobs. Editable scalars (`FootIk.enabled`/`groundHeight`, `KinematicBones.enabled`)
  stay live.
- **Camera** `frustumMaxDistance` clamped ≥0; `showModel`/`showFrustum` hinted. Slider polish
  on `DirectionalLight.ambient` and `ReflectionProbe.intensity`.
- Docs updated: `docs/content/explanations/ui-and-editor/inspector.md`.

## Deferred (need engine work or larger widgets — documented below)

- **Editable rig arrays** — adding/removing/reordering `FootIk.chains`, picking
  `KinematicBones.driven`, tuning per-bone `BonePhysics.bones`. Authoring would ride the
  generic `set-component` (round-trips today, consumed at play), but needs a struct-list /
  bone-mask widget + a bone-picker; left read-only for now.
- **Conditional `disabledWhen`/`visibleWhen`** greying (systemic #9) — needs `renderField` to
  receive the whole DTO.
- **AnimationPlayer.time** as a read-only playhead chip, and `ModelInstance.modelId` as a
  read-only resolved-model chip (currently an editable picker) — both want a read-only
  display FieldKind (systemic #5).
- Engine CLI parity (`set-foot-ik` chains / `set-kinematic-bones` driven), Script field-type
  extensions (systemic #10/#12). Note: hiding `SkinnedMesh.inverseBind` is **editor-side**
  (the structured body omits it) — it must stay serialized, since the same serde feeds
  project save/load.

---

## TL;DR — the root cause

The inspector is entirely data-driven: a field renders via `` FIELD_HINTS[`${component}.${field}`] `` if a hint exists, otherwise it falls to `resolveHint → inferKind(value)`, which keys *only* on value shape (`{x,y,z,w}→vec4`, `{x,y,z}→vec3`, `number→NumberDrag`, `boolean→Switch`, everything else→`text`). The physics and light components were given a full parity table of hints; the animation/model/skinning components — **ModelInstance, SkinnedMesh, AnimationPlayer, FootIk, KinematicBones**, and the unfinished **BonePhysics** — never were, and they are also missing from `COMPONENT_ORDER`. Because of that, their uuid references (`modelId`, `clip`, `mesh`, `rootBone`) render as raw, hand-editable uint64 digit boxes; their derived arrays/matrices (`bones`, `inverseBind`, `driven`, `chains`, `BonePhysics.bones`) dump as a single `JSON.stringify` string; and their enums (`wrap`, `transitionMode`) render as free-text inputs that silently accept typos. The systemic capability gaps behind this are: **no `model`/`animation` AssetKind**, **no read-only / resolved-id-chip FieldKind**, **no entity-reference FieldKind**, **no array / struct-list presentation**, **no engine-driven enum-options channel** (every enum is hand-mirrored), **no conditional `disabledWhen`/`visibleWhen`**, and a **hand-maintained parity table that drifts** (the live `Material.ormTexture` → `occlusionTexture` rename is already broken).

## Systemic fixes (do these once, many components benefit)

Prioritized by reach-per-effort.

1. **Fix the `Material.ormTexture` → `occlusionTexture` drift bug** *(editor-only, trivial)*. The hint table keys a stale name that no longer exists in the serde, so the AO map renders as a raw uint64 text box. Renaming the key to `Material.occlusionTexture` {kind:`uuid`, asset:`texture`} fixes it in **both** the `Material` grid **and** every `MaterialSet` slot (which routes through `renderField("Material", …)`). Highest value-to-effort item in the whole report.

2. **Add the five missing shared `Material.*` hints** *(editor-only hints, trivial)*: `occlusionTexture` (uuid/texture), `normalStrength` (number min 0 max ~4 step 0.01), `heightScale` (number min 0 max ~0.5 step 0.005), `alphaClip` (bool), `alphaCutoff` (slider 0..1 step 0.01). Unblocks **Material** and **MaterialSet** in one stroke.

3. **Add `ModelInstance`, `SkinnedMesh`, `AnimationPlayer`, `FootIk` to `COMPONENT_ORDER`** *(editor-only, trivial)* so they stop rendering as the unordered "extra" tail. Suggested slots: `ModelInstance` near the top (after `Name`/`Transform`, before `Mesh`, as the instance identity badge); `SkinnedMesh` right after `Mesh`; `AnimationPlayer` in the rig/mesh cluster; `FootIk` in the rig-only set with `KinematicBones`/`BonePhysics`. Also add `MaterialAsset` immediately **before** `Material` so the precedence story reads top-down.

4. **Add a `model` AssetKind and an `animation`/`clip` AssetKind to AssetPicker** *(editor-only + small picker change)*. The catalog already tags `.smodel` containers with `type:"model"` on the wire — the picker just refuses to filter to it; an animation `list-clips` command also already exists. Extend AssetPicker's `assetType` union (`mesh|texture|material`) to include `model` and `animation`, plus the drag-drop MIME match. Unblocks **ModelInstance.modelId** (model picker) and **AnimationPlayer.clip** (clip picker), killing two of the loudest raw-uint64 boxes.

5. **Add a read-only / derived-display FieldKind** *(new widget)*: a non-editable readout that resolves a uuid against the catalog to a **thumbnail + display-name chip**. Today the only read-only bodies are hand-coded BonePhysics/Collider exceptions. Unblocks the correct end-state for **ModelInstance.modelId**, **SkinnedMesh.mesh**, and **AnimationPlayer.time** (a `t / duration` progress chip).

6. **Add an entity-reference FieldKind** *(new widget)*: resolve a *scene-entity* uuid (not a catalog asset) to its `Name` via the store, render a name chip with a `(root)` case for `"0"` and click-to-select-in-Hierarchy. Unblocks **SkinnedMesh.rootBone** and **Relationship.parent** (read-only), and is the substrate for the FootIk/KinematicBones bone pickers.

7. **Add an `array` / struct-list FieldKind** *(new widget, larger)*: a collapsible list of per-element cards rendered via the existing `struct` recursion, with an explicit *fixed-length* mode (no add/remove/reorder) for import-derived rigs and an editable mode for authored lists. Unblocks **BonePhysics.bones** (fixed-length, per-bone ragdoll cards), **FootIk.chains** (editable add/remove cards), **KinematicBones.driven** (bone-mask multi-select), and lets **SkinnedMesh.bones** become a read-only count/name list. This single capability retires four `JSON.stringify` text boxes.

8. **Add an engine-reflection channel for skeleton joint names** *(engine change)*: ship the selected rig's joint **index → name** list to the inspector (SkinnedMesh stores Uuids only, no names on the wire). Required so **BonePhysics**, **FootIk.chains**, and **KinematicBones.driven** can render bones by name instead of bare integer indices.

9. **Add conditional `disabledWhen` / `visibleWhen` predicates to FieldHint** *(new capability, keyed on a sibling field's wire value)*. `renderField` must receive the whole component DTO, not just one field value, to evaluate it. Unblocks greying solver-irrelevant **Rigidbody** fields when `motion != dynamic`, **Material.alphaCutoff** gated on `alphaClip`, **Material/heightScale** on `heightTexture`, **ReflectionProbe.boxExtent** on `boxProjection`, **Collider.sourceMesh** on `shape ∈ {convexhull,mesh}`, and **AnimationPlayer.loopBlend** on `wrap == loop`.

10. **Add an engine-driven enum-options channel** *(engine reflection, optional but kills the drift)*. Today every enum (`Rigidbody.motion`, `Collider.shape`, `AnimationPlayer.wrap`/`transitionMode`, `BonePhysics.joint`) must be hand-mirrored in `FIELD_HINTS` or it renders as free-text. Until this lands, hand-author the missing option lists for **AnimationPlayer.wrap** (`once|loop|pingpong`) and **AnimationPlayer.transitionMode** (`crossfade|inertialize`).

11. **Stop surfacing derived matrices / mark fields hidden** *(engine serde or per-component body)*. **SkinnedMesh.inverseBind** (array of flat 16-float mat4s) is machine-computed bind-pose math no one hand-edits; omit it from the structured body (or drop it from `inspect`). A general matrix/matrix-array widget is not worth building.

12. **Extend control-plane write paths** *(engine commands)* for the newly-editable arrays: `set-foot-ik` currently writes only `enabled`+`groundHeight` (no `chains` path at all); `set-kinematic-bones` has no `driven` param. Either extend those or route the array edits through the generic component patch.

## Per-component recommendations

### ModelInstance — **bad**
Headline: add to `COMPONENT_ORDER`, add a `model` AssetKind, and present `modelId` as a read-only resolved-model chip (it is engine-authored provenance, not a knob).

| Field | Wire type | Today | Should be | Why |
|---|---|---|---|---|
| `modelId` | string(uuid) | Raw editable uint64 digit box (`inferKind→text`) | Read-only resolved-model chip (thumbnail + `.smodel` name); optional explicit "Replace source model…" action backed by a **model** AssetPicker | Set once by `instantiateModel`, consumed by reimport; a fat-finger silently dangles the link. Interim minimum: `FIELD_HINTS["ModelInstance.modelId"]={kind:"uuid",asset:"model"}` so it at least becomes a thumbnail picker, not a digit box. |

### SkinnedMesh — **bad**
Headline: add after `Mesh` in `COMPONENT_ORDER`; this is import-derived rig data, so give it a structured body (like BonePhysics), not a flat grid.

| Field | Wire type | Today | Should be | Why |
|---|---|---|---|---|
| `mesh` | string(uuid), `0`=none | Raw uint64 text box | Read-only resolved asset-name chip + thumbnail (or a `mesh` AssetPicker if re-pointing is allowed) | Co-derived with `bones`/`inverseBind`/`rootBone` at this exact import; swapping it alone desyncs the rig — prefer read-only. |
| `rootBone` | string(uuid) — joint **entity** id | Raw uint64 text box | Read-only entity-name chip (resolve via `findEntityByUuid`, click-to-select) — **not** an AssetPicker | It's an intra-scene entity reference, not a catalog asset; the picker can never resolve it. |
| `bones` | array of string(uuid) — ordered joint entity ids | Whole array dumped as one `JSON.stringify` text box | Read-only `N joints (glTF import order)` count readout; optional expandable resolved-name list | Element order is load-bearing (`bones[i] ↔ inverseBind[i] ↔ jointMatrices()[i]`); reordering corrupts skinning. No authoring intent. |
| `inverseBind` | matrix[] — array of flat 16-float mat4s | Massive one-line JSON blob, editable | **Hidden** — do not render | Machine-computed bind math; no one hand-edits a column-major bind matrix. |

### AnimationPlayer — **bad**
Headline: add to `COMPONENT_ORDER` + hints; needs the `animation`/`clip` AssetKind, two enum option lists, and a read-only playhead readout that defers to the Timeline panel.

| Field | Wire type | Today | Should be | Why |
|---|---|---|---|---|
| `clip` | string(uuid), `0`=none | Raw uint64 text box | **clip** AssetPicker filtered to `AssetType::Animation`, showing resolved name + duration | The component's primary authored field; pick by thumbnail/name, never by typing 64 bits. Catalog + `list-clips` already exist. |
| `time` | number | Unbounded editable NumberDrag | Read-only `t = 1.92s / 3.40s` progress chip; editing belongs to the Timeline transport / `seek-animation` | Runtime state advanced by the game loop; a raw drag fights the poll and the seek-blend easing. |
| `speed` | number | Unbounded NumberDrag | NumberDrag step 0.05, soft floor (signed if reverse playback is in scope) | Legitimate authored multiplier (default 1); just needs a tuned step/guard. |
| `wrap` | string(enum) | Free-text Input (typos accepted, engine falls back to Loop) | EnumField `[once→Once, loop→Loop, pingpong→Ping pong]` | Closed authored enum; the canonical free-text-enum gap. |
| `playing` | bool | Switch (blind component patch) | Switch routed through `set-animation-playing`, kept in sync with the Timeline transport | Discrete intent is fine to flip here, but the raw write gets clobbered by the ~6 Hz poll. |
| `transitionMode` | string(enum) | Free-text Input (falls back to Inertialize) | EnumField `[crossfade→Cross fade, inertialize→Inertialize]` | Same closed-enum gap as `wrap`. |
| `loopBlend` | number | Unbounded NumberDrag | Slider min 0 max 1 step 0.01; greyed when `wrap != loop` | Sub-second non-negative blend; only meaningful for Loop wrap. |

### FootIk — **bad**
Headline: add to `COMPONENT_ORDER` + the rig-only set; give it a structured body; `chains` needs the array/struct-list FieldKind + a bone picker + a control-plane write path. Interim: render `chains` as a read-only `N chains` summary, not the corrupt JSON box.

| Field | Wire type | Today | Should be | Why |
|---|---|---|---|---|
| `enabled` | bool | Switch (by inference) | Switch + explicit `FootIk.enabled:{kind:"bool"}` hint | Master on/off; already command-drivable via `set-foot-ik`. Make it contractual. |
| `groundHeight` | number | Unbounded NumberDrag | NumberDrag step 0.01, world-Y label, **signed** (no clamp); greyed when `!enabled` | v1 ground plane Y can be negative, so a slider is wrong. |
| `chains` | array of `{upper,mid,end,poleVector}` | Whole list as one `JSON.stringify` box | Struct-array editor: chain cards with Add/Remove/reorder; per card three **bone-name dropdowns** (index↔name, `(none)`=-1) + a vec3 pole-vector editor | The substance of the component; raw i32 indices are meaningless and the JSON blob invites corruption. Needs the joint-name reflection channel and a `chains` command (none exists today). |

### BonePhysics — **partial**
Headline: replace the hand-coded read-only "N bone bodies" escape hatch with a real fixed-length `array` FieldKind so per-bone ragdoll data becomes authorable; label each row by skeleton bone name.

| Field | Wire type | Today | Should be | Why |
|---|---|---|---|---|
| `bones` | array of per-bone structs | Hand-coded read-only `"{count} bone bodies (auto-fit on import)"` line; all per-bone data invisible | Fixed-length (no add/remove/reorder) collapsible list of struct cards, each labeled by bone name: `shapeHalfExtents`→vec3 (min 0), `mass`→NumberDrag (min 0, kg), `joint`→EnumField `[fixed,hinge,swingtwist,free]`, `swingTwistLimits`→vec3 **deg + convertRadians**, `driveStiffness`/`driveDamping`/`driveMaxForce`→NumberDrag (min 0) | The whole authoring surface (capsule sizing, joint type, limits, PD gains) feeds the Jolt ragdoll build; today none is editable. Length is skeleton-owned (positionally 1:1 with `SkinnedMesh.bones[i]`), so fixed-length. `swingTwistLimits` is radians on the wire — same degree treatment as `Transform.rotation`. Runtime ragdoll controls correctly stay in the Physics panel. |

### KinematicBones — **partial**
Headline: `enabled` is fine; `driven` must move off the JSON text box to a bone-mask multi-select (with an "All joints" master state). Interim: read-only `N of M joints driven (all)` chip.

| Field | Wire type | Today | Should be | Why |
|---|---|---|---|---|
| `enabled` | bool | Switch (hinted) ✓ | Switch | Mirrors `set-kinematic-bones {enabled?}`. Correct. |
| `driven` | array of i32 (joint indices; `[]`=all) | Whole list as `JSON.stringify` box | Bone-subset multi-select (checklist keyed by joint name, value=index) with an explicit "All joints" toggle mapping to the empty array | Indices reference `SkinnedMesh.bones`; bad indices silently no-op, and `[]`=all is invisible in a raw box. Needs the joint-name channel + array FieldKind; `set-kinematic-bones` has no `driven` param yet. |

### Relationship — **bad** (keep hidden)
Headline: keep it in `HIDDEN_COMPONENTS`; parentage is edited in the Hierarchy tree via the guarded `set-parent` path. If ever shown, `parent` must be a read-only resolved-name chip, never editable.

| Field | Wire type | Today | Should be | Why |
|---|---|---|---|---|
| `parent` | string(uuid), `0`=root | Not rendered (hidden); if un-hidden → raw uint64 text box | Read-only entity-ref chip (resolved Name, `(root)` for `0`, click-to-select); reparenting stays exclusively on `set-parent` | An editable id box would bypass `set-parent`'s self/cycle/dangling guards and the world-preserving rebase — a corruption vector and a forbidden duplicate path. Runtime caches `parentHandle`/`children` never hit the wire. |

### MaterialSet — **partial**
Headline: keep the bespoke per-slot "Slot N" structured body (the array shell is correct); the only real bugs are the four missing shared `Material.*` slot-key hints (see systemic #1–2).

| Field | Wire type | Today | Should be | Why |
|---|---|---|---|---|
| `slots` | array of MaterialSlot | Bespoke per-slot cards; per-slot keys reuse `Material` hints **except** `occlusionTexture` (raw id box), `normalStrength`/`heightScale`/`alphaCutoff` (unbounded drags), `alphaClip` (Switch by luck) | Keep the per-slot card body; fix the four/five missing `Material.*` hints. Array is read-structured (no add/remove/reorder — slots are import-derived, indexed by `Submesh.materialSlot`); each element is an editable Material sub-form | Users genuinely tune per-slot PBR on multi-material meshes; the missing hints are the same bugs as the Material grid. Don't add `FIELD_HINTS["MaterialSet.slots"]` — the bespoke body intercepts before `resolveHint`. Optional polish: source-material/submesh name in the card header instead of a bare ordinal; collapsible cards. |

### Material — **partial**
Headline: rename the `ormTexture` drift key and add the five tail hints (systemic #1–2); two fields want conditional visibility.

| Field | Wire type | Today | Should be | Why |
|---|---|---|---|---|
| `baseColor` | vec4 | ColorField (color4) ✓ | Keep | Alpha is meaningful for blend/clip. |
| `albedoTexture`/`metallicRoughnessTexture`/`normalTexture`/`emissiveTexture`/`heightTexture` | string(uuid) | texture AssetPicker ✓ | Keep | The model the broken slots should copy. |
| `metallic`/`roughness` | number | SliderField 0..1 ✓ | Keep | Bounded factors. |
| `emissive` | vec3 | ColorField (color3) ✓ | Keep | Pairs with `emissiveStrength`. |
| `emissiveStrength` | number | NumberDrag 0..100 ✓ | Keep (prefer soft cap; allow type-in >100) | HDR radiance multiplier. |
| `unlit` | bool | Switch ✓ | Keep | PSO mode. |
| `occlusionTexture` | string(uuid) | **BROKEN: raw uint64 box** (table keys stale `ormTexture`) | texture AssetPicker — rename hint key | The concrete "ids as raw uint64 text" complaint. |
| `normalStrength` | number | Unbounded NumberDrag (fallback) | Slider/number min 0 max ~2 step 0.01 | Normal-map intensity (default 1). |
| `heightScale` | number | Unbounded NumberDrag, too-coarse step | number min 0 max ~0.5 step 0.001; greyed when `heightTexture==0` | POM scale (default 0.05); needs fine step. |
| `alphaClip` | bool | Switch (by luck) | Switch + explicit hint; gates `alphaCutoff` | Make intent explicit. |
| `alphaCutoff` | number | Unbounded NumberDrag (no 0..1 clamp) | Slider 0..1 step 0.01; greyed unless `alphaClip` | Mask threshold (default 0.5). |

> Note: `uvTiling`/`uvOffset` exist in the struct but are **not serialized** — do **not** add hints; if wanted, add to `emitSceneSerde()` first.

### Camera — **partial**
Headline: three editor-gizmo fields are unhinted; only `frustumMaxDistance` is an actual defect (unbounded, goes negative).

| Field | Wire type | Today | Should be | Why |
|---|---|---|---|---|
| `fov` | number | NumberDrag 1..179 step 0.5 ✓ | Keep (degrees on wire — **no** convertRadians) | Adding convertRadians would be the 57× bug. |
| `near`/`far` | number | NumberDrag (min 0.001 / min 0.1) ✓ | Keep | Clip planes. |
| `primary` | bool | Switch ✓ | Keep | First-primary-wins. |
| `showModel`/`showFrustum` | bool | Switch (by inference) | Switch + explicit hints | Drift safety; widget already right. |
| `frustumMaxDistance` | number | **Unbounded NumberDrag, scrubs negative** | NumberDrag min 0 step 0.5 | The one real defect; editor-only overlay extent, never negative. |

> Optional cosmetic: split into "Projection" vs "Editor gizmo" groups.

### MaterialAsset — **good**
Headline: widget is correct; just insert into `COMPONENT_ORDER` **before** `Material`.

| Field | Wire type | Today | Should be | Why |
|---|---|---|---|---|
| `material` | string(uuid), `0`=none | material AssetPicker ✓ | Keep; empty state should read `(none — default material)` | Shared `.smat` reference; clearing falls back to the built-in default — surface that. |

### Script — **good** (do not touch)
Headline: owned by the bespoke `ScriptSlots.tsx` body; adding a `FIELD_HINTS` entry would regress it to the JSON fallback. The `scripts: ScriptSlot[]` field renders a full slot-list editor with live `get-script-schema` per-field widgets, override-vs-default state, and `set-script-override` writes. The only remaining work is **engine-side**: extend `ScriptFieldDto`/`ScriptFieldType` to carry numeric range+step (so the per-field NumberDrag can become a clamped slider), an enum field type (options list → EnumField), and optionally an asset/entity-reference field type.

### DirectionalLight — **good**
Headline: fully hinted; two optional polish items.

| Field | Wire type | Today | Should be | Why |
|---|---|---|---|---|
| `direction` | vec3 | VectorEditor ✓ | Keep; optional spherical az/el "direction" control | Users think "where in the sky," not signed cartesian. |
| `color` | vec3 | ColorField ✓ | Keep | Canonical color3. |
| `intensity` | number | NumberDrag 0..50 ✓ | Keep | Open-ended brightness. |
| `ambient` | number | NumberDrag 0..1 | **SliderField** 0..1 step 0.01 (flip `kind:"number"`→`"slider"`) | Hard-bounded 0..1 fraction; matches metallic/roughness/damping. |

### PointLight — **good**
All three fields (`color` ColorField, `intensity` NumberDrag 0..100, `range` NumberDrag 0..200) are hinted and correct. Only non-inspector nit: a viewport falloff-radius gizmo for `range` (engine overlay, not a field widget).

### SpotLight — **good**
Fully hinted; `unit:"deg"` on the angles is correctly label/clamp-only (no radians conversion — no 57× trap). Nits: promote bounded scalars (`intensity`, `innerAngle`, `outerAngle`) NumberDrag→SliderField; add a `direction` normalize-on-commit; and the one genuinely new ask — **relational bounds** so `innerAngle ≤ outerAngle` (FIELD_HINTS only supports static literal min/max today).

### ReflectionProbe — **good**
Fully hinted; runtime `dirty` correctly never serialized. Nits: `intensity` → SliderField 0..8; `boxExtent` should be greyed when `boxProjection` is false (conditional-disable gap). `influenceRadius` should stay NumberDrag (0.1..500 is too wide for a slider track).

### Rigidbody — **good**
Fully hinted, all eight fields correct (`motion`/`collisionLayer` EnumField, damping sliders, lock grids, `mass`/`gravityFactor` NumberDrag). The lockAxes hints are *essential* — without them the `bvec3 {x,y,z:false}` shape mis-routes to a numeric VectorEditor. The single outstanding item is the **`disabledWhen` predicate** (systemic #9): grey `mass`/`linearDamping`/`angularDamping`/`gravityFactor`/`lockPosition`/`lockRotation` when `motion != dynamic`, while `motion` and `collisionLayer` stay enabled. Optional: `gravityFactor` → SliderField 0..2.

### Collider — **good**
Best-served physics component: in order, fully hinted, with a hand-coded structured body (Fit-to-mesh button + static-body note). Two second-order gaps, both needing conditional/shape-aware presentation: (1) `halfExtents` is a flat vec3 but its meaning is shape-dependent (box half-size vs sphere radius in `.x` vs capsule radius/half-height) — relabel/reshape per `shape`; (2) `sourceMesh` (mesh AssetPicker, already correct) should hide/grey unless `shape ∈ {convexhull,mesh}`. Optional: an `isSensor` tooltip noting it forces the Sensor object layer.

### CharacterController — **good**
The model the other physics components were patterned on: in order, fully hinted, and `maxSlopeAngle` already gets the scalar degrees↔radians conversion (`unit:"deg"`, `convertRadians:true`) — do **not** regress it to a raw radians box. The only nits: `gravityFactor` → SliderField (it already carries min+max, the slider signal); optionally `maxSpeed`/`maxStepHeight` → bounded sliders; and unit tooltips. Runtime fields (`desiredVelocity`/`verticalVelocity`/`onGround`) correctly never serialize.

### Name & Transform — **good**
The baseline. **Name**: single `text` field, hinted, correct; `removable=false` should suppress any remove affordance; minor polish — placeholder `"Unnamed entity"` and commit-on-Enter. **Transform**: three vec3 rows; `rotation` correctly edits in degrees while the wire stays radians (the documented 57× guard); `removable=false`. The runtime `WorldTransformComponent` mat4 is intentionally unregistered and must never become an editable JSON box. Optional: per-field reset-to-default and a uniform-scale lock.

### Mesh — **good**
The positive reference: `mesh` is hinted `{kind:"uuid", asset:"mesh"}` and routes to the mesh AssetPicker. Nothing to change — this is the hint pattern the broken components should copy.

## Suggested edit map

- **`editor/src/components/fieldRenderer.tsx` — `FIELD_HINTS`:**
  - Rename the stale `"Material.ormTexture"` key → `"Material.occlusionTexture"` {kind:`uuid`, asset:`texture`} (fixes Material **and** MaterialSet slots).
  - Add `"Material.normalStrength"`, `"Material.heightScale"`, `"Material.alphaClip"`, `"Material.alphaCutoff"`.
  - Add `"Camera.showModel"`/`"Camera.showFrustum"` {kind:`bool`} and `"Camera.frustumMaxDistance"` {kind:`number`, min:0, step:0.5}.
  - Flip `"DirectionalLight.ambient"` and `"ReflectionProbe.intensity"` (and optionally `CharacterController.gravityFactor`, `SpotLight.intensity/innerAngle/outerAngle`, `Rigidbody.gravityFactor`) from `kind:"number"`→`kind:"slider"`.
  - Add the AnimationPlayer block: `"AnimationPlayer.clip"` {uuid, asset:`animation`}, `.wrap`/`.transitionMode` EnumField option lists, `.speed` step, `.loopBlend` slider 0..1, `.time` readonly.
  - Add `"FootIk.enabled"` {bool}, `"FootIk.groundHeight"` {number, step:0.01}.
  - Add `"ModelInstance.modelId"` {uuid, asset:`model`} (interim before the read-only chip).
  - Add `"BonePhysics.bones"` declaring the array element schema (struct sub-fields incl. `swingTwistLimits` deg+convertRadians).
- **`editor/src/components/fieldRenderer.tsx` — kinds & AssetKind union:** extend `AssetKind` from `mesh|texture|material` to add `model` and `animation`; add new FieldKinds `readonly`/id-chip, `entityRef`, `array`/struct-list, and the `disabledWhen`/`visibleWhen` predicate channel (`renderField` must take the whole component DTO).
- **`editor/src/lib/componentOrder.ts` — `COMPONENT_ORDER`:** insert `MaterialAsset` before `Material`; add `ModelInstance` (high, near identity), `SkinnedMesh` (after `Mesh`), `AnimationPlayer` (rig/mesh cluster), `FootIk` (rig-only). Keep `Relationship` in `HIDDEN_COMPONENTS`; add `FootIk` to the `RIG_ONLY` set with `KinematicBones`/`BonePhysics`.
- **`editor/src/components/AssetPicker.tsx`:** widen the `assetType` filter + `application/x-se-asset` drop MIME match to `model` and `animation`; resolve `type:"model"`/`AssetType::Animation` catalog entries.
- **New widgets under `editor/src/components/`:** a read-only resolved-id chip (catalog thumbnail+name), an entity-reference chip (store name lookup + click-to-select-in-Hierarchy), and an array/struct-list card list (fixed-length and editable modes). The fixed-length list backs `BonePhysics.bones`, `SkinnedMesh.bones`; the editable mode backs `FootIk.chains`; the multi-select mode backs `KinematicBones.driven`.
- **`editor/src/panels/InspectorPanel.tsx`:** give `SkinnedMesh`, `FootIk`, `KinematicBones` structured bodies (mesh chip + rootBone entity chip + joint-count readout for SkinnedMesh; enabled+groundHeight+chain-cards for FootIk; enabled+driven-mask for KinematicBones). When the real `array` FieldKind lands, **delete** the hand-coded `BonePhysics.bones` read-only escape hatch (~lines 505–518) and route through the generic renderer. Keep the existing `MaterialSet` and `Script` (`ScriptSlots`) bodies untouched.
- **`editor/src/state/store.ts` / `editor/src/control/client.ts`:** wire `AnimationPlayer.playing` through `set-animation-playing` (not a blind component patch); a uuid→entity-name resolver for the entity-ref chip (entity list already in the store).
- **Engine serde — edit `scene.cppm` `to/fromJson`, NOT the generated `scene_component_serde.generated.cpp`** (it is regenerated): mark/omit `SkinnedMesh.inverseBind` from `inspect` (or hide it in the body); if UV tiling is ever wanted, add `uvTiling`/`uvOffset` to `emitSceneSerde()` *before* hinting.
- **Engine reflection / new control channels:** a skeleton joint index→name list reaching the inspector (for BonePhysics, FootIk.chains, KinematicBones.driven); optionally an engine-driven enum-options channel (retires the hand-mirrored `wrap`/`transitionMode`/`shape`/`motion`/`joint` lists); extend `set-foot-ik` to write `chains` (or add `add/remove/set-foot-chain`) and `set-kinematic-bones` to write `driven`; and the Script-side `ScriptFieldDto`/`ScriptFieldType` extensions (numeric range, enum, asset/entity field types) in `engine/source/saffron/script/script.cppm` + `control_dto.cppm`.
