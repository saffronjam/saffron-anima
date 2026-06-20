# Phase 7 — Control plane: morph + binding commands + channel metadata

**Status:** NOT STARTED

**Depends on:** Phase 4 (node-TRS runtime: node `PoseOverrideComponent` + mesh
`MorphWeightOverrideComponent` written by `tickAnimation`, durable name→Uuid binding) and
Phase 5 (GPU morph deform + `MorphComponent` durable / `MorphWeightOverrideComponent` runtime
application). This phase is the wire surface; it adds no runtime behaviour of its own — it
exposes what Phases 4 and 5 already compute.

## Why

Morph weights and a clip's channel makeup are new drivable/inspectable engine state, so each
gets a control command (AGENTS.md: state worth driving gets one `registerCommand`). Node-TRS
playback **reuses the existing player commands** — it is the same `AnimationPlayerComponent`,
so a duplicate playback verb would violate NO LEGACY / one-code-path. This phase also surfaces
per-channel metadata + binding status so the editor Timeline/Inspector (Phase 8) can drill into
bone/node/morph channels without a parallel UI. The hard gate is wire-contract cleanliness: the
control-schema contract test and `bun run check` must pass with all five generated files
git-diff-clean — enforced **now**, not deferred to Phase 9.

## Naming note (in-flight rebrand + plural command names)

- A `se` → `sa` rebrand (`plans/rebrand-anima`, "Saffron Engine" → "Saffron Anima") is in flight. The
  protocol-file rename has already landed (`editor/src/protocol/sa-types.ts`); other surfaces (the
  `Saffron.Control` namespace, the CLI under `tools/`, the Lua engine table) may still be mid-rename.
  Reference these **by role**, not by literal prefix; reach the protocol via the `../protocol` shim,
  never the generated file by name. Match whatever prefix the tree uses when this phase lands; do not
  fight the `plans/rebrand-anima` rename.
- Command names are **plural**: `set-morph-weights`, `get-morph-weights` — matching their DTOs
  (`SetMorphWeightsParams`/`GetMorphWeightsParams`/`MorphWeightsResult`), the Lua `setMorphWeights`
  binding, and the Phase 8 client wrappers, since the command carries the whole weights vector.
  `list-clip-bindings` rounds out the set.

## Grounding (real files/symbols)

- DTO source of truth: `engine/source/saffron/control/control_dto.cppm` —
  `AnimationClipDto` (~1457), `AnimationStateResult` (~1563), `PlayAnimationParams` (~1529),
  `SetRagdollParams`/`RagdollResult` (~571–586), `EntitySelector` (~26). Regex-parsed by
  `tools/gen-control-dto/gen.ts` (no `(`, `)`, `=` in a member line); field order = positional
  CLI arg order; IDs are `WireUuid` (decimal-string on the wire).
- Command registration: `engine/source/saffron/control/control_commands_animation.cpp`
  `registerAnimationCommands` (~208); helpers `playerOf` (~133), `footIkOf` (~171),
  `resolveClip` (~30), `stateOf` (~188); `animatableDescendant` is `scene.cppm:667`;
  `animationVersion` bump pattern (~302, ~346, ~360, ~508). Ragdoll mirror:
  `control_commands_physics.cpp` `set-ragdoll`/`get-ragdoll` (~330–385).
- gen.ts: command list (~402–598), `commandFixtures` (~967), `commandSkips` (~1069),
  `enumWireNames` triple-edit (~71, ~1264, ~2106). Every command needs a fixture **or** a skip.
- Editor: `editor/src/control/client.ts` typed wrappers (ragdoll ~235, animation ~275–315).
- CLI: `tools/sa/source/main.cpp` — generic dispatch (`coerce` ~40, positional/flag mapping
  ~86–122); verbs are the command name + positionals in DTO field order, no per-verb C++.
- Lua host bridge: `engine/source/saffron/script/script.cppm` `ScriptHostBridge` `std::function`
  members (`raycast`/`setRagdollBlend`/`ragdollState`/`logSink`, ~131–148);
  `script_runtime.cpp` `.addFunction(...)` registrations (~1006–1046) + the guarded call shape
  (~553–566). `Saffron.Script` must NOT import `Saffron.Animation`.

## Ordered steps

### 1. DTOs in `control_dto.cppm`

Place the new animation DTOs near `AnimationClipDto`/`AnimationStateResult` (~1457–1573); place
the ragdoll-mirroring morph params near the ragdoll block for symmetry.

1.1 `AnimationChannelDto` — per-channel metadata. **`kind` is a plain `std::string`, not a
gen.ts enum**, to dodge the fragile three-table enum edit (`enumWireNames` + `tsType` switch +
`jsonSchemaFor`). The runtime emits a stable lowercase token per channel target+path
(`"bone-translation"`, `"bone-rotation"`, `"bone-scale"`, `"node-translation"`,
`"node-rotation"`, `"node-scale"`, `"morph-weights"`).

```cpp
struct AnimationChannelDto
{
    std::string name;  // bone/node target name, or the mesh node name for a weights channel
    std::string kind;  // "bone-rotation" | "node-translation" | "morph-weights" | … (plain string, not an enum)
};
```

1.2 Extend `AnimationClipDto` (~1457) with an **additive** `channels` vector. Filled only for a
single-clip query; **empty** in the `list-clips` catalog and in `get-asset-model`'s clip list
(those stay cheap — the existing `tracks` count summarizes them; doc the "empty unless queried
singly" rule on the field).

```cpp
struct AnimationClipDto
{
    WireUuid id;
    std::string name;
    f32 duration;
    i32 tracks;
    std::vector<AnimationChannelDto> channels;  // per-channel makeup; empty in the catalog, filled for a single-clip query
};
```

1.3 `SetMorphWeightsParams` — mirror `SetRagdollParams`'s whole-vector / single-target dual
shape. `weights` sets the entire vector; `index`+`weight` set one target (matching `set-ragdoll`
`bone`+`weight`). All optional; a single-target write leaves the rest untouched.

```cpp
struct SetMorphWeightsParams
{
    EntitySelector entity;
    std::optional<std::vector<f32>> weights;  // whole-vector set (length must equal the mesh target count)
    std::optional<i32> index;                 // a single morph-target index to set (with `weight`)
    std::optional<f32> weight;                // the value for `index`
};
```

1.4 `GetMorphWeightsParams` / `MorphWeightsResult`.

```cpp
struct GetMorphWeightsParams
{
    EntitySelector entity;
};

struct MorphWeightsResult
{
    std::vector<f32> weights;        // live weights (override layer if present, else the durable MorphComponent)
    std::vector<std::string> names;  // per-target names from the mesh (parallel to weights), empty when unnamed
    i32 animationVersion;            // bumped by set-morph-weights so the reconcile poll can gate on it
};
```

1.5 `ListClipBindingsParams` / `ClipBindingsResult` / `ClipBindingDto` — node binding
inspection (the UE "Fix Actor References" analog). Node-name binding can go stale on
rename/reparent; this is the editor's read-only inspection path. A full `rebind-channel`
mutation is deferred (reimport regenerates bindings in this clean-slate engine); only the
inspection command ships in v1 so an unresolved binding is at least visible.

```cpp
struct ClipBindingDto
{
    std::string name;        // the channel's target name (bone/node/mesh-node)
    std::string kind;        // same token vocabulary as AnimationChannelDto.kind (plain string)
    std::string targetName;  // the resolved scene entity/bone name, empty when unresolved
    bool resolved;           // the durable name→Uuid binding found a live target
};

struct ListClipBindingsParams
{
    EntitySelector entity;
    AssetSelector clip;
};

struct ClipBindingsResult
{
    std::vector<ClipBindingDto> bindings;
};
```

1.6 Extend `AnimationStateResult` (~1563) with **additive** optional live morph weights, so the
existing `animationVersion`-gated reconcile poll carries them with no extra command. Optional →
**absent** on the wire for a non-morph entity (keeps the ~6 Hz poll small).

```cpp
struct AnimationStateResult
{
    WireUuid clip;
    std::string clipName;
    f32 duration;
    f32 time;
    bool playing;
    std::string wrap;
    f32 speed;
    i32 animationVersion;
    std::optional<std::vector<f32>> morphWeights;  // present only for a morph-bearing entity; live override-layer values
};
```

1.7 Add the matching `dtoToJson` / `parseDto` **forward declarations** at the bottom of
`control_dto.cppm` (~1877–2075, alongside the existing `RagdollResult`/`AnimationClipDto`/
`AnimationStateResult` declarations) for every new serialized type: `AnimationChannelDto`,
`MorphWeightsResult`, `ClipBindingDto`, `ClipBindingsResult`, and the three params parsers.
The bodies are emitted into `control_dto_serde.generated.cpp` by gen.ts — do not hand-write.

> `optional<vector<f32>>` + `vector<string>` must be supported by gen.ts's type mapper. Grep
> gen.ts for existing `vector<f32>`/`vector<string>` and for `optional` handling before writing
> the DTOs. If `optional<vector<...>>` is not yet handled, the generic mapper extension is part
> of step 3, exercised by a fixture.

### 2. Command implementations in `control_commands_animation.cpp`

Registered inside `registerAnimationCommands`. Reuse the existing idioms:
`resolveEntity(ctx, json{{"entity", selector.value}})` → `activeScene(ctx.sceneEdit)`;
`animatableDescendant(scene, *entity)` (`scene.cppm:667`) for rig resolution; bump
`ctx.sceneEdit.animationVersion += 1` on any mutating command.

2.1 Add a `morphableDescendant(scene, root)` helper in the file's anonymous namespace, mirroring
`animatableDescendant` but matching `MorphComponent` (Phase 3). The morph component can sit on a
different node than the rig in a skinned-morph model, so do **not** reuse `animatableDescendant`.

2.2 `set-morph-weights` (`registerCommand<SetMorphWeightsParams, MorphWeightsResult>`): resolve
`morphableDescendant`; `Err` if no `MorphComponent`. Write the live weights through the
**runtime override layer** (`MorphWeightOverrideComponent` from Phase 5) — the same path
`tickAnimation` writes — so the set is non-destructive (reverts on stop, like the pose
override). Validate: whole-vector `weights` length must equal the mesh target count (`Err` on
mismatch); a single `index` must be in range (`Err` otherwise); clamp values to `[0,1]`. Bump
`animationVersion`. Return the post-write `MorphWeightsResult`.

2.3 `get-morph-weights` (`registerCommand<GetMorphWeightsParams, MorphWeightsResult>`): resolve
`morphableDescendant`; `Err` if no `MorphComponent`. Read live weights (override layer if
present, else the durable `MorphComponent`) + the per-target names from the mesh. Echo
`animationVersion`. No mutation, no version bump.

2.4 `list-clip-bindings` (`registerCommand<ListClipBindingsParams, ClipBindingsResult>`):
resolve the entity via `animatableDescendant`, the clip via `resolveClip(ctx, params.clip)`
(~30). For each channel of the resolved clip, emit a `ClipBindingDto` with the channel
name+kind, then run the **same** durable name→Uuid resolution Phase 4 uses against the live
scene to fill `targetName`/`resolved`. Read-only.

2.5 Add a shared `channelsOf(clip)` helper (anonymous namespace) mapping a loaded clip's
generalized tracks (`AnimTrack` with `Target{Bone,Node}` + `Path::{Translation,Rotation,Scale,
Weights}` from Phases 1–2) to the `kind` token vocabulary. Use it from both `list-clip-bindings`
and the single-clip channel population (step 2.7). One token map, one code path.

2.6 Fill `morphWeights` in the existing `stateOf` helper (~188): set it only when the player's
entity (or its `morphableDescendant`) has a `MorphComponent`; leave the `std::optional` unset
otherwise (absent on the wire). This makes `get-animation-state` and every command returning
`stateOf` carry live morph weights for free, gated by the existing `animationVersion`. Node-TRS
adds nothing here — it already flows through `stateOf`.

2.7 Fill `AnimationClipDto.channels` for single-clip queries only. `list-clips`
(`control_commands_animation.cpp:229`) and `get-asset-model` (`AssetModelResult.clips`) leave
`channels` empty (the catalog stays cheap); a single-clip lookup populates it via
`channelsOf(clip)`.

2.8 **Node-TRS reuse — explicit non-action.** Do **not** add `play-node-animation`,
`play-morph-animation`, or any node/morph-specific playback verb. `play-animation` on a
BoxAnimated container drives node transforms; on a mesh drives bones + morph weights. Note this
in the command summaries. If `playerOf`/`animatableDescendant` does not resolve a skinless
animated container to its node-track player entity (Phase 4 attaches the player there), that is
a **Phase 4 gap to fix in Phase 4**, not a new command here. The morph commands set live weights
only; *playback* of weights is the clip via `play-animation`.

### 3. gen.ts — generator entries + fixtures

3.1 Add three command descriptors to the command list (~402–598, near the animation commands):

```ts
{ name: "set-morph-weights", params: "SetMorphWeightsParams", result: "MorphWeightsResult",
  summary: "set a mesh's morph-target weights (whole vector or one index)" },
{ name: "get-morph-weights", params: "GetMorphWeightsParams", result: "MorphWeightsResult",
  summary: "read a mesh's live morph-target weights + target names" },
{ name: "list-clip-bindings", params: "ListClipBindingsParams", result: "ClipBindingsResult",
  summary: "per-channel binding status of a clip against a live entity (node/morph resolution)" },
```

3.2 Add a fixture **or** a skip for each (`emitManifest` throws otherwise). All three need a
live morph/node entity in play, which the schema contract test cannot stage — record **skips**
citing the e2e, mirroring `["get-ragdoll", "needs a rigged entity in play — covered in make
e2e"]` and the `play-animation`/`seek-animation` skips (~1116–1121):

```ts
["set-morph-weights",   "needs a morph-bearing entity in play — covered in make e2e"],
["get-morph-weights",   "needs a morph-bearing entity in play — covered in make e2e"],
["list-clip-bindings", "needs a rigged/node entity + an imported clip — covered in make e2e"],
```

3.3 If `optional<vector<f32>>` is not yet handled by the mappers (note under 1.7), extend the
three mappers (`emitCpp` type fn, `emitTs` `tsType`, `jsonSchemaFor`) on the **generic**
optional-of-vector path (C++ emitted-optional of a JSON array; TS `number[] | undefined`;
OpenRPC absent/array-of-number). Do not special-case a field name.

3.4 Run `bun run tools/gen-control-dto/gen.ts` and commit **all five** generated outputs (the
generator owns their style; `make format` skips `*.generated.cpp`):
- `engine/source/saffron/control/control_dto_serde.generated.cpp`
- `engine/source/saffron/scene/scene_component_serde.generated.cpp`
- `editor/src/protocol/sa-types.ts` (generated; reach it via the `../protocol` shim)
- `schemas/control/openrpc.generated.json`
- `schemas/control/command-manifest.generated.json`

The git-diff-clean check on exactly these five **gates this phase**.

### 4. CLI verbs (the `tools/sa` / `tools/se` control CLI)

`tools/sa/source/main.cpp` dispatches generically: positionals map to params by **DTO field
declaration order**, `--flag value`/`--flag=value` by name. The three commands are reachable
the moment the engine registers them — **no per-verb C++ needed**.

4.1 Verify the field-order positional mapping works:
- `set-morph-weights <entity> --weights '[…]'` (whole vector) or
  `set-morph-weights <entity> --index N --weight W` (single target). Because `weights` is
  `optional<vector>`, pass it as a `--weights` JSON-array flag (the CLI `coerce` parses a JSON
  array token); a positional after `<entity>` would mis-map.
- `get-morph-weights <entity>`; `list-clip-bindings <entity> <clip>` (two positionals in DTO
  order: `entity` then `clip`).

4.2 If the CLI keeps a hand-listed usage/help table (grep `main.cpp` for the existing animation/
ragdoll verbs), add the three rows in the same style; otherwise the generic path covers it.
Keep the CLI usable for shell-driven visual debugging (root AGENTS.md keep-current rule).

### 5. Editor client wrappers (`editor/src/control/client.ts`)

The regenerated protocol file gives the typed shapes; `client.ts` adds thin wrappers mirroring
the existing animation/ragdoll ones (~235–311). **Phase 8 binds the UI** — this phase only adds
the methods:

```ts
setMorphWeight(p: {
  entity: string;
  weights?: number[];
  index?: number;
  weight?: number;
}): Promise<MorphWeightsResult> {
  return call("set-morph-weights", p);
},
getMorphWeight(entity: string): Promise<MorphWeightsResult> {
  return call("get-morph-weights", { entity });
},
listClipBindings(entity: string, clip: string): Promise<ClipBindingsResult> {
  return call("list-clip-bindings", { entity, clip });
},
```

No Timeline/Inspector/Clips changes here. The existing `getAnimationState` wrapper already
returns `AnimationStateResult`; the new optional `morphWeights` rides along its type with no
wrapper change.

### 6. Lua `setMorphWeights` binding (host-bound POD bridge)

`Saffron.Script` must NOT import `Saffron.Animation`. Follow the `raycast`/`setRagdollBlend`
host-bridge pattern.

6.1 Add a `ScriptHostBridge` member in `engine/source/saffron/script/script.cppm`. Weights are
variable-length, so the POD signature is a **length-prefixed array** (uuid + count +
`const f32*`), not a fixed arity:

```cpp
// Drive a mesh's morph-target weights from script without Saffron.Script importing
// Saffron.Animation. POD only: (entity uuid, weight count, weight pointer). Unset = no-op.
std::function<void(u64 entityUuid, u32 count, const f32* weights)> setMorphWeights;
```

6.2 Bind it in `script_runtime.cpp` (~1006–1046) as a Lua function on the engine table taking
the entity + a Lua sequence of numbers, copying them into a `std::vector<f32>` and calling
`host->setMorphWeights(uuid, count, data)` under the guard `if (host && host->setMorphWeights)`
(mirror the ragdoll-blend guard ~553–566). Name it `setMorphWeights`.

6.3 Wire the bridge in the Host (which already imports `Saffron.Animation`): the provided
`std::function` copies the POD array into the entity's `MorphWeightOverrideComponent` (the same
runtime layer the control command + `tickAnimation` write). Add it where
`raycast`/`setRagdollBlend`/`ragdollState` are assigned (`host.cppm` / its impl). No-op when the
entity has no `MorphComponent`.

6.4 Lua type stub: add the `setMorphWeights(entity, weights)` `---@param`/signature line to the
hand-kept engine-table defs (`library/sa.lua` → `se.lua` `SaLuaDefs` section) next to
`raycast`/`set_ragdoll_blend`. (gen.ts's `script_component_defs.generated.hpp` covers component
shapes, not engine-table functions.)

## Frontend work

Minimal by design: the regenerated protocol file carries the new types; `client.ts` gains three
thin wrappers + the ridealong `morphWeights` field on `AnimationStateResult`. The **UI binding
(Timeline channel drill-down, Inspector morph sliders) is Phase 8** and must extend the existing
Timeline + Clips + Inspector — no parallel UI here. This phase establishes only the wire surface
Phase 8 consumes.

## Performance

- `AnimationStateResult.morphWeights` is `std::optional` and set only for a morph-bearing entity
  (2.6), so the ~6 Hz reconcile poll (`store.ts`, focus-gated,
  `sceneVersion`/`selectionVersion`/`animationVersion`-keyed) carries no extra bytes for the
  common non-morph selection. A morph vector is small (a few to a few dozen scalars).
- `AnimationClipDto.channels` is empty in `list-clips` and `get-asset-model`; per-channel
  metadata is paid only on a deliberate single-clip query.
- All three commands are O(channels) or O(weights); none touch the GPU or the per-frame draw
  path. They run on the control drain like the existing animation/ragdoll commands.

## Control commands (this phase)

| Command | New? | Notes |
|---|---|---|
| `set-morph-weights` | new | whole-vector or single-index; writes the runtime override layer; bumps `animationVersion` |
| `get-morph-weights` | new | live weights + target names; echoes `animationVersion` |
| `list-clip-bindings` | new | per-channel resolved/unresolved status against the live scene |
| `play-animation` | reused | drives node-TRS (BoxAnimated) unchanged — no node verb |
| `seek-animation` | reused | node-TRS playhead |
| `set-animation-loop` | reused | node-TRS wrap |
| `get-animation-state` | reused | now carries optional `morphWeights` |

## Docs

Defer the concept pages to **Phase 9** — this phase only establishes the wire surface they
document. Do **not** add docs pages here. (Phase 9 updates
`docs/content/explanations/animation/_index.md` and the relevant animation/geometry pages with
morph + node-TRS + channel-metadata + the new control commands.)

## Tests

Gating tests run **in this phase**, not only Phase 9:

1. **`make engine`** — builds clean with the new DTOs, serde, and commands.
2. **`bun run check`** — TS typecheck + protocol regen succeed; the new wrappers and the
   `morphWeights` ridealong typecheck.
3. **Control-schema contract test** (`tools/check-control-schema`, part of `tools/ci/check.sh`)
   — passes with the new commands registered (skip reasons in the manifest), live registry
   matching OpenRPC, IDs as decimal strings.
4. **`git diff --exit-code`** on the five generated files (3.4) — clean. This + the contract
   test are the phase gate.
5. **`make prepare-for-commit`** (format + clang-tidy + oxlint) — clean for the C++/TS touched.

`tests/e2e` (bun, `make e2e`) — behavioural coverage the contract fixtures skip:

6. **Morph round-trip:** import `AnimatedMorphCube`, spawn it, `set-morph-weights` to deform the
   cube, then `get-morph-weights` reads the set value back (and `names` is non-empty when the
   asset names its targets). Assert a validation-clean engine log.
7. **Node binding report:** import/spawn `BoxAnimated`, `play-animation` on the container, then
   `list-clip-bindings` reports the node track(s) with `resolved=true` and a `node-*` kind, and
   any deliberately-unresolvable channel as `resolved=false`.
8. **Node-TRS via the reused path:** `play-animation` + `seek-animation` on `BoxAnimated`
   animates node transforms (assert the driven node's world transform changes across two seeks),
   proving the single playback path drives node-TRS with no node verb.
9. **`get-animation-state` ridealong:** on the morph cube `morphWeights` is present; on a plain
   (non-morph) entity it is absent.

Add the new commands' typed helpers to the e2e driver as needed (the suite is typed via the
generated protocol).

## Risks

- **gen.ts is regex-based:** new enums/vectors must be registered in all three maps or codegen
  mis-emits silently. `AnimationChannelDto.kind`/`ClipBindingDto.kind` are plain strings (not an
  enum) precisely to avoid the triple-edit. Confirm `optional<vector<f32>>` is supported (1.7).
- **One playback command** must remain the only one — resist a morph-/node-specific play verb
  under UI pressure; the clip via `play-animation` is the single source.
- **`AnimationStateResult` poll size** stays small because `morphWeights` is optional and
  morph-only.

## Acceptance criteria

- `set-morph-weights`, `get-morph-weights`, `list-clip-bindings` registered in
  `control_commands_animation.cpp` with gen.ts descriptors + fixtures/skips.
- Node-TRS reuses the single `play`/`seek`/`loop`/`state` path; **no** duplicate playback verb.
- `AnimationClipDto.channels` and `AnimationStateResult.morphWeights` are **additive**; `kind`
  is a plain string.
- The control-schema contract test + `bun run check` pass with the five generated files
  git-diff-clean.
- A Lua `setMorphWeights` binding works over the POD length-prefixed bridge **without**
  `Saffron.Script` importing `Saffron.Animation`.
- `make engine` + `make prepare-for-commit` clean.
- e2e: `set-morph-weights` deforms the cube and `get-morph-weights` reads it back;
  `list-clip-bindings` reports a node track's resolved status; `play-animation` on BoxAnimated
  animates node transforms.
