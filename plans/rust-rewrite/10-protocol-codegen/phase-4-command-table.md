# Phase 4 — The shared command table (the single source the runtime and emitters both read)

**Status:** COMPLETED

**Depends on:** 10-protocol-codegen:phase-1-dto-crate-and-derives

## Goal

Define the single ordered command table — the `commands: CommandDef[]` analogue — that both
`saffron-control`'s `register_*_commands` (runtime dispatch) and the codegen emitters (OpenRPC methods +
manifest) read, plus the fixture/skip static-data tables. After this phase the 153 name→params→result→
summary triples and their fixture/skip metadata live in exactly one place, with no second hand-synced
list.

## Why this shape (NO LEGACY)

- **One table, two consumers — the `gen.ts` benefit without the parser.** Today the command list is a
  `const commands: CommandDef[]` in `gen.ts` (`gen.ts:138`), *separate* from the C++ `registerCommand`
  call sites — a two-place sync the contract test catches after the fact. The Rust design makes the table
  the single source: a `&'static [CommandSpec]` in `saffron-protocol` (`{ name, summary, params:
  &'static str, result: &'static str }`) that the runtime joins to handler fns by name and the emitters
  read directly. Area-09 README §3 and the foundations contract both name this ("a single registration
  site that both the codegen and the runtime read").
- **A `&'static [CommandSpec]` table, NOT `inventory`.** Same ordering argument as the component registry
  (phase-3): registration order = `help` order = manifest/OpenRPC `methods` order, and a contract test
  compares against it (area-09 README §3). `inventory`'s link-order collection is not deterministic, so an
  explicit ordered slice keeps the order load-bearing-and-visible. The runtime's
  `register_typed::<P,R>(name, ...)` calls iterate this table (in the five-domain registration order:
  render → scene → asset → animation → physics) to bind handlers; the emitters iterate the same slice to
  emit methods.
- **`params`/`result` are type *names* as `&str`, resolved to `$ref`s by the emitter.** The OpenRPC
  `methods` entries reference DTO schemas by name (`gen.ts:2580` `$ref:#/components/schemas/${params}`); the
  manifest carries the bare type names (`gen.ts:2607`). So the table stores the type names as strings (the
  emitter joins them to the phase-2 schema fragments), exactly as `CommandDef.params`/`.result` are strings
  in `gen.ts`. The runtime separately knows the concrete `P`/`R` types at its `register_typed` call sites —
  the table's strings are for the emitters' refs.
- **Fixture/skip maps are e2e metadata, not runtime data.** `commandFixtures` (the fixture names) and
  `commandSkips` (the skip reasons) (`gen.ts:1090`–1162) feed only the manifest; they are wire-contract
  data transcribed verbatim into two static tables in `saffron-protocol`, kept out of the runtime registry
  (the host does not need them). Every command has exactly one of the two; a `#[test]` reproduces the
  `gen.ts:2602` "missing fixture or skip" invariant so a new command without metadata fails the build.
- **`help` is the lone untyped command and the manifest's single skip.** It is not in the typed command
  table — it is registered untyped in the runtime (area-09 §5) and recorded as
  `skips:[{name:"help", reason:"reflective registry"}]` in the manifest (phase-5). This phase pins that the
  table holds exactly the 153 typed commands, `help` excluded.

## Grounding (real files / symbols)

- `tools/gen-control-dto/gen.ts`: `commands` (138, the 153 triples + summaries — the table to transcribe),
  `CommandDef` (21, the row shape), `commandFixtures` (the fixture map), `commandSkips` (1107, the skip
  reasons), `emitManifest`'s missing-fixture-or-skip throw (2602), the `help` skip (2618).
- `09-control-plane/catalog.md`: the full 153-command list grouped by registration domain (render 29,
  scene 47, asset 52, animation 13, physics 12) — the order the table preserves.
- `engine-old/source/saffron/control/control_commands_*.cpp`: the five `registerCommand` call sites the
  runtime's `register_typed` calls mirror (the join target for the table's handler fns).
- `engine-old/source/saffron/control/command.cppm`: `registerCommand<Params,Result>` (42) — the typed
  registration the runtime joins to this table by name.

## Acceptance gate

- `cargo build --workspace` succeeds; clippy + fmt clean; `#![deny(unsafe_code)]` holds.
- The `&'static [CommandSpec]` table holds exactly **153** entries in the catalog's registration order
  (render → scene → asset → animation → physics); a `#[test]` counts them and asserts the first/last name
  per domain matches the catalog, and that `help` is absent.
- The `commandFixtures` + `commandSkips` static tables are transcribed; a `#[test]` reproduces the
  `gen.ts:2602` invariant — every command name in the table has exactly one of a fixture or a skip (no
  command has both or neither) — so a new command without metadata fails.
- A `#[test]` asserts every `CommandSpec.params`/`.result` type name resolves to a DTO that exists in the
  phase-1 crate (the join the emitter relies on), catching a typo'd type name at test time.
