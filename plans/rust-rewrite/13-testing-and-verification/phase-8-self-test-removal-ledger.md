# Phase 8 — The self-test-removal ledger

**Status:** COMPLETED

**Depends on:** 13-testing-and-verification:phase-1-test-conventions-and-coverage-map

## Goal

Carry the authoritative ledger that maps **every** in-engine `run*SelfTest` in `engine-old/` to its
Rust `#[test]`/e2e replacement, and the CI assertion that no self-test mechanism survives anywhere in
`engine/`. Pre-plan §0 states the rule ("No in-engine `self-test` functions survive"); this phase is the
audit that proves the rule held — that each oracle's coverage was preserved as a real test and the
startup `SAFFRON_SELFTEST` machinery was deleted with nothing slipping through.

The ledger is a living checklist filled in as each area lands its replacement; it is complete when every
row is green and the grep assertion passes.

## The ledger (every C++ self-test → its Rust replacement)

| C++ self-test (symbol, file) | Replaced by | Owning area |
|---|---|---|
| `runSignalSelfTest` (`signal.cppm:61`) — fan-out sum, stop-propagation order, unsubscribe, re-entrant self-unsubscribe | `#[test]`s on `SubscriberList` | `00-foundations` phase 3 |
| `runSceneSerializationSelfTest` (`scene.cppm:1658`) — scene JSON write/read diff | scene serde round-trip `#[test]` + golden (`smeta`/scene fixture) | `03-ecs-and-scene` phase 6/7 |
| `runSceneHierarchySelfTest` (`scene.cppm:1854`) — parent/child relink, cycle refusal, transform compose | hierarchy/transform `#[test]`s + `hierarchy.test.ts` e2e | `03-ecs-and-scene` phase 4 |
| `runPlayModeSelfTest` (`scene_edit_play.cpp:232`) — play-mode JSON-roundtrip duplicate | play-mode `#[test]` + `play.test.ts` / `undo-redo.test.ts` e2e | `03-ecs-and-scene` phase 10 |
| `runGeometrySelfTest` (`geometry.cppm:2186`) — OBJ/glTF import, `.sanim` save/load | import + `.sanim` round-trip `#[test]`s + `.smesh`/`.sanim` golden | `02-math-and-geometry` phase 3/4/5/6 |
| `runTranslateDeterminismSelfTest` (`geometry.cppm:1981`) — glTF translate graph/id stability | glTF determinism property `#[test]` | `02-math-and-geometry` phase 5 |
| `runContainerSelfTest` (`geometry.cppm:2024`) — `.smesh` write/read | `.smesh` byte golden `#[test]` | `02-math-and-geometry` phase 3 |
| `runPickMathSelfTest` (`geometry.cppm:2137`) — ray/center/gap/backface picking | picking-math `#[test]`s + `picking.test.ts` e2e | `02-math-and-geometry` phase 2 |
| `runContainerMetadataSelfTest` (`assets.cppm:754`) — `.smodel` metadata write/read | container-metadata `#[test]` + `.smodel` golden | `07-assets-and-materials` phase 3 |
| `runCatalogLinkageSelfTest` (`assets.cppm:549`) — catalog linkage | catalog `#[test]` + `catalog_cache.test.ts` e2e | `07-assets-and-materials` phase 1/4 |
| `runBakeModelSelfTest` (`assets.cppm:4531`) — bake → container | bake `#[test]` + `model_flow.test.ts` e2e | `07-assets-and-materials` phase 8 |
| `runChunkLoaderSelfTest` (`assets.cppm:4639`) — chunked model load | chunk-load `#[test]` + `model_asset.test.ts` e2e | `07-assets-and-materials` phase 4/8 |
| `runInstantiateSelfTest` (`assets.cppm:5068`) — instantiate model into scene | instantiate `#[test]` + `model_flow.test.ts` e2e | `07-assets-and-materials` phase 9 |
| `runExtractSelfTest` (`assets.cppm:5162`) — extract sub-asset | extract `#[test]` + `asset-usages.test.ts` e2e | `07-assets-and-materials` phase 8 |
| `runReimportSelfTest` (`assets.cppm:5246`) — reimport | reimport `#[test]` + `assets.test.ts` e2e | `07-assets-and-materials` phase 8 |
| `runAnimationSelfTest` (`animation.cpp:766`, ~430 lines) — sampling, pose algebra, two-bone IK | the animation oracle `#[test]`s (the area's explicit charter) | `04-animation` phase 3 (+ phase 5 for runtime slices) |
| `runScriptSelfTest` (`script.cppm:303`) — VM create, good/broken chunk, traceback, sandbox probe | VM/sandbox `#[test]`s + `script.test.ts` e2e | `12-scripting` phase 1 |
| `runPhysicsSelfTest` (`physics.cpp:1533`) — step ok across the orchestration | physics `#[test]`s + the determinism gate + `physics*.test.ts` e2e | `05-physics-jolt-bridge` phase 3/5 |

The startup driver itself — the `SAFFRON_SELFTEST` block in `host.cppm:1312`–`:1351` that calls all of
the above — is **deleted**: the Rust host has no startup self-test path and no `SAFFRON_SELFTEST` env
gate (the host run loop in `08-host-and-viewport` never branches on it).

## Why this shape (NO LEGACY)

- **One audit, not fifteen scattered claims.** Each area's phase deletes and replaces its own self-test,
  but without a central ledger there is no single place that proves *all* of them were handled and none
  slipped through as a quietly-kept runtime function. The ledger is that proof, and the grep assertion
  makes it mechanical rather than a matter of trust.
- **The startup gate is deleted, not parameterized.** A tempting non-NO-LEGACY move is to keep a
  `#[cfg(feature = "selftest")]` startup path "for convenience." That is exactly the second code path
  the conventions forbid: the engine never tests itself at runtime; tests run under `cargo test` / `bun
  test`. So the `SAFFRON_SELFTEST` branch has no Rust analogue at all.
- **The ledger maps to *named* replacements, not "covered somewhere."** Each row points at the specific
  phase that owns the replacement, so phase completion is verifiable: a row is green only when that
  phase's gate names the test. An oracle with no named owner is a gap the ledger surfaces.

## Grounding (real files/symbols)

- `engine-old/source/saffron/host/host.cppm` — the `SAFFRON_SELFTEST` block (`:1312`–`:1351`): the
  driver to delete and the complete call list the ledger rows enumerate.
- Every self-test symbol in the ledger table, at the cited file/line — read each to confirm its checks
  are preserved by the named replacement (the animation oracle, `animation.cpp:766`, is the richest and
  is `04-animation`'s explicit charter).
- `plans/rust-rewrite/00-foundations/conventions.md` §8 (no runtime self-tests) — the rule this phase
  audits.

## Acceptance gate

- The ledger is complete: every `run*SelfTest` symbol in `engine-old/` appears as a row with a named
  Rust replacement and an owning phase; no orphan oracle.
- A CI assertion (wired into the reproducible gate, phase 9) passes:
  `! grep -rniE 'run[A-Za-z]+SelfTest|SAFFRON_SELFTEST|fn .*self_test' engine/` finds nothing outside
  `#[cfg(test)]` modules — i.e. no self-test function and no startup env-gate survives in the Rust tree.
- Each row's owning phase, once landed, has its replacement test green (the ledger tracks completion as
  areas finish).
- The Cargo workspace compiles; `cargo test --workspace` green.

## Audit result (gate)

- **Ledger complete and accurate.** All 18 distinct `run*SelfTest` symbols in `engine-old/`
  (`runAnimationSelfTest`, `runBakeModelSelfTest`, `runCatalogLinkageSelfTest`, `runChunkLoaderSelfTest`,
  `runContainerMetadataSelfTest`, `runContainerSelfTest`, `runExtractSelfTest`, `runGeometrySelfTest`,
  `runInstantiateSelfTest`, `runPhysicsSelfTest`, `runPickMathSelfTest`, `runPlayModeSelfTest`,
  `runReimportSelfTest`, `runSceneHierarchySelfTest`, `runSceneSerializationSelfTest`,
  `runScriptSelfTest`, `runSignalSelfTest`, `runTranslateDeterminismSelfTest`) appear as a row with a
  named Rust `#[test]`/e2e replacement and an owning phase; no orphan oracle. Each named e2e file
  (`hierarchy`, `play`, `undo-redo`, `picking`, `catalog_cache`, `model_flow`, `model_asset`,
  `asset-usages`, `assets`, `script`, `physics*`) exists in `tests/e2e/`, and each named inline replacement
  exists (e.g. the four `SubscriberList` cases in `signal/src/lib.rs`, the IK/sampling/pose oracle in
  `animation/src/{ik,lib,runtime}.rs`, the VM/sandbox cases in `script/src/vm.rs`, the determinism gate at
  `physics/tests/determinism.rs`).
- **Grep assertion passes.** `grep -rniE 'run[A-Za-z]+SelfTest|SAFFRON_SELFTEST|fn .*self_test'`
  over `engine/**/*.rs` finds only doc/line comments that cross-reference the C++ oracle a test ports,
  plus one match — `scene_hierarchy_self_test` in `scene/src/hierarchy.rs:606` — which is a `#[test]`
  inside the `#[cfg(test)] mod tests` block at `:569`. No runtime self-test function survives and no
  `SAFFRON_SELFTEST` env-gate exists anywhere in the Rust tree (the host reads no such var; the
  `SAFFRON_SELFTEST` startup branch has no Rust analogue).
- **Workspace green.** `cargo build --workspace` compiles; `cargo test --workspace` = 1276 passed, 0
  failed across the workspace.
