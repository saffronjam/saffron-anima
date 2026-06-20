# Rust test conventions — the binding verification policy

This is the test-architecture authority for the Rust engine, the testing counterpart to
`00-foundations/conventions.md`. Every feature phase's acceptance gate cites a rule here rather than
inventing one. The policy is **decided once** so a reviewer can predict where a behavior is tested and
which oracle proves it.

The C++ engine had no unit-test framework: its only CPU-logic verification was ~15 in-engine
`run*SelfTest` functions run at host startup under `SAFFRON_SELFTEST` (`host.cppm:1312`–`:1351`). Rust
has `#[test]` natively, so these conventions establish the *replacement discipline*, not a port of a
framework. There is exactly one way to write a test, never a self-test function and an external check
doing the same job (NO LEGACY).

---

## 1. Where a test lives (the three locations, one owner each)

A behavior is tested in exactly one of three places, chosen by what it needs to run:

| Location | For | Default? |
|---|---|---|
| **Inline `#[cfg(test)] mod tests`** in the same file as the code under test | Pure functions and type-local invariants: math/geometry formulas, byte-format encode/decode, sampling/IK, serde round-trips, DTO shapes, gizmo/picking math, the `SubscriberList` hand-roll, the seqlock header math | **Yes** — covers the bulk |
| **A crate's `tests/` directory** | Fixtures needing on-disk assets, cross-module composition within one crate, or a built artifact (a `.smodel` baked then re-read; a Slang-codegen graph compiled) | No — integration tests scoped to one crate |
| **The e2e harness** (`tests/e2e/`, bun) | Anything needing a *running engine* over the wire: control commands, render-state, validation-clean assertions, pixel checks | No — over the frozen wire |

**One owner per behavior.** Never duplicate an e2e behavior as a unit test or vice versa. If a behavior
is provable as a pure function, it is an inline `#[test]`; it does not *also* get an e2e test "for
safety". This is the NO-LEGACY single-path rule applied to tests.

**No `proptest`/fuzz mandate.** A phase *may* add a property test where the C++ had a determinism
invariant — the OBJ `BTreeMap`/first-seen dedup and the glTF node-order reconstruction are the two
named, highest-value targets (`02-math-and-geometry`). Anything beyond those is opt-in, not policy.

## 2. The shared comparators (`saffron-test-support`)

The C++ self-tests each re-declared their own `expect`/`eps`/`quatClose` lambdas (`animation.cpp:769`,
`scene.cppm:1714`, `geometry.cppm:2024`). Re-expressing each with its own helper would recreate that
drift surface, so the tolerances that *are* the contract live in **one** dev-only crate,
`engine/crates/test-support` (`saffron-test-support`), pulled in under `[dev-dependencies]` by every
crate whose oracle needs a float or byte comparator.

It exports:

| Symbol | Contract |
|---|---|
| `close(a, b, eps) -> bool` / `assert_close(a, b, eps)` | `|a − b| ≤ eps`; the assert form reports both values and the delta |
| `quat_close(a, b) -> bool` / `assert_quat_close(a, b)` | same orientation under the double cover: `|dot(a, b)| > 1 − 1e-4` (`q` and `−q` are equal) |
| `golden(actual, expected) -> Result<(), String>` / `assert_golden(actual, expected)` | byte-exact compare; on mismatch reports the first differing offset + a windowed hexdump |
| `EPS = 1e-4`, `IK_REACH_EPS = 1e-3`, `IK_OVER_REACH_EPS = 1e-2` | the C++ tolerances, documented at the symbols (§ below) |

The C++ epsilons, lifted verbatim from `runAnimationSelfTest` (`animation.cpp:766`):

- **`EPS = 1e-4`** — the general "values are equal" tolerance (`eps`, `animation.cpp:782`): sampled
  translations/scales, playhead times, applied-delta endpoints, `quat_close`'s double-cover margin.
- **`IK_REACH_EPS = 1e-3`** — two-bone IK lands its end effector on an in-range target this close
  (`animation.cpp:565,1160,1172`).
- **`IK_OVER_REACH_EPS = 1e-2`** — an over-extended chain straightens and clamps to max reach this
  close (`animation.cpp:1183,1185`); looser because the clamped solve only approximately straightens.

`golden` is the comparison core the phase-2 snapshot helper (`assert_bytes_match_golden`) wraps once it
adds on-disk fixture loading and the `UPDATE_GOLDEN=1` reseed path. Format-owning crates do not write
their own byte differ.

**No copy-pasted comparator survives.** The previously-duplicated `quat_close`/`const EPS` (in
`animation/src/lib.rs`, `animation/src/runtime.rs`) and `approx` (in `geometry/src/picking.rs`) have been
deleted and re-pointed at `saffron-test-support`. A new `fn quat_close`/`fn approx`/`const EPS` defined
in a test module is a convention violation — import it.

## 3. The single test entrypoint and the standing gates

- **`cargo test --workspace`** runs every unit + crate-`tests/` suite. One command, no per-crate
  invocation in CI.
- **The e2e suite is a separate invocation** (`bun test` in `tests/e2e/`, phase 3) — it drives the
  frozen wire and is engine-language-agnostic by design, so it is not a Cargo target.
- **The four standing gates are separate runnable checks** with one owner phase each:

  | Gate | Detects | Owner | Run by |
  |---|---|---|---|
  | Validation-layer-clean | GPU-state bugs that never throw | `13:phase-5` | every e2e asserts `validationErrors() == []`; the smoke greps the log |
  | Control-schema contract (decimal-string-u64) | a `Uuid` emitted as a JSON *number*; a missing/drifted command | `13:phase-6` | a Rust port of `tools/check-control-schema/check.ts` |
  | Cross-arch determinism | a Jolt build that loses bit-exactness across x86/ARM | `05-physics:phase-5` | `crates/physics/tests/determinism.rs` against `GOLDEN_TRACE_HASH`; operated by `13:phase-9` |
  | Golden / snapshot byte-exact | a `.smesh`/`.smat`/`.sanim` byte that drifts; a std430 offset shift; a shm header change | `13:phase-2` | snapshot `#[test]`s in the owning crate against committed fixtures |

- **The orchestrator (phase 9)** sequences all three (workspace tests, e2e, the gates) so a regression in
  any one fails the whole gate.

## 4. The per-area coverage map (binding)

Each feature phase's acceptance gate **must name its inline tests**, and any byte-exact format it owns
**must have a golden fixture**. This map (mirrored from area-13 README §2) ties each area to the specific
C++ self-test that is its oracle, so phase 8's removal ledger verifies a 1:1 mapping with no orphans. The
"landed" column reflects the current workspace state (phases 1–100 complete).

| Area | Inline `#[cfg(test)]` | crate `tests/` | Oracle from C++ | Landed |
|---|---|---|---|---|
| `00` core/signal/json | error/`Uuid`/JSON-union round-trips; `SubscriberList` fan-out/stop/unsub/re-entrant | — | `runSignalSelfTest` | core 18, signal 6, json 10 |
| `02` math/geometry | picking math; normals; `.smesh`/`.sanim` byte round-trip; `subIdFor` | baked `.smodel` open; glTF/OBJ determinism (property) | `runGeometrySelfTest`, `runPickMathSelfTest`, `runTranslateDeterminismSelfTest`, `runContainerSelfTest` | geometry 70 + `gltf_import.rs` |
| `03` ecs/scene | component serde byte-compat; hierarchy/transform math; gizmo/smoothing; migrations | scene JSON round-trip; play-mode JSON duplicate | `runSceneSerializationSelfTest`, `runSceneHierarchySelfTest`, `runPlayModeSelfTest` | scene 62 + `component_serde_bytecompat.rs`, sceneedit 58 |
| `04` animation | sampling/pose algebra; two-bone IK (the full oracle) | — | `runAnimationSelfTest` (~430 lines) | animation 33 |
| `05` physics | shape/auto-fit math; contact-ring logic | bridge smoke; **determinism gate** (its own phase) | `runPhysicsSelfTest` | physics 28 + `determinism.rs`, physics-sys 7 |
| `06` rendering | std430 size/offset `const _` asserts; barrier-derivation truth table | render-graph pass declarations | — (validation gate + golden) | rendering 143 + `swapchain_present.rs` |
| `07` assets/materials | `.smat` serde; container metadata; node-graph folding | bake/chunk-load/instantiate/extract/reimport | `runBakeModelSelfTest`, `runChunkLoaderSelfTest`, `runInstantiateSelfTest`, `runExtractSelfTest`, `runReimportSelfTest`, `runContainerMetadataSelfTest`, `runCatalogLinkageSelfTest` | assets 168 |
| `08` host/viewport | seqlock header math; layer dispatch | **shm-ABI gate** (its own phase) | — | host 24 + `shm_abi_gate.rs`, app 8 |
| `09` control | param coercion; envelope framing | socket drain; per-domain command smoke (→ e2e) | — (contract gate + e2e) | control 52 + `server.rs` |
| `10` protocol-codegen | `Uuid` decimal-string emit/accept; schemars fragment shapes | regenerated-artifact byte-diff | — (contract gate) | protocol 32 + `inventory.rs`/`schema_fragments.rs`/`wire.rs` |
| `12` scripting | value marshaling; sandbox probes; session-guard scoping | scheduler/contact-ring; generated-defs freshness | `runScriptSelfTest` | script 60 + 4 `tests/` files |

`sa` (CLI, 68 inline) and `window` (8 inline) carry their own unit tests with no C++ self-test oracle —
they are leaf utility crates, covered inline, no golden, no e2e mirror.

## 5. The self-test-removal invariant (audited by phase 8)

Pre-plan §0: "No in-engine self-test functions survive." The phase-8 ledger maps every `run*SelfTest`
to its named Rust `#[test]`/e2e replacement, and the reproducible gate (phase 9) asserts:

```sh
! grep -rniE 'run[A-Za-z]+SelfTest|SAFFRON_SELFTEST|fn .*self_test' engine/   # outside #[cfg(test)]
```

finds nothing **outside `#[cfg(test)]` modules**. The exclusion is load-bearing: a few `#[test]`
functions and doc comments *cite* the C++ oracle by name to document what they port (e.g.
`scene_hierarchy_self_test` in `scene/src/hierarchy.rs`, inside a `#[cfg(test)]` module; oracle citations
in `geometry/src/picking.rs`, `assets/src/catalog.rs`). Those are legitimate — the rule forbids a
*runtime* self-test path and the `SAFFRON_SELFTEST` env-gate, not a test that names its source oracle.
Phase 8's grep is therefore scoped to skip `#[cfg(test)]` regions; a bare `grep` will surface the cited
names and is not the gate.

As of phases 1–100: no runtime `run*SelfTest`, no `SAFFRON_SELFTEST` branch, and no shipping
`fn *self_test` exists; the only matches are inside `#[cfg(test)]` or doc comments.
