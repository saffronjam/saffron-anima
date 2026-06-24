# Shape-coverage tests, fixtures, docs

**Status:** COMPLETED (with the e2e run deferred to real hardware). Fixture
`tests/e2e/fixtures/multi-node.gltf` (+ `gen_multi_node.py`) is the static two-mesh-node S2 shape;
engine unit tests cover the shape matrix (scene resolvers S1–S5, rig overlay, thumbnail merge, collider
fit, foot-IK rig guard); docs updated at `scene-and-ecs/scene-hierarchy.md` ("Resolving a model's draw
set" + code-table rows). The e2e driver `tests/e2e/forest-display.test.ts` is authored but the
headless-weston + llvmpipe harness cannot complete a run in this sandbox — every suite fails the same
way (confirmed against the unrelated `control.test.ts`: 2 pass / 10 fail, socket ENOENT), so it is
left for a real-hardware/CI run. The host boots clean (`SAFFRON_EXIT_AFTER_FRAMES=3`, exit 0) with
these changes. `just check`'s clippy/build legs pass; its e2e leg shares the sandbox limitation above.
**Depends on:** phases 1–6

## Goal

Close the test gap that let every one of these cracks ship: **no in-tree fixture is a multi-mesh-node
forest**, so none of the single-entity assumptions ever had a failing test. Add shape-coverage so this
class of bug can't regress, and update the docs that describe model spawn/display.

## The gap

The audit noted the existing fixtures (`two-materials.gltf`, `BoxAnimated.gltf`, `leg.gltf`) all have a
single mesh-bearing node or a rig — none exercise S2 (static multi-node forest), which is why
GothicCommode-class models broke silently. `spawn_tests.rs` has an S4 morph test
(`instantiate_animated_single_morph_node_keeps_its_player`) that *proves* the animated shape spawns fine,
yet the gate rejected it — the spawn test and the display gate were never cross-checked.

## Tasks

1. **Fixtures.** Add a minimal **static multi-mesh-node** glTF fixture (two+ sibling mesh nodes, no skin,
   no animation — the GothicCommode shape in miniature) under the test asset dir. If a multi-node
   *animated* and a *morph* fixture aren't already covered, add those too. Keep them tiny.
2. **Engine unit/integration tests.** Lock the shape matrix S1–S5 against each repaired surface:
   gate accepts (phase 2), bounds union (phase 2), rig overlay emits for a child-rig (phase 3),
   material-assign/collider-fit/morph drive hit the mesh entities (phase 4), thumbnail renders all chunks
   (phase 5).
3. **e2e (`tests/e2e`).** A `forest-display` suite: boot a headless host, load a project with the
   multi-node fixture, drive `enter-asset-preview` (assert open + bounds), `material-assign` on the
   container (assert applied), and a thumbnail request (assert no error / non-fragment) — all over the
   control plane, validation-log clean.
4. **Docs.** Update the spawn/display explanation pages under `docs/content/` (the model spawn shapes and
   the asset-preview flow) to state the rule explicitly: *display surfaces resolve the forest's mesh
   entities, never a single resolved entity*; document `model_mesh_entities` / `model_render_bounds` /
   `model_rig_entity` in the relevant `What | File | Symbols` table and update the hub `_index.md` row. Use
   the `docs-page` skill / house style; run the `humanizer` pass.

## Verify

- `just check` (full gate: build + shaders + smoke + schema + frontend build) green.
- `just e2e` — the new `forest-display` suite passes; overall suite has no *new* failures attributable to
  this work.
- `just run-docs` builds; the updated pages render and link-check clean.
- Final: `just engine` + `just prepare-for-commit` green, and mark this plan **COMPLETED** in the README.
