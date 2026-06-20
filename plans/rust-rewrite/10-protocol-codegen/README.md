# 10 — Protocol codegen: Rust DTOs as the single source for the frozen wire

The control wire is **frozen** (locked ground rule, feasibility §4.6): the Tauri editor — frontend and
the already-Rust `editor/src-tauri/` — speaks the JSON-over-unix-socket envelope unchanged, so the Rust
port must regenerate `@saffron/protocol` (the TS types), the OpenRPC schema, and the command manifest
**byte-equivalently** to what `tools/gen-control-dto/gen.ts` produces today, from a single Rust source
of truth. This area is the codegen replacement: it owns `saffron-protocol` (the DTO crate) and the
`xtask`-driven emitters that turn those DTOs into the editor-facing artifacts.

The headline subtraction (PP-3): the bespoke **3504-line `gen.ts` regex parser** and **all ~5.7k LOC of
hand-generated C++ serde** (`control_dto_serde.generated.cpp`, 167 KB) disappear, because the Rust
compiler is the parser and `serde` derives are the serializer. `gen.ts` exists only because C++ has no
reflection and `JSON_NOEXCEPTION` forbids serde-throwing; both reasons evaporate. What survives is a
*thin* emitter layer — schemars→OpenRPC, schemars→manifest, ts-rs→TS, and the Luau defs for area 12 —
hand-rolled over the derive ecosystem because OpenRPC and the manifest have no good crate.

This README is the locked design. Read it with [`09-control-plane/catalog.md`](../09-control-plane/catalog.md)
(the authoritative 236-DTO + 17-enum + 153-command list this codegen enumerates), the existing
`tools/gen-control-dto/gen.ts` (the generator being replaced — every emit shape below is grounded in a
`gen.ts` function), and `00-foundations/conventions.md` (the idiom rules).

---

## 1. The premise: derives subsume reflection, but five emitters remain

Rust has **no runtime reflection**. `gen.ts` works around C++'s same lack by regex-parsing
`control_dto.cppm` into a `StructDef[]`/`EnumDef[]` model and emitting six files from it. Rust does the
opposite: the DTO *is* the model, and `derive` macros run at compile time. So the strategy is **DTOs as
the single source of truth**, with these consumers:

| `gen.ts` output | Why it existed | Rust replacement |
|---|---|---|
| `control_dto_serde.generated.cpp` (parse/serialize) | C++ has no serde; `JSON_NOEXCEPTION` forbids throwing | `#[derive(Serialize, Deserialize)]` on every DTO — **deleted, no generator** |
| `scene_component_serde.generated.cpp` (component serde) | hand-maintained, registered in 4 places | a `register_component!` macro / `inventory` collection — §6 |
| `editor/src/protocol/sa-types.ts` (`@saffron/protocol`) | TS types for the editor's typed client | `ts-rs` derive → `xtask gen-protocol` emits one `.ts` |
| `schemas/control/openrpc.generated.json` | the contract test's schema oracle | hand-rolled emitter over `schemars` per-DTO fragments — §4 |
| `schemas/control/command-manifest.generated.json` | the e2e fixture/skip ledger + `help` parity | hand-rolled emitter over the command table — §5 |
| `script_component_defs.generated.hpp` (Luau `---@class` defs) | typed `:get_component()` snapshots | the **same** typegen skeleton emits the `.luau` defs — §7, owned by area 12 |

The first row collapses to zero generated code (the `?` operator is the parser; `serde_json` returns
`Result`). The other five are the area's phases. Three of them (`xtask gen-protocol`, OpenRPC, manifest)
are one tool; the Luau emitter (area 12) reuses the same skeleton.

## 2. `saffron-protocol`: the DTO crate (the single source)

`saffron-protocol` is a `lib` crate depending only on `saffron-core` (foundations contract). It holds
the **236 structs + 17 enums** transcribed from `control_dto.cppm` (catalog.md §"DTO inventory"), each
deriving the full stack:

```rust
#[derive(Debug, Clone, Serialize, Deserialize, JsonSchema, TS)]
#[serde(rename_all = "camelCase")]   // C++ field names are already camelCase; wire keys match verbatim
#[ts(export)]
pub struct RaycastParams { /* fields in declaration order */ }
```

It carries `#![deny(unsafe_code)]` (not an FFI seam). It is engine-dependency-free so both the engine
(`saffron-control` handlers), the `sa` CLI (links *only* protocol), and `xtask` (the emitters) share one
type set and can never drift. The crate is the boundary the foundations contract names: "the DTO crate
deriving serde/schemars/ts-rs; single source of truth for wire types, shared by engine + sa CLI + xtask
codegen."

**Field/struct order is load-bearing.** `control_dto.cppm` field declaration order IS the positional CLI
argument order (`AGENTS.md` rule); `gen.ts` preserves it because Rust struct field order is the derive
emit order, and the protocol crate keeps the source-file order verbatim. `serde_json` must be built with
the **`preserve_order`** feature (a `BTreeMap`-free, insertion-ordered `Map`) so result objects emit keys
in field order, matching the C++ `nlohmann::ordered_json` behavior the editor's typed client assumes.

**What stays opaque `serde_json::Value`** (catalog §"Wire-helper types"): `EntitySelector`,
`AssetSelector`, and every `Json`-typed field (`InspectResult.components`, `SetComponentFieldParams.value`,
`MaterialSetGraphParams.graph`, `SetScriptOverrideParams.value`). These carry component/graph/override
blobs whose shape the scene component registry (§6), not the protocol crate, defines — they are **not**
modeled as typed sub-DTOs. `DtoTag<T>` (the C++ template-dispatch tag) disappears entirely (PP-3: Rust
resolves `P: Deserialize` by type parameter).

## 3. The `Uuid(u64)` newtype: the byte-frozen decimal-string seam

This is the single most silent-failure-prone type in the whole wire. Every id crosses as a **decimal
string** (the JS 2^53 limit); a default `serde` `u64` emits a JSON *number* and silently corrupts the id
on a JS client. The C++ `WireUuid` (`control_dto.cppm:21`) emits via `uuidToJson` (decimal string) and
reads via `readWireUuid` (string **or** number, whole-string parse). The Rust target is a newtype with
the exact `serde_with` attribute the feasibility study and the foundations contract pin:

```rust
#[serde_with::serde_as]
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, JsonSchema, TS)]
#[ts(type = "string")]                 // ts-rs emits `type WireUuid = string` (matches sa-types.ts:7)
pub struct Uuid(
    #[serde_as(as = "serde_with::PickFirst<(serde_with::DisplayFromStr, _)>")]
    pub u64,
);
```

- **Emit**: `DisplayFromStr` writes the `u64` as its decimal `Display`, i.e. a JSON string — byte-identical
  to `uuidToJson` (`control_dto_serde.generated.cpp:645`).
- **Accept**: `PickFirst<(DisplayFromStr, _)>` tries the string form first, then the raw `u64`, reproducing
  `readWireUuid`'s string-or-number leniency (`:157`).
- **Schema**: `JsonSchema` must report `{ "type": "string" }` (matching `jsonSchemaFor` WireUuid case,
  `gen.ts:2134`), not `integer`. `serde_with`'s schemars integration handles this when the attribute is
  applied; a phase-2 test asserts the emitted schema fragment is `"type": "string"`.

The `<1024` reservation lives on the `saffron-core` `Uuid` (foundations); the protocol `Uuid` is the same
newtype re-exported, or a thin wrapper — phase-1 pins which (preferring: `saffron-core` owns the type,
`saffron-protocol` applies the wire derives). The **cross-crate contract test** the foundations
open-question names (PP-7/PP-13) lives here: it asserts the protocol `Uuid` derive and the imperative
`saffron-json` `uuid_to_json`/`json_u64` helpers emit **byte-identical** output for the same value, so the
two encode paths (DTO-derive vs scene-component imperative serde) never diverge.

## 4. The OpenRPC emitter (`xtask gen-protocol`, OpenRPC half)

`typed-openrpc` is too early-stage to depend on (feasibility §4.6), so the OpenRPC doc is hand-rolled over
`schemars` fragments — exactly what `emitOpenRpc` does (`gen.ts:2569`). The shape to reproduce:

- `{ openrpc: "1.3.2", info: { title: "Saffron Anima control DTOs", version: "0.2.0" }, methods: [...],
  components: { schemas: {...} } }`.
- `methods`: one per command in **registration order** (the command table §5), each
  `{ name, summary, params: [{ name: "params", schema: { $ref } }], result: { name: "result", schema: { $ref } } }`.
- `components.schemas`: one entry per DTO struct (sorted by name — `gen.ts:2570` sorts `schemaNames`),
  each an object schema with `additionalProperties: false`, `properties` (per-field schema), and
  `required` = the non-`Option` fields. Plus the hand-authored `componentSchemas()` block (the 21
  component shapes + `Vec3`/`Vec4`/`BVec3` + `Components`/`ComponentBody`), which `gen.ts` emits by hand
  (`gen.ts:2178`) — these describe the opaque component blobs the contract test validates.

`schemars` (draft 2020-12 — exactly what the engine declares) gives the per-DTO `properties`/`required`
fragments; the emitter assembles them into the OpenRPC envelope and prepends/sorts/refs as `gen.ts` does.
The special-cases `gen.ts` hard-codes are reproduced verbatim: `SelectionResult.entity` →
`oneOf:[EntityRef, null]` (`gen.ts:2162`), `InspectResult.components` → `$ref Components` (`:2165`),
`SetComponentParams.json` → `$ref ComponentBody` (`:2168`), `EnvironmentDto` → `$ref Environment`
(`:2153`). These are wire-shape facts, so they port as named overrides in the emitter, not as schemars
attributes (they cross DTO boundaries).

The **acceptance oracle is the unchanged `tools/check-control-schema/check.ts`**: it reads
`openrpc.generated.json`, validates live command results against the schemas, and compares live `help`
against the manifest. PP-7's emitter is correct iff that test passes against a Rust host with no edits.

## 5. The manifest emitter and the command table

The command manifest (`emitManifest`, `gen.ts:2598`) is the e2e fixture/skip ledger plus the `help`-parity
oracle: `{ generatedBy, commands: [{ name, params, result, status: "typed", fixture? | skip? }],
skips: [{ name: "help", reason: "reflective registry" }] }`. Every command has **either** a fixture
**or** a skip reason; `gen.ts` throws if neither (`:2602`). The two maps `commandFixtures` (the fixture
names) and `commandSkips` (the skip reasons, e.g. `"requires an external model fixture path"`,
`"destructive: requires confirmed-unused asset ids"`) are transcribed verbatim from `gen.ts:1090`–1162 —
they are wire-contract data, not logic, so the manifest output stays byte-identical.

**The command table is the new single source for the 153 name→params→result→summary triples.** In `gen.ts`
this is the `const commands: CommandDef[]` array (`gen.ts:138`). In Rust it becomes a single registration
site read by **both** the codegen (to emit OpenRPC methods + manifest) and the runtime (`saffron-control`'s
`register_*_commands`). The foundations contract and area-09 README both name this: "the command list
itself ... becomes a single registration site that both the codegen and the runtime read." §6 designs the
mechanism (the same `inventory`/macro discipline as the component registry); the fixture/skip maps stay
as static data tables in `saffron-protocol` (they are not in the runtime registry — they are e2e metadata).

## 6. The single-registration-site mechanism (macro / inventory)

`gen.ts` exists partly because the C++ scene-component registration is a **four-place hand-sync** trap
(`scene/AGENTS.md`: "Register it once ... miss step 3 and the component silently never serializes"). The
Rust design collapses the registration surface to **one place per type**, enforced by a test. Two
collection sites need this discipline:

1. **The scene component registry** (replacing `scene_component_serde.generated.cpp` +
   `scene_edit_components.cpp`'s 24 `registerComponent<C>` calls). Area 03 phase-5 owns the *table shape*
   (`ComponentTraits` → fn-pointer struct, `register_component::<C>`) and phase-6 owns the *serde bodies*;
   **PP-7 owns the mechanism that makes registration one place**: either a `register_component!(C, "Name")`
   declarative macro expanded at the one call site, or `inventory`-collected `submit!` records gathered at
   startup. The decision (§ phase-3) is **a declarative macro over an explicit list in one
   `register_builtin_components` function**, not `inventory` — because the registration **order is
   load-bearing** (it is the `componentOrder` canonical order and the OpenRPC/manifest emit order), and
   `inventory`'s collection order is link-order-defined and not guaranteed stable. An explicit ordered
   `register_component!` list in one function gives one-place registration *and* deterministic order; the
   macro just removes the closure boilerplate the C++ template carried. Area 03's registry-completeness
   `#[test]` is the tripwire that replaces the silent-no-serialize failure mode.

2. **The command table** (§5). Same reasoning: an explicit ordered list (the `commands: CommandDef[]`
   analogue) read by both codegen and runtime, *not* `inventory`, because registration order = `help`
   order = manifest/OpenRPC order, and a contract test compares against it (area-09 README §3). The
   runtime `register_typed::<P,R>(name, help, handler)` calls and the codegen's command list are the
   **same** ordered source: phase-4 pins whether that is one `const` table the runtime iterates to
   register and the emitter iterates to emit, or a macro that does both — preferring **one shared
   `&'static [CommandSpec]` table** in `saffron-protocol` (`{ name, summary, params: &str, result: &str }`)
   that the runtime joins to handler fns by name and the emitters read directly.

This keeps "adding a component / command touches one place" (the `gen.ts` benefit) without re-introducing
a regex parser or a second hand-synced table.

## 7. Where generation runs, and the editor repoint

`gen.ts` runs as `bun run gen:protocol` (which spawns `bun run tools/gen-control-dto/gen.ts`,
`editor/scripts/gen-protocol.ts`), called by `bun run check`/`bun run build`
(`editor/package.json:8`–10). The Rust equivalent runs in **`xtask`** (the workspace tooling bin,
foundations contract — "replaces gen.ts codegen + CompileShaders.cmake slangc fan-out"), not a `build.rs`
or a `proc-macro`:

- **Not `build.rs`**: the artifacts are *cross-repo* (`editor/src/protocol/sa-types.ts`,
  `schemas/control/*.json`, the `.luau` defs in the project scaffold), written outside the Cargo
  `OUT_DIR`/target tree; a `build.rs` that writes into the source tree is an anti-pattern and races with
  the editor build. `ts-rs` itself emits via a `#[test]`-driven export (`cargo test export_bindings`), so
  `xtask gen-protocol` wraps that plus the two JSON emitters into one command.
- **Not a `proc-macro`**: the OpenRPC/manifest/Luau emitters need the *whole* DTO set + the command table
  at once (to sort, ref, and assemble), which a per-item derive cannot see; they are whole-program
  emitters, the `xtask` shape.
- **`schemars` + `ts-rs` derives** live on the DTOs (compile-time); `xtask gen-protocol` reads them
  (calls `schema_for!`/`TS::export`) and assembles the files. So the *derives* are on the types and the
  *assembly* is in `xtask` — the split `gen.ts` did in one script.

**Editor repoint**: `editor/scripts/gen-protocol.ts` (the spawner) is changed to invoke
`cargo run -p xtask -- gen-protocol` instead of `bun run tools/gen-control-dto/gen.ts`; everything
downstream (`editor/src/protocol/index.ts` re-exports `sa-types.ts`, `bun run check` = `gen:protocol &&
tsc`) is **untouched** because the emitted `sa-types.ts` is byte-equivalent. This is the only editor
change in the whole rewrite, and it is one file. The freshness check the build gate runs
(`01-build-and-toolchain` phase-6) becomes "`xtask gen-protocol` produces a clean git diff", replacing
`check-script-defs` (deleted — the Luau defs are now generated from the binding source, §7 ties into area
12).

## 8. The Luau typegen reuse (area 12 hook)

The locked ground rule (pre-plan §0) is that the typed `sa.*` Luau surface is **generated from the Rust
binding source**, the same single-source pattern as `@saffron/protocol`. PP-7 designs the **shared typegen
skeleton**; area 12 (scripting) owns the binding source and the `sa.*` API emission. The skeleton this
area provides and area 12 reuses:

- The **component-snapshot defs** (`script_component_defs.generated.hpp`'s `---@class sa.<Component>` +
  `:get_component` overloads, `emitScriptComponentDefs`, `gen.ts:3361`) are emitted from the **same DTO
  component shapes** §6 registers — so PP-7 owns this emitter (it reads the protocol component DTOs and the
  registered-name set, exactly as `gen.ts` reads the TS interfaces + `scene_edit_components.cpp`). It emits
  a `.luau` defs file instead of a C++ `string_view` blob (NO LEGACY: no `#pragma once` header wrapper, no
  `library/sa.lua` append).
- Area 12 adds the **`sa.*` function/namespace defs** (the binding API surface) using the same
  `Rust-type → Luau-`---@class`/`fun(...)`` mapping helper this skeleton defines (the `tsToLua` analogue,
  `gen.ts:3394`). The mapping (`number`/`boolean`/`string`/`WireUuid→string`/`Vec3→{x,y,z}`/nested→`sa.T`)
  is one function in the shared typegen module, consumed by both emitters.

So the deliverable boundary: **PP-7 owns the typegen *skeleton* + the component-snapshot emitter; area 12
owns the binding-source-driven `sa.*` API emitter built on it.** The single drift tripwire
(`check-script-defs`) is deleted in both areas (one source, no hand-synced copy).

## 9. Subtractions (NO LEGACY)

- **`tools/gen-control-dto/gen.ts` (3504 LOC regex parser) is deleted** — the Rust compiler is the parser;
  `xtask gen-protocol` is ~few-hundred LOC of emitter over derives.
- **`control_dto_serde.generated.cpp` (167 KB) is deleted, no replacement** — serde derives.
- **`scene_component_serde.generated.cpp` (~5.7k LOC with the DTO serde) collapses to derives + the
  `register_component!` macro** (§6; bodies owned by area 03 phase-6).
- **`DtoTag<T>` template dispatch disappears** (`P: Deserialize` by type parameter).
- **The per-enum three-table hand-sync** (`enumWireNames` map + read/name C++ helpers) collapses to one
  `#[serde(rename_all = "kebab-case")]` (or explicit `#[serde(rename = "...")]` per variant where the
  kebab spelling is irregular, e.g. `msaa2`/`metallic-roughness`) on each of the 17 enums, with
  unknown-value = a typed `Deserialize` error (matching `read*` default-on-unknown only where C++ did —
  the catalog notes enums error on unknown).
- **`script_component_defs.generated.hpp` (C++ `string_view` header) is deleted** — replaced by a `.luau`
  defs file (§7/8).
- **The `JSON_NOEXCEPTION` firewall reason for `Saffron.Json` is gone** — `serde_json` returns `Result`;
  `saffron-json` survives only as the imperative scene-serde helper gateway (foundations), not a firewall.

## 10. Phase split

| Phase | What | Depends on |
|---|---|---|
| `phase-1-dto-crate-and-derives` | `saffron-protocol`: 236 structs + 17 enums + the 4 wire-helpers, serde/schemars/ts-rs derives, the `Uuid` `PickFirst<DisplayFromStr>` newtype + the byte-identity contract test | `00-foundations:phase-2-core-crate` |
| `phase-2-schemars-fragments-and-special-cases` | `schemars` draft-2020-12 per-DTO fragments + the hand-authored `componentSchemas()` block + the OpenRPC special-cases; `Uuid`→`"string"` schema assertion | `phase-1` |
| `phase-3-component-registry-macro` | the `register_component!` declarative macro + the ordered `register_builtin_components` discipline + the registry-completeness tripwire (table shape owned by area 03) | `phase-1`, `03-ecs-and-scene:phase-5-component-registry` |
| `phase-4-command-table` | the shared `&'static [CommandSpec]` command table (the `commands: CommandDef[]` analogue) + the fixture/skip static data tables, read by both runtime and emitters | `phase-1` |
| `phase-5-xtask-emitters-and-editor-repoint` | `xtask gen-protocol`: ts-rs TS export + OpenRPC emitter + manifest emitter; editor `gen-protocol.ts` repoint; the freshness gate; the byte-equivalence test vs the committed `gen.ts` outputs | `phase-2`, `phase-4` |
| `phase-6-luau-typegen-skeleton` | the shared `Rust-type → Luau` mapping module + the component-snapshot `.luau` emitter (the area-12 hook) | `phase-3`, `phase-5` |

`dto-crate-and-derives` is the cross-area dependency id area 03 phase-6 names; it resolves to `phase-1`
here. The emitter phases (5/6) depend on the command table + the component registry, so they land after
those exist — but the DTO crate (phase-1) is early in the global order because area 03 phase-6 (scene
serde byte-compat) blocks on the `Uuid` derive.

## 11. Grounding (real files / symbols)

| What | File | Symbols |
|---|---|---|
| The generator being replaced (all 6 emitters) | `tools/gen-control-dto/gen.ts` | `commands`, `enumWireNames`, `commandFixtures`, `commandSkips`, `parseStructs`/`parseEnums`, `emitCpp`, `emitSceneSerde`, `emitTs`, `emitOpenRpc`, `emitManifest`, `emitScriptComponentDefs`, `jsonSchemaFor`, `schemaFor`, `componentSchemas`, `tsToLua`, `main` |
| DTO source of truth (236 structs, 17 enums, wire-helpers) | `engine-old/source/saffron/control/control_dto.cppm` | `DtoTag`, `WireUuid`, `EntitySelector`, `AssetSelector`, `Vec3`, `Vec4`, all `*Params`/`*Result`/`*Dto`, the 17 enums |
| Generated serde (deleted in Rust) | `engine-old/source/saffron/control/control_dto_serde.generated.cpp` | `uuidToJson` (645), `readWireUuid` (157), `readF32` (87), the per-enum `read*`/`*Name` |
| Generated scene serde (collapses to derives + macro) | `engine-old/source/saffron/scene/scene_component_serde.generated.cpp` | `u64FromJson` (36), every `*ToJson`/`*FromJson`, `inverseBind` flat-cols (411) |
| Component registry table shape (area 03 owns) | `engine-old/source/saffron/scene/scene.cppm` | `ComponentTraits` (1209), `ComponentRegistry` (1224), `registerComponent<C>` (1301), `serializeEntity` (1491) |
| The 24 registration calls (collapse to the macro list) | `engine-old/source/saffron/sceneedit/scene_edit_components.cpp` | `registerBuiltinComponents`, the `registerComponent<C>(reg, "Name", ...)` calls |
| The contract-test oracle (kept, unchanged) | `tools/check-control-schema/check.ts` | manifest/OpenRPC validation, live `help` parity |
| Editor codegen entrypoints (repointed) | `editor/scripts/gen-protocol.ts`, `editor/package.json` | the spawner, `gen:protocol`/`check`/`build` scripts |
| The generated TS the editor consumes | `editor/src/protocol/sa-types.ts`, `editor/src/protocol/index.ts` | `WireUuid = string`, `CommandParamsMap`, `CommandResultMap`, the re-exports |
| Committed generated artifacts (regenerated byte-equivalently) | `schemas/control/{openrpc,command-manifest}.generated.json` | 153 commands + 1 `help` skip |
| Envelope schema (hand-authored, kept) | `schemas/control/envelope.schema.json` | `Envelope` |
