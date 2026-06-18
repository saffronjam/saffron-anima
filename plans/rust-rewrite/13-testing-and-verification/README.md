# 13 — Testing & verification

The test architecture that proves the Rust rewrite is *complete and correct*, built entirely on real
test harnesses — never an in-engine self-test function the engine runs against itself at startup. This
area is cross-cutting: it does not port a subsystem, it defines the verification discipline every
feature phase in areas `00`–`12` cites in its acceptance gate, and it owns the four standing gates and
the parity rig that decide cutover.

The folder number is topical, not temporal. The *harness and strategy* are stood up early in the
foundations block (the e2e harness already exists in `tests/e2e/`; the golden-snapshot crate and the
contract-test port land alongside `00`/`01`). Each feature phase then carries its own unit/e2e tests as
part of its gate — testing is woven in, not trailing (pre-plan §2). The only thing that genuinely
trails is the cross-engine **parity rig** (phase 7), which exists only while both engines are alive,
and the **self-test-removal ledger** (phase 8), which is the audit that no `run*SelfTest` survived.

## 1. The verification model (what proves the port is done)

The C++ engine had two detector layers: ~15 in-engine `run*SelfTest` functions gated behind
`SAFFRON_SELFTEST` (run at host startup, `host.cppm:1312`), and an external suite — the `tests/e2e`
bun harness, the `check-control-schema` contract test, the validation-layer-clean assertion, and the
present-only smoke — wired into `tools/ci/check.sh`. The rewrite **keeps and strengthens the external
layer and deletes the internal one entirely**. Verification rests on four pillars:

1. **Unit tests** — pure-CPU logic proven inline `#[cfg(test)]` (math, geometry byte formats, animation
   sampling/IK, scene serde, DTO round-trips, gizmo math), with the heavier cross-crate fixtures in a
   crate's `tests/` directory. Every C++ self-test becomes one or more of these (the oracle ported, the
   runtime function deleted). The policy is fixed in §2 below and `00-foundations/conventions.md` §8.
2. **Wire-driven e2e** — the existing `tests/e2e` bun suite (81 `*.test.ts` files), kept as-is because
   it drives the *frozen wire* and is therefore engine-language-agnostic by design. It becomes the
   **cross-engine parity harness**: point `SAFFRON_ANIMA_BIN` at the C++ binary or the Rust binary and
   the same assertions must pass on both. A thin native-Rust e2e mirror (phase 4) exists for the engine
   crew who do not want to leave Cargo.
3. **The four standing gates** — first-class, continuously-green deliverables (the feasibility study's
   "only automated detectors for the entire silent-failure class"): validation-layer-clean (phase 5),
   the decimal-string-u64 contract test (phase 6), cross-arch determinism (owned by physics phase 5,
   *operated* here), and golden/snapshot for the byte-exact formats + std430 + the shm ABI (phase 2).
4. **The parity rig** — during cutover, assert the Rust engine matches the C++ engine where it *must*:
   golden images, Jolt sim traces, and serde byte-equality, run against both binaries (phase 7).

A subsystem is verified when (a) its self-test oracle is ported to `#[test]`, (b) its slice of the e2e
suite passes against the Rust binary, and (c) any byte-exact contract it owns has a golden fixture that
matches the C++ output. The cutover (area `14`) flips the binary only when **all** of this is green.

## 2. Unit-test coverage policy, per area

Decided once, applied everywhere (cited by every feature phase's gate):

- **Inline `#[cfg(test)] mod tests`** in the same file as the code under test, for pure functions and
  type-local invariants. This is the default and covers the bulk: math/geometry formulas, byte-format
  encode/decode, sampling/IK, serde round-trips, DTO shapes, gizmo/picking math, the `SubscriberList`
  hand-roll, the seqlock header math.
- **A crate's `tests/` directory** for fixtures that need on-disk assets, cross-module composition, or a
  built artifact (e.g. a `.smodel` baked then re-read; a Slang-codegen graph compiled). These are
  integration tests scoped to one crate.
- **The e2e harness** (`tests/e2e/`) for anything that needs a *running engine* over the wire (control
  commands, render-state, validation-clean assertions, pixel checks). Never duplicate an e2e behavior as
  a unit test or vice versa — one owner per behavior (NO LEGACY).
- **No `proptest`/fuzz mandate**, but a phase may add property tests where the C++ had a determinism
  invariant (the OBJ `BTreeMap` first-seen dedup, the glTF node-order reconstruction) — these are the
  highest-value property targets and named in `02-math-and-geometry`.

Per-area coverage targets and their oracle sources:

| Area | Inline `#[cfg(test)]` | crate `tests/` | Oracle from C++ |
|---|---|---|---|
| `00` core/signal/json | error/`Uuid`/JSON-union round-trips; `SubscriberList` fan-out/stop/unsub/re-entrant | — | `runSignalSelfTest` |
| `02` math/geometry | picking math; normals; `.smesh`/`.sanim` byte round-trip; `subIdFor` | baked `.smodel` open; glTF/OBJ determinism (property) | `runGeometrySelfTest`, `runPickMathSelfTest`, `runTranslateDeterminismSelfTest`, `runContainerSelfTest` |
| `03` ecs/scene | component serde byte-compat; hierarchy/transform math; gizmo/smoothing; migrations | scene JSON round-trip; play-mode JSON duplicate | `runSceneSerializationSelfTest`, `runSceneHierarchySelfTest`, `runPlayModeSelfTest` |
| `04` animation | sampling/pose algebra; two-bone IK (the full oracle) | — | `runAnimationSelfTest` (~430 lines) |
| `05` physics | shape/auto-fit math; contact-ring logic | bridge smoke; **determinism gate** (its own phase) | `runPhysicsSelfTest` |
| `06` rendering | std430 size/offset `const _` asserts; barrier-derivation truth table | render-graph pass declarations | — (validation gate + golden) |
| `07` assets/materials | `.smat` serde; container metadata; node-graph folding | bake/chunk-load/instantiate/extract/reimport | `runBakeModelSelfTest`, `runChunkLoaderSelfTest`, `runInstantiateSelfTest`, `runExtractSelfTest`, `runReimportSelfTest`, `runContainerMetadataSelfTest`, `runCatalogLinkageSelfTest` |
| `08` host/viewport | seqlock header math; layer dispatch | **shm-ABI gate** (its own phase) | — |
| `09` control | param coercion; envelope framing | socket drain; per-domain command smoke (→ e2e) | — (contract gate + e2e) |
| `10` protocol-codegen | `Uuid` decimal-string emit/accept; schemars fragment shapes | regenerated-artifact byte-diff | — (contract gate) |
| `12` scripting | value marshaling; sandbox probes; session-guard scoping | scheduler/contact-ring; generated-defs freshness | `runScriptSelfTest` |

## 3. The four standing gates as deliverables

Each gate is a runnable check with a single owner phase; the reproducible-gate orchestrator
(`01-build-and-toolchain/phase-6`, mirrored here in phase 9) invokes them in sequence so a regression in
any one fails the whole gate.

| Gate | What it detects | Owner phase | Run by |
|---|---|---|---|
| Validation-layer-clean | GPU-state bugs that never throw (e.g. the MSAA sample-count regression) | `13:phase-5` | every e2e test asserts `validationErrors() == []`; the smoke run greps the log |
| Control-schema contract (decimal-string-u64) | a `Uuid` emitted as a JSON *number*; a command missing from the manifest; a result that drifts from its OpenRPC schema | `13:phase-6` | a Rust port of `tools/check-control-schema/check.ts`, kept as the cross-language tripwire |
| Cross-arch determinism | a Jolt build that silently loses bit-exactness across x86/ARM | `05-physics-jolt-bridge:phase-5` (built there) | operated as a standing gate by `13:phase-9`'s orchestrator |
| Golden / snapshot byte-exact | a `.smesh`/`.smat`/`.sanim` byte that drifts; a std430 offset shift; a shm header-layout change | `13:phase-2` | snapshot `#[test]`s in the owning crate against committed fixtures generated from the C++ engine |

## 4. The parity discipline (cutover only)

The parity rig (phase 7) runs both engines and diffs three things the editor cannot tolerate drifting:
golden **images** (the `*_render` pixel tests), Jolt **sim traces** (the determinism scenario, but
C++-vs-Rust rather than arch-vs-arch), and **serde byte-equality** (a scene/material/model authored by
one engine loads byte-identically in the other). It is the explicit sign-off input to `14-migration`.
It is wired only while `engine-old/` exists and is deleted with it at cutover (NO LEGACY).

## 5. The self-test-removal ledger

Phase 8 carries the authoritative ledger: every `run*SelfTest` in `engine-old/` mapped to its Rust
`#[test]`/e2e replacement, with a CI assertion that no `*self_test*`/`run*SelfTest` symbol and no
`SAFFRON_SELFTEST` env-gate survives anywhere in `engine/`. This is the audit that "no in-engine
self-test survives" (pre-plan §0) is actually true, not just intended.

## Grounding (real files/symbols)

| What | File | Symbols |
|---|---|---|
| The e2e harness (boot, `call`, `validationErrors`, `getThumbnail`, `importEntity`, `rig`) | `tests/e2e/harness.ts` | `Engine.boot`, `Engine.call`, `validationErrors`, `getThumbnail`, `importEntity`, `rig`, `shutdown` |
| The e2e suite (81 files) + tiers + golden status | `tests/e2e/AGENTS.md`, `tests/e2e/*.test.ts` | `*.test.ts`, `imggen.ts` (`makePng`, PNG decode), `material_codegen_render.test.ts`, `perf.test.ts` |
| The contract test (manifest completeness, OpenRPC validation, `assertRawU64`, per-fixture seeding) | `tools/check-control-schema/check.ts` | `assertRawU64`, `validate`, `paramsForFixture`, `schemaForResult` |
| The reproducible gate (build → smoke → contract → script-defs → projects → frontend) | `tools/ci/check.sh` | the `step` blocks |
| The self-tests to delete + their boot site | `engine-old/source/saffron/host/host.cppm` | `SAFFRON_SELFTEST` block (`:1312`–`:1351`) calling `runScene*`, `runGeometrySelfTest`, `runContainer*`, `runCatalog*`, `runBakeModel*`, `runChunkLoader*`, `runInstantiate*`, `runExtract*`, `runReimport*`, `runAnimationSelfTest`, `runScriptSelfTest`, `runSignalSelfTest`, `runPhysicsSelfTest`, `runPlayModeSelfTest` |
| The animation oracle (the richest self-test) | `engine-old/source/saffron/animation/animation.cpp` | `runAnimationSelfTest` (`:766`) |
| Geometry self-tests | `engine-old/source/saffron/geometry/geometry.cppm` | `runGeometrySelfTest` (`:2186`), `runTranslateDeterminismSelfTest` (`:1981`), `runContainerSelfTest` (`:2024`), `runPickMathSelfTest` (`:2137`) |
| Scene self-tests | `engine-old/source/saffron/scene/scene.cppm` | `runSceneSerializationSelfTest` (`:1658`), `runSceneHierarchySelfTest` (`:1854`) |
| Play-mode self-test | `engine-old/source/saffron/sceneedit/scene_edit_play.cpp` | `runPlayModeSelfTest` (`:232`) |
| Asset self-tests | `engine-old/source/saffron/assets/assets.cppm` | `runCatalogLinkageSelfTest` (`:549`), `runContainerMetadataSelfTest` (`:754`), `runBakeModelSelfTest` (`:4531`), `runChunkLoaderSelfTest` (`:4639`), `runInstantiateSelfTest` (`:5068`), `runExtractSelfTest` (`:5162`), `runReimportSelfTest` (`:5246`) |
| Script + signal + physics self-tests | `engine-old/source/saffron/{script/script.cppm,signal/signal.cppm,physics/physics.cpp}` | `runScriptSelfTest` (`:303`), `runSignalSelfTest` (`:61`), `runPhysicsSelfTest` (`:1533`) |
| The byte-exact format owners (golden sources) | `engine-old/source/saffron/{geometry,assets,rendering}` | `.smesh`/`.sanim`/`.smodel` writers, `.smat` serde, std430 `MaterialParamsData`/`InstanceData`/`GpuLight`, the shm seqlock header |
