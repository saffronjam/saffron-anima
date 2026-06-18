# Phase 6 — Component serde (byte-compatible wire)

**Status:** COMPLETED

**Depends on:** 03-ecs-and-scene:phase-5-component-registry, 10-protocol-codegen:dto-crate-and-derives

## Goal

Reproduce every component's JSON (de)serialization byte-for-byte against the C++
`scene_component_serde.generated.cpp`, and wire it into the registry's `serialize`/`deserialize`
fn-pointers. This is the frozen-wire obligation: the on-disk `project.json` and the control-plane scene
payloads must be byte-identical so the unchanged editor and any existing project files keep working.
Every key spelling, every default, the uuid-string encoding, the enum-name spellings, the named-vector
shape, and the flat-matrix layout are load-bearing — a single drift fails silently as corrupted data.

## Why this shape (NO LEGACY)

- **The C++ "generated" file is hand-maintained; the Rust serde is derive-driven + helper-driven.** The
  C++ `scene_component_serde.generated.cpp` is authored in `gen.ts`'s `emitSceneSerde` (not
  schema-driven — `scene/AGENTS.md` calls the "generated from the catalog" header "a half-truth"). The
  Rust port replaces it with `serde` derives where the field layout is plain, plus the saffron-json
  imperative helpers (`json_u64`, `json_f32_or`, etc.) and the protocol crate's `Uuid` `serde_with`
  attribute for the cases the C++ hand-wrote. PP-7/PP-10 own the `Uuid` decimal-string derive and the
  helper set; this phase consumes them and asserts byte-equality. The ~5.7k LOC of generated C++ serde
  collapses to derives + a thin set of custom (de)serializers (a PP-3 subtraction).
- **The exact wire contracts, each reproduced verbatim:**
  - **Named vectors:** `vec3 → {"x","y","z"}`, `vec4 → {"x","y","z","w"}`, `bvec3 → {"x","y","z"}` bool.
    Never positional (quat/vec storage order is config-dependent — `scene.cppm:1121`). Per-field defaults
    differ (vec3 `0`, vec4 `1`) and must match (`scene.cppm:1123`–1151).
  - **Uuids as decimal strings** on write (`uuid_to_json`); readers accept string-or-number
    (`json_u64`/`u64_from_json`, `scene_component_serde.generated.cpp:36`). `MaterialAsset`/`ModelInstance`
    hand-inline `std::to_string` + string-or-number (`scene_edit_components.cpp:39,59`) — same contract.
  - **Enums as lowercase string names** with default-on-unknown: `SkyMode` color/texture/procedural,
    `Wrap` once/loop/pingpong, `Transition` inertialize/crossfade, `Motion` static/kinematic/dynamic,
    `Shape` box/sphere/capsule/convexhull/mesh, `Joint` fixed/hinge/swingtwist/free. Unknown → the C++
    default (with a `log_warn` for `SkyMode`).
  - **Key spellings:** `"near"`/`"far"` (not `nearPlane`), `transitionMode`, `loopBlend`, `rootBone`,
    `influenceRadius`, etc. — copy the exact key set per component from the C++.
  - **`inverseBind`:** an array of 16-element flat float arrays in column-major order
    (`glam::Mat4::to_cols_array`, matching `scene_component_serde.generated.cpp:411`).
  - **`Script.scripts`:** array of `{scriptPath, overrides}`; `overrides` is opaque JSON, defaulted `{}`,
    non-object coerced to `{}` (`scene_component_serde.generated.cpp:266`).
  - **`Bone`:** serializes as an empty object `{}` (`scene_component_serde.generated.cpp:394`).
  - **Runtime fields excluded on write / reset on read:** `AnimationPlayer` omits
    `previewInEdit`/`pingForward`/`prevClip`/`transition`/`transitionDuration`; `ReflectionProbe.dirty`
    resets to `true` on read; `CharacterController` runtime velocities serialize as zero. Match exactly.
- **`environment`/`atmosphere` serde** (`scene_component_serde.generated.cpp:64,693`): the
  `SceneEnvironment` block and the nested `AtmosphereSettings`, with `skyMode` string + the full
  coefficient set.

## Grounding (real files / symbols)

- `engine-old/source/saffron/scene/scene_component_serde.generated.cpp`: every `*ToJson`/`*FromJson`
  (e.g. `transformComponentToJson` 114, `cameraComponentToJson` 140, `materialComponentToJson` 160,
  `scriptComponentToJson` 256, `animationPlayerComponentToJson` 286, `skinnedMeshComponentToJson` 404,
  `relationshipComponentToJson` 383, `boneComponentToJson` 394, `rigidbodyComponentToJson` 554,
  `colliderComponentToJson` 595, `environmentToJson` ~692), plus `vec3ToJson`/`vec3FromJson`
  (`scene.cppm:1123`), `u64FromJson` (36), `skyModeName`/`atmosphereToJson` (23/64), and the per-enum
  name↔value helpers.
- `engine-old/source/saffron/sceneedit/scene_edit_components.cpp`: the hand-inlined `MaterialAsset`/
  `ModelInstance` serde lambdas (39, 59) — the only two registered outside the generated file.

## Acceptance gate

- Cargo workspace compiles; every component's `serialize`/`deserialize` fn-pointer is wired in the
  registry.
- `cargo test -p saffron-scene`: **byte-equality against captured C++ fixtures.** A `tests/` fixture set
  (JSON dumps produced by the C++ engine, or hand-authored to the C++ shape) is parsed → re-serialized →
  compared byte-for-byte (after canonical key ordering matching the C++ `nlohmann` object insertion
  order). Covers at minimum Transform, Camera, Material, SkinnedMesh (inverseBind matrices), Script
  (overrides passthrough), AnimationPlayer (enum names + omitted runtime fields), Rigidbody/Collider
  (enum names), Relationship (uuid string), Bone (empty object), environment/atmosphere.
- A round-trip `#[test]` per enum confirms unknown-string → C++ default.
- The contract is also asserted via the shared protocol/contract test (PP-13) that no `Uuid` emits a JSON
  *number* (decimal-string only).
- Workspace build green; prior phases still pass.
