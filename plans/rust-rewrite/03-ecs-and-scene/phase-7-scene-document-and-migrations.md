# Phase 7 — Scene document + version migrations

**Status:** COMPLETED

**Depends on:** 03-ecs-and-scene:phase-6-component-serde-bytecompat

## Goal

Assemble the whole-scene document serde on top of the per-component serde and registry: `scene_to_json`
(the `{version, environment, entities:[{id, components, componentOrder}]}` doc), `scene_from_json` (with
the v1→v4 migration branches), and the file helpers `write_scene`/`read_scene`. Port the C++
`runSceneSerializationSelfTest` as the regression `#[test]`s. After this phase the scene is fully
round-trippable and byte-compatible with the on-disk `project.json` scene block.

## Why this shape (NO LEGACY)

- **`SceneVersion = 4`, with every migration branch preserved.** The AGENTS.md note that the version is
  "3" is stale — the code is at 4 (`scene.cppm:1207`). Version history: 1 = entities only; 2 = adds the
  top-level `environment` block; 3 = adds the per-entity `Relationship` (durable parent uuid); 4 = adds
  per-entity `componentOrder`. `scene_from_json` migrates older documents (`scene.cppm:1551`):
  - v1 has no `environment` → defaulted via `environment_from_json({})`.
  - pre-v3 has no `Relationship` on entities → every entity loads as a root (`relink_hierarchy` defaults a
    root `Relationship`).
  - pre-v4 has no `componentOrder` → derive the canonical order (`sort_component_order`).
  - unknown component name → `log_warn` + skip (forward-compat read).
  - version `< 1` or `> SceneVersion` → an error.
  One reader with branches, not a per-version reader zoo (NO LEGACY: one code path, the migration is part
  of the single reader).
- **Entities are created preserving uuids, not minted.** `scene_from_json` clears the world, then for each
  entry creates a raw entity and emplaces `Id { uuid }` directly (NOT `create_entity`, which would mint a
  fresh uuid — `scene.cppm:1572`). Cross-entity references (parent uuids, skin joint uuids) resolve in a
  **post-loop** `relink_hierarchy` pass, so a child entry may precede its parent in the array
  (`scene.cppm:1615`).
- **The play duplicate depends on this being exact.** `enter_play` duplicates the scene via
  `scene_to_json` → `scene_from_json` (phase-10). The duplicate must be the same thing a save/load
  produces, so round-trip fidelity here is what makes play mode correct — a serde gap surfaces as a
  play-mode divergence, not a load error.
- **File IO returns typed `Result`.** `write_scene`/`read_scene` (`scene.cppm:1623`/1639) open the file,
  dump/parse JSON, and return the crate's `Result<()>`; the C++ `std::ofstream`/`ifstream` + manual
  error strings become `std::fs` + `?` composing a saffron-json error into the scene error via `#[from]`
  (PP-1 error model). No `JSON_NOEXCEPTION` abort firewall — serde returns `Result` (a PP-3 subtraction).

## Grounding (real files / symbols)

- `engine-old/source/saffron/scene/scene.cppm`: `SceneVersion=4` (1207), `sceneToJson` (1532),
  `sceneFromJson` (1551), `writeScene` (1623), `readScene` (1639), `environmentToJson`/`environmentFromJson`
  (consumed here), the post-loop `relinkHierarchy` resolve (1619).
- The oracle: `runSceneSerializationSelfTest` (`scene.cppm:1658`) — round-trip entity count + cube
  position, hierarchy parent-handle/children rebuild, component-order round-trip, child-before-parent
  (reversed array) resolution, v2 migration (every entity → root, canonical order derived), skinned-rig
  round-trip (bones + inverseBind survive, boneHandles re-resolve), and the dangling-parent → root
  downgrade.

## Acceptance gate

- Cargo workspace compiles; `scene_to_json`/`scene_from_json`/`write_scene`/`read_scene` exist.
- `cargo test -p saffron-scene` ports `runSceneSerializationSelfTest` as `#[test]`s and they pass:
  basic round-trip (entity count + cube translation), hierarchy round-trip (leaf resolves parent handle,
  root lists leaf child, component order survives), reversed-array (child-before-parent) resolution, v2
  migration (2 entities → 2 roots, pre-v4 canonical order derived), skinned-rig round-trip (mesh id +
  2 bones + 2 inverseBind survive, inverseBind values to `1e-6`, boneHandles re-resolved), and dangling
  parent → root.
- A `write_scene`→`read_scene` file round-trip `#[test]` (to a tmp path) confirms disk fidelity.
- A byte-equality `#[test]` against a captured C++ `project.json` scene block (full document, including
  `version`, `environment`, `componentOrder`).
- Workspace build green; prior phases still pass.
