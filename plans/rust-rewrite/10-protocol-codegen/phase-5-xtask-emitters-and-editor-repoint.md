# Phase 5 — `xtask gen-protocol`: the TS / OpenRPC / manifest emitters + the editor repoint

**Status:** COMPLETED

**Depends on:** 10-protocol-codegen:phase-2-schemars-fragments-and-special-cases, 10-protocol-codegen:phase-4-command-table

## Goal

Implement the `xtask gen-protocol` subcommand that emits the three editor-facing artifacts —
`editor/src/protocol/sa-types.ts` (via ts-rs), `schemas/control/openrpc.generated.json`, and
`schemas/control/command-manifest.generated.json` — byte-equivalently to today's `gen.ts` output, repoint
the editor's `gen-protocol.ts` spawner to it, and wire the freshness gate. This is the phase that makes
the editor run unchanged.

## Why this shape (NO LEGACY)

- **One `xtask` subcommand, not `build.rs`, not a proc-macro.** The artifacts are cross-repo files
  (`editor/`, `schemas/`) written outside the Cargo target tree, so a `build.rs` (which should write only
  to `OUT_DIR`) is wrong and would race the editor build; a per-item proc-macro cannot see the whole DTO
  set + command table at once (needed to sort, `$ref`, and assemble). `xtask` is the foundations-contract
  home for "replaces gen.ts codegen" and `01-build-and-toolchain` already places shader codegen there.
- **ts-rs replaces `emitTs`.** `gen.ts`'s `emitTs` (3-table TS interface emission, `gen.ts:1813`) is
  replaced by `ts-rs` `#[derive(TS)]` on each DTO; `xtask gen-protocol` triggers the export (ts-rs emits
  via a `cargo test`-style export, so the xtask wraps `TS::export_all_to` into one `sa-types.ts`). The
  `Uuid` newtype carries `#[ts(type="string")]` so the output keeps `export type WireUuid = string`
  (`sa-types.ts:7`). The `CommandParamsMap`/`CommandResultMap` index types (`gen.ts:2105`) are emitted from
  the command table (phase-4) joined to the DTO type names — a small hand-emitted tail ts-rs does not
  produce, appended after the interfaces.
- **The OpenRPC emitter assembles phase-2 fragments in the phase-4 order.** It reproduces `emitOpenRpc`
  (`gen.ts:2569`): the `{openrpc:"1.3.2", info, methods, components.schemas}` envelope, `methods` one per
  command in table order (`$ref` params/result), `components.schemas` = the per-DTO fragments (sorted by
  name, `gen.ts:2570`) + the `componentSchemas()` block + the four special-cases. `serde_json` with
  `preserve_order` keeps key order; the emitter pretty-prints with 2-space indent + trailing newline to
  match `JSON.stringify(doc, null, 2) + "\n"` (`gen.ts:2595`).
- **The manifest emitter reproduces `emitManifest`.** `{generatedBy, commands:[{name,params,result,
  status:"typed", fixture?|skip?}], skips:[{name:"help", reason:"reflective registry"}]}` (`gen.ts:2598`),
  driven by the phase-4 table + fixture/skip tables; the `generatedBy` string updates to the xtask path
  (the only intentional byte change, asserted as the sole diff).
- **The editor repoint is one file.** `editor/scripts/gen-protocol.ts` (the spawner, currently
  `bun run tools/gen-control-dto/gen.ts`) changes to `cargo run -p xtask -- gen-protocol`; everything
  downstream (`editor/src/protocol/index.ts` re-exports, `editor/package.json`'s `gen:protocol`/`check`/
  `build` scripts) is untouched because the emitted files are byte-equivalent. This is the only
  editor-side change in the rewrite.
- **The freshness gate replaces `check-script-defs`.** `01-build-and-toolchain` phase-6's gate runs
  `xtask gen-protocol` and asserts a clean git diff (codegen is up to date), the direct analogue of the
  C++ gen-freshness check; the deleted `check-script-defs` drift tripwire is gone (Luau defs are generated,
  phase-6).

## Grounding (real files / symbols)

- `tools/gen-control-dto/gen.ts`: `emitTs` (1813), `tsType` (1746), `CommandParamsMap`/`CommandResultMap`
  emission (2105), `emitOpenRpc` (2569), `emitManifest` (2598), `main`'s write fan-out (3463) and the
  `JSON.stringify(_, null, 2)+"\n"` formatting (2595/2614).
- `editor/scripts/gen-protocol.ts`: the spawner (`spawn("bun", ["run", generator])`) the repoint edits.
- `editor/package.json`: `gen:protocol` (8), `check` (9), `build` (10) — the scripts that stay unchanged.
- `editor/src/protocol/sa-types.ts` + `index.ts`: the byte-equivalence target + the re-export façade.
- `schemas/control/openrpc.generated.json`, `schemas/control/command-manifest.generated.json`: the
  committed artifacts to match byte-for-byte (modulo the `generatedBy` string).
- `tools/check-control-schema/check.ts`: the contract-test oracle (unchanged) that validates the emitted
  OpenRPC + manifest against a live Rust host.

## Acceptance gate

- `cargo run -p xtask -- gen-protocol` writes the three artifacts; `cargo build --workspace` + clippy + fmt
  clean; `#![deny(unsafe_code)]` holds (xtask is a bin and may use `anyhow`).
- **Byte-equivalence**: a `#[test]` (or the gate script) runs the emitter and diffs each output against the
  committed `gen.ts` artifact — `sa-types.ts` and the two JSON files are byte-identical except the
  manifest's `generatedBy` string (asserted as the only difference).
- The editor builds unchanged: with `gen-protocol.ts` repointed, `cd editor && bun run check` (=
  `gen:protocol && tsc --noEmit`) passes with **zero** TypeScript edits to `index.ts`/`sa-types.ts`
  consumers.
- The unchanged `tools/check-control-schema/check.ts` validates the emitted OpenRPC schemas and `help`
  parity against a live Rust host (run when `saffron-control` exists; until then, against the committed
  artifacts as a self-consistency check).
- Re-running `xtask gen-protocol` produces a clean git diff (the freshness gate); the gate fails if a DTO
  or command changed without regenerating.
