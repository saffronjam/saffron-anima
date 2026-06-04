# Phase 6 — Component DTO migration (scoped, optional-but-planned)

**Status:** NOT STARTED

**Depends on:** phase 5

## Goal

Close the last opaque seam: make the component bodies (Name, Transform, Mesh, Camera,
Material, DirectionalLight, PointLight, SpotLight, and the non-component `SceneEnvironment`)
flow through generated DTO serde instead of the hand-written per-component
`toJson`/`fromJson` lambdas. This is the deliberately-last, optional-but-planned phase: the
command surface is already typed (phases 2–3), and component blobs have been passing through
as opaque json — this phase types the blobs themselves. Scope it carefully; it collides with
`plans/scene-hierarchy/` and reaches into `Saffron.Scene`.

## What this phase touches (the migrate-to-generated-serde surface)

- The 8 per-component `toJson`/`fromJson` lambda pairs in
  `engine/source/saffron/sceneedit/scene_edit_components.cpp` (inside
  `registerBuiltinComponents`) — these encode the wire field names, units, and defaults.
  `registerComponent<C>` (`scene.cppm:451-490`) synthesizes every other closure; only these
  two lambdas are per-component, so they are the migration target.
- The `environmentToJson` / `environmentFromJson` free functions (`scene.cppm:382-417`) plus
  `skyModeName` / `skyModeFromName` (`scene.cppm:362-380`) — `SceneEnvironment` is **not** a
  component (`scene.cppm:212-224`), so it is scoped separately from the registry components.
- The near-duplicate Name/Transform serde inside `runSceneSerializationSelfTest`
  (`scene.cppm:666-690`) — a second hand-maintained copy that must be regenerated or deleted
  in lockstep, or the self-test diverges from the production registry.

## Wire invariants the generated serde MUST preserve

These are encoded in the hand-written lambdas today and are **not** the C++ struct member
names — the generator needs per-field rename/alias and unit support, not naive member-name
emission:

- camelCase keys; u64 ids as decimal strings (`WireUuid` / `uuidToJson`).
- `Transform.rotation` is Euler XYZ **radians** on the wire.
- `SpotLight.innerAngle` / `outerAngle` are **degrees** on the wire.
- `Camera` uses wire keys `near` / `far`, not the C++ field names `nearPlane` / `farPlane`
  (the toJson deliberately renames, `scene_edit_components.cpp`).

## Mechanism decision (a fork — recommended option)

C++26 static reflection is unavailable (Clang 21 + libc++), so generated component serde
cannot reflect struct members. Two options:

- **Recommended: extend the existing textual DTO generator** to read component DTO structs
  declared in the `:Dto` partition (or a Scene-side DTO partition), with per-field annotations
  for the wire key rename and unit conversion (radians↔, degrees↔). The component structs are
  declared once in the restricted subset; the generator emits the `toJson`/`fromJson` bodies
  that the registry lambdas call. This keeps one generator and one source-of-truth mechanism.
- *Alternative: vendor `reflect-cpp`* (named in `shared-types.md:26` as the candidate) — but
  it is a heavy header dep that would land in `Saffron.Scene` / a new DTO module, forcing
  classic `#include` in the GMF and no `import std`, and it adds a third-party surface for a
  job the textual generator already does. Prefer extending the generator.

The component DTOs must **not** drag an ImGui or heavy dep into `Saffron.Scene` — `drawInspector`
is deliberately kept opaque at the scene layer (`component-registry.md:74-81`); generated
serde stays data-only.

## Steps

1. Declare the 8 component DTO structs + the environment DTO in the restricted subset with the
   per-field key-rename / unit annotations above.
2. Extend the generator to emit component `toJson`/`fromJson` bodies and the environment
   serde; regenerate.
3. Replace the hand-written lambdas in `scene_edit_components.cpp` with calls to the generated
   serde (the `registerComponent<C>` itable still wires them; only the lambda bodies change to
   delegate). Regenerate or delete the `runSceneSerializationSelfTest` duplicate
   (`scene.cppm:666-690`).
4. Type the `inspect` `components` map and the `set-component` `json` body that phases 2/5
   left opaque — now that each component has a DTO, the open map is a map of typed component
   DTOs (keyed by registered name). Decide whether to expose the union in TS or keep the map
   `Record<name, ComponentDto>`; recommended: keep the registry-keyed map, with each value
   typed by the component DTO.
5. **`dump-schema` becomes redundant or its input.** `dump-schema`
   (`control_commands_scene.cpp`) reflects live component shapes via scratch entities; with
   generated component serde the shapes are known at generate time. Either retire `dump-schema`
   (it has no in-repo automated consumer — the maps confirm zero callers) or repurpose it to
   emit the generated DTO catalog. Remove its phase-2 carve-out from the manifest accordingly.

## Validation

- Build `-j1` + `check.sh` green; the regenerate-and-diff gate covers the component serde TU.
- Scene round-trip: save a scene with all 8 component types + a non-default environment, load
  it, and assert byte-identical re-serialization (the existing self-test, now generated).
- `inspect` output for every component type is byte-identical to the pre-migration build
  (camelCase, radians, degrees, `near`/`far` rename all preserved).
- `set-component` / `set-component-field` / `set-transform` / `set-material` round-trip
  unchanged (they route through the same `serialize`/`deserialize` closures).
- The contract test now validates the inner component fields of `inspect` (the coverage gap
  phase 5 flagged is closed).

## Risks

- **Scene-hierarchy collision (active plan).** `plans/scene-hierarchy/` phase 2 adds a 9th
  component (`RelationshipComponent`) with hand-written serde in `scene_edit_components.cpp`
  and bumps `SceneVersion` 2→3. This phase and that one collide on
  `scene_edit_components.cpp`, the component set, and the contract test. Sequence: land
  scene-hierarchy's component work first and have this phase regenerate all 9 components, or
  block this phase until that plan is `COMPLETED`. Do not migrate component serde while
  `RelationshipComponent` is mid-flight.
- **Unit/rename fidelity.** A generator that emits struct member names naively would break the
  `near`/`far` rename and the radians/degrees units, silently corrupting scene files. The
  per-field annotation support (step 1) is mandatory and must be validated by the
  byte-identical scene round-trip before the hand lambdas are deleted.
- **`dump-schema` scratch-entity path.** It mutates+restores the live scene per component
  (creates a scratch entity, serializes, destroys). If retired, confirm no consumer relies on
  it; if repurposed, keep default-construction valid. The maps confirm no in-repo automated
  consumer, so retirement is low-risk but should be explicit.
- **Reaching into `Saffron.Scene`.** Component DTOs sit closer to the scene than the control
  DTOs; placing them in `Saffron.Control:Dto` would invert the DAG (Scene must not depend on
  Control). Place component DTOs in a Scene-side partition (or keep them in SceneEdit, which
  already owns `registerBuiltinComponents`) so the dependency direction stays Scene → (no
  Control). Decide placement before declaring the structs.
