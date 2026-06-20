# Phase 6 — The control-schema contract gate (decimal-string-u64)

**Status:** COMPLETED

**Depends on:** 10-protocol-codegen:phase-5-xtask-emitters-and-editor-repoint, 09-control-plane:phase-1-socket-server-and-dispatch

## Goal

Keep the `check-control-schema` contract test green against the Rust engine. It is the tripwire for the
single most dangerous silent failure in the whole system: a `Uuid` id emitted as a JSON *number* instead
of a decimal *string*. JS cannot represent u64 past 2^53, so a number-encoded id corrupts silently in
the editor; only the raw-bytes check (`assertRawU64`) catches it. The test also enforces
manifest↔help completeness (no command added or dropped without the manifest knowing) and validates
every live command result against its generated OpenRPC schema.

This phase keeps the existing `tools/check-control-schema/check.ts` as the runnable gate, re-pointed at
the Rust binary and the Rust-generated schemas. The test is *generator-agnostic* (it reads
`openrpc.generated.json` + `command-manifest.generated.json`, whatever produced them), so as long as
`10-protocol-codegen` emits byte-equivalent artifacts, the existing check runs unchanged against the
Rust engine.

Concretely:

- **The TS check runs against the Rust binary** via `SAFFRON_ANIMA_BIN`, reading the schemas the Rust
  `xtask` emitter regenerated. It boots the engine, diffs live `help` against the manifest both ways
  (every live command in the manifest; every manifest command live), drives each command with its
  fixture, validates the result against its OpenRPC schema, and runs `assertRawU64` on the raw reply
  bytes.
- **`assertRawU64` is the load-bearing assertion** (`check.ts:134`): it scans the raw `result` JSON for
  the id-bearing keys (`id`, `mesh`, `albedoTexture`, `skyTexture`, `texture`, `entity`, `parent`,
  `parentId`, `rootBone`) and requires each to be a *quoted* decimal string that round-trips as a
  `BigInt`. The Rust DTO crate's `Uuid(u64)` newtype with `serde_with::PickFirst<(DisplayFromStr, _)>`
  (per `00-foundations/conventions.md` §7 and `10-protocol-codegen`) must emit exactly this; a plain
  serde `u64` emits a number and fails the gate. This phase is the executable proof that the
  encoding-decision in `10` is correct.
- **The per-fixture seeding (`paramsForFixture`) stays in TS** — it is a large hand-authored switch
  (`check.ts:232`) mapping each command to a valid param set; it is wire-level, not C++-coupled, so it
  ports across the binary swap untouched. New Rust commands that need a fixture add a case here, same as
  C++ did.
- **An optional Rust mirror.** A `#[test]` in `saffron-e2e` (phase 4) can run the same `assertRawU64`
  logic against a typed reply for the engine team's convenience, but the TS check remains the canonical
  gate because it is the one the reproducible gate (`tools/ci/check.sh:42`) already invokes and the one
  that proves the *editor's* JSON path.

## Why this shape (NO LEGACY)

- **The contract test is wire-level and generator-agnostic by design — keep it.** It reads the generated
  schema files and drives the live socket; nothing in it is C++-specific. The feasibility study names
  "the `check-control-schema` contract test" as a first-class deliverable that must stay
  "continuously-green." Rewriting it in Rust would discard a validated tripwire for zero gain and add a
  second copy of the decimal-string check. The only change is the binary it boots and the schemas it
  reads — both produced by the Rust side.
- **The decimal-string-u64 encoding is the highest-silent-risk contract in the system.** The feasibility
  study: "A default serde `u64` emits a JSON *number* and silently fails the gate." This gate is the
  *only* automated detector. It must be green before any command that returns an id is considered
  ported, and it is named in every command phase's gate in `09-control-plane`.
- **One manifest, one OpenRPC doc, one check.** The C++ generated these from `control_dto.cppm` via
  `gen.ts`; the Rust side generates byte-equivalent files from the Rust DTOs via `xtask`
  (`10-protocol-codegen`). There is exactly one set of generated artifacts and one check over them — no
  parallel "old manifest / new manifest."

## Grounding (real files/symbols)

- `tools/check-control-schema/check.ts` — `assertRawU64` (`:134`, the id-key regex + `BigInt`
  round-trip), `validate` (`:75`, OpenRPC `$ref`/`oneOf`/`const`/`enum`/`properties`/`additionalProperties`
  walker), `paramsForFixture` (`:232`, the per-command fixture switch), `schemaForResult` (`:195`),
  the help↔manifest completeness loop (`:417`), the hierarchy round-trip + bad-command checks (`:476`,
  `:527`).
- The generated artifacts it reads: `schemas/control/openrpc.generated.json`,
  `schemas/control/command-manifest.generated.json`, `schemas/control/envelope.schema.json` — emitted
  byte-equivalently by `10-protocol-codegen` phase 5.
- The encoding decision: `00-foundations/conventions.md` §7 (`Uuid` → decimal string via
  `serde_with::PickFirst<(DisplayFromStr, _)>`); the C++ source it reproduces is
  `engine-old/source/saffron/json/json.cppm` — `uuidToJson` (`:72`), `jsonU64` (`:92`).
- `tools/ci/check.sh:41` — the "control DTO contract test" step that invokes this check; carried into
  the justfile reproducible gate.

## Acceptance gate

- `tools/check-control-schema/check.ts` runs against the **Rust** binary (`SAFFRON_ANIMA_BIN`) reading
  the Rust-generated schemas and reports "all N manifest-driven control checks passed" — including
  `assertRawU64` green on every id-returning command (ids emitted as quoted decimal strings that
  round-trip as `BigInt`).
- A negative probe confirms the gate bites: a deliberately number-encoded id makes `assertRawU64` fail
  (the detector is live, not vacuous).
- help↔manifest completeness passes both directions; every live command's result validates against its
  OpenRPC schema; the bad-command and hierarchy round-trip checks pass.
- The Cargo workspace compiles; `cargo test --workspace` green; the contract step is wired into the
  reproducible gate (phase 9).

## How it closed (GREEN)

The canonical TS gate runs unchanged against the live Rust host: with `SAFFRON_ANIMA_BIN` pointed at
`target/debug/saffron-host` under a headless weston, `tools/check-control-schema/check.ts` reports
**"all 158 manifest-driven control checks passed"** — help↔manifest completeness both ways, every typed
command's result validated against its OpenRPC schema, `assertRawU64` green on every id-returning
command, plus the hierarchy round-trip, cycle rejection, and bad-command envelope checks. The
reproducible gate (`tools/ci/check.sh`) already invokes it as the "control DTO contract test" step and
it passes there too. The decimal-string-u64 encoding is correct at the source: `saffron-protocol`'s
`Uuid` newtype carries `serde_with::PickFirst<(DisplayFromStr, _)>`, so every id serializes as a quoted
decimal string.

The optional Rust mirror landed in `saffron-e2e` (the phase-4 driver):

- `saffron_e2e::assert_raw_u64(raw, label) -> Vec<String>` ports `check.ts:assertRawU64` faithfully —
  it scans the raw reply line's `result` region for the nine id-bearing keys and requires each value to
  be a quoted decimal string (or `null`), working on the *bytes* deliberately (a parsed `Value` would
  coerce a number into a `Number` and erase the quoted-vs-bare distinction).
- The committed **negative probe** is the unit test
  `assert_raw_u64_accepts_decimal_strings_and_bites_numbers` (in `crates/e2e/src/lib.rs`): a quoted
  decimal-string id passes clean and a deliberately number-encoded id is *caught*. It runs with
  `cargo test`, no display needed, so the detector's bite is regression-protected in CI. Sibling unit
  tests prove every id key is covered, `null` is allowed, only the `result` region is scanned, and a
  missing `result` never panics.
- The live positive proof is `crates/e2e/tests/contract_u64.rs`
  (`live_id_returning_commands_emit_decimal_string_ids`): it drives `add-entity cube` and
  `list-entities` against the booted host and asserts their raw reply bytes carry every id as a quoted
  decimal string.
- The byte-exact path needed a verbatim-bytes accessor, so `saffron_control_client::Client` gained
  `call_raw_text` (returns the raw reply line, lifting `ok:false` into `Error::Engine`) and `TestEngine`
  forwards it as `call_raw_text`. The TS check remains the canonical gate the reproducible gate invokes
  because it also proves the *editor's* JSON path.
