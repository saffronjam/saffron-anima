# Phase 0 — Research and architecture

**Status:** COMPLETED

**Depends on:** —

## Goal

Pin the decisions the rest of the plan builds on, grounded in the current control plane: the
exact restricted DTO subset, how the bun parser reads it, how the typed `registerCommand`
overload erases down to the existing row, where the `:Dto` partition sits in the module DAG,
and the irregular commands that resist a single-struct DTO. No code lands in this phase; it
produces the architecture the implementing phases follow. (Plans in this repo carry a
phase-0 research file — `plans/scene-hierarchy/phase-0`, `plans/skybox/phase-0` — and this
matches that convention.)

## The command surface, as it is

- One erased handler type. `CommandTraits.run` is a single
  `std::function<Result<json>(EngineContext&, const json&)>` (`command.cppm:37-42`). The
  lone `registerCommand` (`control_server.cpp:32`) pushes a `CommandTraits` row into
  `CommandRegistry.rows` and records `byName[name] = index` (`control_server.cpp:35-37`).
  `dispatch` (`control_server.cpp:213`) looks up the row, reads `params` via
  `request.value("params", json::object())`, calls `row->run(ctx, params)`, and wraps the
  result in the `{ok,error,result,id}` envelope. **Handlers return only the bare payload.**
  Any typed overload must erase to this same `std::function` so it coexists with the 65
  existing raw handlers without rewriting `CommandRegistry` or `dispatch`.

- Dual input shape. `positionalOr(params, name, index)` (`control_server.cpp:50`) returns
  `params[name]` if present, else `params["args"][index]` if `args` is an array, else null.
  The `se` CLI dumps bare tokens into `params["args"]` as strings; `--key value` becomes
  `params["key"]`. A `Params` DTO parser must replicate this per field: name lookup first,
  then positional index = declaration order.

- Wire id discipline. `uuidToJson` (`json.cppm:72`) emits u64 as a decimal string; `jsonU64`
  (`json.cppm:92`) reads a string or a number. `entityRef` (`control_server.cpp:134`) uses
  bare `std::to_string` while `list-entities` (`control_commands_scene.cpp`) uses
  `uuidToJson`; both yield decimal strings. `WireUuid` unifies this at the DTO boundary.

- The drift guard. `tools/check-control-schema/check.ts` validates engine **output** against
  `schemas/control/*.schema.json` and asserts the u64-as-quoted-string invariant
  (`assertRawU64`). It never parses **input**, never calls `help`, and hardcodes its
  command↔schema pairs — there is no completeness gate today.

## Decisions to pin

### The restricted DTO subset

Field vocabulary, fixed and enforced by the parser rejecting anything else:
`bool`, `i32`, `u32`, `f32`, `WireUuid`, `EntitySelector`, `std::string`, `Json`,
`std::vector<T>`, `std::optional<T>`, nested DTO structs, named `enum class`
(string-valued on the wire).
Structs are plain (no methods, no inheritance, no templates). Declaration order is the
positional order. This is narrow enough that a regex/line-oriented parser reads it without
libclang (C++26 static reflection is unavailable in Clang 21 + libc++, so textual parsing of
a restricted grammar is the chosen mechanism, not a fallback).

### `WireUuid` and `EntitySelector`

- `WireUuid` wraps a `u64` (`value`); its only wire form is a decimal string via
  `uuidToJson` / `jsonU64`. Internal code converts at the boundary only — handlers receive a
  `WireUuid` and call `.value` to get the `u64` they pass to `resolveEntity` / the registry.
- `EntitySelector` is the wire type for the id-or-name duality `resolveEntity`
  (`control_server.cpp:72`) resolves: it accepts a `WireUuid`-shaped decimal string, a bare
  name string, or a json number. Its parser preserves the exact resolution order
  (uuid-first, then exact-name scan) so behavior is unchanged. The generated parser hands the
  raw selector to the existing `resolveEntity`, which stays the resolver of record.

### The erasure thunk

`registerCommand<Params, Result>(reg, name, help, handler)` generates a lambda of the
existing erased shape that: (1) calls the generated `parse<Params>(json)` →
`Result<Params>`, returning its `Err` verbatim into the envelope on failure; (2) calls
`handler(ctx, params)` → `Result<ResultDto>`, propagating its `Err`; (3) calls the generated
`toJson(result)` → `Json`. It then `registerCommand`s that lambda through the existing path,
so `CommandRegistry` / `dispatch` are untouched.

### Module placement

A `:Dto` partition `export module Saffron.Control:Dto;` needs `Saffron.Core` (for
`Result`/`u64`/`Err`/`WireUuid`'s underlying type) and `Saffron.Json` (for `Json` and the
gateway helpers). `Saffron.Core` is already in the `:Command` partition's import set, but
`Saffron.Json` is **not** (`command.cppm:14-19` imports Core, Window, Rendering, Scene,
SceneEdit, Assets; `Json` is imported only by the implementation `.cpp`s, e.g.
`control_commands_scene.cpp:14`). Both modules already exist in the build as `CXX_MODULES`
siblings, so the partition importing `Saffron.Json` is a clean addition. It uses classic
`#include <nlohmann/json.hpp>` in the global module fragment and does **not** `import std`,
matching `command.cppm:2-4` / `json.cppm:3-4`. Add it to `engine/CMakeLists.txt`'s
`FILE_SET CXX_MODULES` `FILES` list (`engine/CMakeLists.txt:8-26`); **list order does not
matter** — CMake scans the module sources for `import`/`export` and builds the BMI
dependency graph itself, so the `FILES` order is not the build order (the existing list is
roughly topological for readability, not by requirement). The generated serde body is a
separate non-module `.cpp` listed in the `PRIVATE` sources alongside `control_commands_*.cpp`
(Open Question #1 in the README), so it does not add a BMI node.

## Irregular-command catalog (each needs a DTO design or a carve-out)

Inventory the commands the maps flag as not-one-struct-per-command, and decide each:

| Command(s) | Irregularity | Decision |
|---|---|---|
| `help` | captures `&reg`, enumerates the registry | carve-out: stays raw; manifest skip-with-reason "reflective" |
| `dump-schema` | reflects live registry/env/render-stats via scratch entities | carve-out: stays raw until phase 6 makes its output redundant |
| `pick` | result is a union: `{hit:false}` vs `entityRef + {hit,kind}`, **flat** (the handler adds `hit`/`kind` as siblings of the inlined `id`/`name` — `control_commands_scene.cpp:364-380`) | flat DTO `{bool hit, std::optional<WireUuid> id, std::optional<std::string> name, std::optional<PickKind> kind}` — EntityRef fields inlined, **not** nested under an `entity` key (matches the editor's flat `PickResult`, `client.ts:38-43`) |
| `get-selection` | nullable entity | DTO with `std::optional<EntityRef> entity` + version fields |
| `inspect` | open map keyed by registered component name | DTO with `EntityRef` head + a `components` map kept as opaque json (phase 6 may type it) |
| `screenshot` | varying `pending` flag | flat DTO `{target, path, bool pending}` |
| `get-thumbnail` / `view-asset` | base64 PNG blob `{id,format,width,height,base64}` | DTO with `std::string base64` etc.; not field-validated like a value DTO |
| `attach-/resize-native-viewport` | bespoke `readU64`/`readI32`, x/y/w/h named-only | DTO whose fields are required but named-only; parser must not treat them as positional |
| `set-*` merge commands | overlay only provided keys | DTO with `std::optional<T>` per overlayable field (README OQ #3) |
| `set-probes` / `recapture-probes` / `list-probes` | reflection-probe controls added after the original plan: bool toggle, no-param mutation, and a list result with probe validity/state | regular DTOs: `SetProbesParams { bool enabled }`, empty params for recapture, and `ListProbesResult { std::vector<ReflectionProbeRef> probes }` with uuid fields as `WireUuid` |

This table is the input to phases 2 and 3; every command lands a concrete DTO or an explicit
manifest carve-out — no command is left undecided.

## Deliverable

A short architecture note appended to this file (or the README) recording: the final field
vocabulary, the `WireUuid`/`EntitySelector` wire forms, the thunk signature, the partition
name and CMake position, and the irregular-command decisions above. No build change.

## Phase 0 architecture record

### DTO grammar

The DTO parser reads a restricted, line-oriented subset from the hand-written
`Saffron.Control:Dto` partition. It accepts only:

- `enum class Name { ... };` declarations whose wire form is a string token.
- Plain `struct Name { FieldType field; ... };` declarations with no methods,
  inheritance, templates, access labels, default member initializers, or macro-generated
  fields.
- Field types from this fixed vocabulary: `bool`, `i32`, `u32`, `f32`, `WireUuid`,
  `EntitySelector`, `std::string`, `Json`, `std::vector<T>`, `std::optional<T>`, named
  enums, and named DTO structs.

`Json` is allowed only for explicitly opaque fields such as component blobs and open maps.
Any other map/variant/pointer/reference/raw integer type is a generator error. Declaration
order is semantic: for normal params DTOs it is the positional order used by the `se` CLI;
for result DTOs it is the emitted field order.

Params parsing is name-first, positional-second:

1. if `params[fieldName]` exists, parse that value;
2. otherwise, if positional parsing is enabled for the DTO and `params.args[index]` exists,
   parse that value;
3. otherwise, use `std::nullopt` for optional fields or report a missing-field error for
   required fields.

Named-only DTOs are an explicit generator annotation for commands whose existing handlers do
not accept bare positionals (`attach-native-viewport` / `resize-native-viewport`). Empty
params DTOs are allowed for no-param commands.

### Wire id types

`WireUuid` wraps `u64 value`. Its output wire form is always a decimal string through
`uuidToJson`. Input accepts the existing `jsonU64` behavior so current numeric test clients
keep working until the schema-retirement phase tightens the public contract.

`EntitySelector` represents the current id-or-name selector. It parses a decimal string, a
json number, or a non-numeric string and keeps the raw selector semantics for
`resolveEntity`. The DTO layer does not resolve entities; typed handlers pass the selector
to the existing resolver so uuid-first/name-second behavior stays centralized.

`EntityRef` is the shared `{WireUuid id, std::string name}` result DTO. Commands returning an
entity use this exact spelling unless their existing wire format inlines the fields, as
`pick` does.

### Typed registration and erasure

Phase 1 adds a typed overload with this conceptual shape:

```cpp
template <typename Params, typename ResultDto, typename Handler>
void registerCommand(CommandRegistry& reg, std::string name, std::string help, Handler handler);
```

The overload erases to the existing `std::function<Result<json>(EngineContext&, const json&)>`
row by registering a thunk that:

1. calls generated `parseParams<Params>(params)` and returns `Err` on invalid input;
2. calls `handler(ctx, parsed)` and propagates its `Err`;
3. serializes the returned DTO with generated `toJson(result)`.

`CommandRegistry`, `CommandTraits`, `dispatch`, and the raw `registerCommand` overload stay
unchanged. Raw and typed commands coexist until the completeness phase.

### Module and generated-file placement

The hand-written DTO declarations live in
`engine/source/saffron/control/control_dto.cppm` as `export module Saffron.Control:Dto;`.
That partition imports `Saffron.Core` and `Saffron.Json`, uses classic includes in the
global module fragment, and does not `import std`.

`control.cppm` exports the new partition with `export import :Dto;`. CMake lists the DTO
partition in the existing `FILE_SET CXX_MODULES`; source order is for readability only
because CMake scans module dependencies. Generated serde lives in a non-module
implementation source, for example
`engine/source/saffron/control/control_dto_serde.generated.cpp`, listed as a normal private
source so it does not add a BMI node.

Generated artifacts are committed and refreshed by `tools/gen-control-dto/`:

- C++ parse/toJson implementations.
- `editor/src/protocol/se-types.ts` with `CommandParamsMap` and `CommandResultMap`.
- `schemas/control/openrpc.generated.json`.
- a command manifest consumed by the contract test.

The freshness gate regenerates these files and fails on diff.

### Irregular commands

The carve-outs are `help` and `dump-schema`; both remain raw and appear in the manifest with
skip reasons (`reflective registry` and `legacy schema dump`). `inspect` keeps
`components: Json` until component DTOs land in phase 6. `set-component` and
`set-component-field` keep their component body/value as opaque `Json` in early phases.

Union-like results are flattened to match current wire shape rather than normalized:

- `pick` stays `{hit, id?, name?, kind?}`.
- `get-selection` uses `std::optional<EntityRef> entity`.
- `screenshot`, thumbnail/view results, and reflection-probe results are ordinary DTOs.

Merge commands (`set-transform`, `set-material`, `set-light`, `set-environment`,
`set-camera`, `set-gizmo`) use `std::optional<T>` for every patchable field so absence and
explicit defaults are distinct.

### Live command inventory

Phase 0 cross-checked the command surface against live `help` output from a headless host on
2026-06-04. The current surface has 65 commands:

`add-component`, `add-entity`, `assign-asset`, `attach-native-viewport`, `copy-entity`,
`create-entity`, `deselect`, `destroy-entity`, `dump-schema`, `focus`, `get-camera`,
`get-environment`, `get-gizmo`, `get-project`, `get-selection`, `get-thumbnail`,
`gizmo-pointer`, `help`, `import-model`, `import-texture`, `inspect`, `list-assets`,
`list-components`, `list-entities`, `list-probes`, `load-project`, `load-scene`,
`new-project`, `open-project`, `pick`, `ping`, `quit`, `recapture-probes`,
`remove-component`, `rename-asset`, `rename-entity`, `render-stats`,
`resize-native-viewport`, `save-project`, `save-scene`, `screenshot`, `select`, `set-aa`,
`set-camera`, `set-clustered`, `set-component`, `set-component-field`,
`set-contact-shadows`, `set-depth-prepass`, `set-environment`, `set-exposure`, `set-gi`,
`set-gizmo`, `set-ibl`, `set-light`, `set-material`, `set-probes`, `set-restir`,
`set-rt-shadows`, `set-shadows`, `set-ssao`, `set-ssgi`, `set-transform`, `view-asset`,
`viewport-native-info`.

## Validation

- Documentation-only phase: build and `check.sh` are unaffected (nothing compiles yet).
- Cross-check the irregular-command table against the live `help` output (65 commands) so no
  command is missing from the catalog before phase 1 starts.

## Risks

- **Under-scoping the subset.** If a real command needs a type the vocabulary lacks (e.g. a
  map, a variant), the parser must reject it loudly rather than silently mis-generating.
  Decide the map/union handling here (carve-out to opaque json, as for `inspect`) so phase 2
  does not discover it mid-migration.
- **`resolveEntity` ownership.** Keep `resolveEntity` as the single resolver — the DTO layer
  passes the selector through, it does not reimplement id-or-name resolution, or the two
  drift.
