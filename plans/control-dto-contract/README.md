# Control DTO contract

Make C++ DTO structs the single source of truth for the control-plane wire contract. Today
the schemas in `schemas/control/*.schema.json` are hand-authored and the engine's command
handlers build ad-hoc `nlohmann::json` literals that a separate contract test validates
against those schemas after the fact. This plan inverts that: a deliberately restricted set
of C++ DTO structs describes every command's params and result, a small bun generator emits
the C++ serde, the TypeScript protocol, and an OpenRPC document from those structs, and the
editor's `callRaw` escape hatch disappears so the typed `call<C>(cmd, params)` is the only
way to reach the engine — what the DTO says is what both sides get.

**Status:** COMPLETED

## Why

Three sources describe the same wire today and drift independently:

- the engine handler that produces the json (`control_commands_*.cpp`, ad-hoc `json{...}`),
- the hand-authored schema it is validated against (`schemas/control/*.schema.json`),
- the hand-kept `CommandResultMap` block the editor's typegen appends
  (`editor/scripts/gen-protocol.ts:65-95`).

There are **no C++ DTO structs in the loop at all** — every handler takes a raw `const json&`
and returns a raw `json` through one erased signature (`CommandTraits.run`,
`command.cppm:41`), and the only drift guard is `tools/check-control-schema/check.ts`, which
validates engine *output* against the schemas but never exercises *input* parsing and never
asserts the command set is complete. The editor splits its call sites between a typed
`call<C>` (~21 wrappers) and an untyped `callRaw` with manual `as` casts
(`client.ts:84-88`), so the type system does not actually cover the wire.

A DTO source of truth collapses the three descriptions into one and lets the generator keep
the TypeScript types, the C++ serde, and the docs in lockstep with a freshness gate.

## The restricted DTO subset (so a bun parser, not libclang, can read it)

DTO structs live in a `:Dto` partition of `Saffron.Control` (see phase 1 for placement) and
are written in a narrow C++ subset the generator's parser understands:

- plain `struct`s, no methods, no inheritance;
- field types drawn from a fixed vocabulary: `bool`, `i32`, `u32`, `f32`, `WireUuid`,
  `std::string`, `std::vector<T>`, `std::optional<T>`, nested DTO structs, and named enums;
- declaration order is meaningful — it is the positional-argument order for params (so the
  `se` CLI's bare-positional form keeps working) and the field order in the result.

`WireUuid` is a strong type wrapping `u64` whose wire form is **always** a decimal string,
routed through the existing `uuidToJson` / `jsonU64` gateway (`json.cppm:37,42`). Internal
engine code keeps `u64`; the `u64`↔string conversion lives only at the DTO boundary. An
`EntitySelector` wire type captures the id-or-name duality that `resolveEntity`
(`control_server.cpp:72`) resolves today.

The `{WireUuid id, std::string name}` pair that ~12 commands return is the DTO **`EntityRef`**
— one name, used everywhere, matching the existing wire title (`entity-ref.schema.json`'s
`title: EntityRef` and the generated TS interface `EntityRef`, `protocol/index.ts:227`). No
`EntitySummary` / alternate spelling; the plans name it `EntityRef` throughout.

## Typed handlers

A new `registerCommand<Params, Result>(reg, name, help, handler)` overload takes a handler
of shape `(EngineContext&, const Params&) -> Result<ResultDto>` and **erases down to the
existing** `std::function<Result<json>(EngineContext&, const json&)>` row
(`command.cppm:41`) — it generates a thunk that parses params from json into `Params`,
calls the handler, and serializes `ResultDto` back to json. `CommandRegistry`, `dispatch`
(`control_server.cpp:213`), and the 65 existing raw-json handlers are untouched and coexist;
commands migrate one at a time.

The params-parse and result-serialize functions are **generated** (one
`auto parse<Params>(const Json&) -> Result<Params>` and one `toJson(const ResultDto&) ->
Json` per DTO), `std::expected`-returning and abort-free under `JSON_NOEXCEPTION` — they
emit per-field `is_*()` checks through the `json.cppm` helpers exactly like the scene
component serde does (`scene.cppm` `fromJson` traits), never raw `.at()` / `.get_to()` /
`NLOHMANN_DEFINE_TYPE_*` (those abort on malformed input — see Risks).

## The generator

A self-contained bun tool under `tools/gen-control-dto/` (mirroring the zero-dep
`tools/check-control-schema/check.ts` shape — `#!/usr/bin/env bun`, no `bun install`) reads
the `:Dto` partition source and emits four artifacts, all **committed** with a
`GENERATED — do not edit` banner and a regenerate-and-diff freshness gate in `check.sh`:

1. C++ serde TU(s) for the DTOs (the parse/toJson functions),
2. `editor/src/protocol/se-types.ts` replacing the schema-derived `index.ts`, carrying full
   `CommandParamsMap` **and** `CommandResultMap` (the hand-kept map in `gen-protocol.ts`
   dies),
3. an OpenRPC document as the generated lingua franca for docs/tooling,
4. the command manifest the contract test iterates (command → params DTO, result DTO,
   schema identity, skip-with-reason).

## Phases (dependency order)

| # | Phase | File | Depends on |
|---|-------|------|------------|
| 0 | Research and architecture: pin the subset, the parser, the erasure thunk, the placement | [`phase-0-research-and-architecture.md`](phase-0-research-and-architecture.md) | — |
| 1 | Foundation: `WireUuid`/`EntitySelector`, the `:Dto` partition, generator v1, typed `registerCommand`, 2–3 pilot commands | [`phase-1-foundation.md`](phase-1-foundation.md) | 0 |
| 2 | Migrate the scene command group to typed DTOs | [`phase-2-scene-commands.md`](phase-2-scene-commands.md) | 1 |
| 3 | Migrate the render + asset/project command groups to typed DTOs | [`phase-3-render-asset-commands.md`](phase-3-render-asset-commands.md) | 1 |
| 4 | Editor cutover: generated `se-types.ts`, typed `call`, delete `callRaw`/`.raw()`, migrate call sites | [`phase-4-editor-cutover.md`](phase-4-editor-cutover.md) | 2, 3 |
| 5 | Completeness gate, contract-test refactor, schema retirement, docs/AGENTS rewrite | [`phase-5-completeness-and-retirement.md`](phase-5-completeness-and-retirement.md) | 4 |
| 6 | Component DTO migration (scoped, optional-but-planned) | [`phase-6-component-dtos.md`](phase-6-component-dtos.md) | 5 |

Each phase is independently landable and ends with the engine build (`cmake --build
build/debug -j1`) and `tools/ci/check.sh` green. Phases 2 and 3 are parallel (both depend
only on the phase-1 foundation); phase 4 needs both because deleting `callRaw` requires
every call site to have a typed target.

## Status convention

Each phase file carries a `**Status:**` line (`NOT STARTED` / `IN PROGRESS` / `COMPLETED`).
Mark a phase `COMPLETED` when its work is done and validation-clean; delete a phase file only
*after* it is `COMPLETED` and merged.

## Current anchors

- One erased command signature: `CommandTraits.run` is
  `std::function<Result<json>(EngineContext&, const json&)>` (`command.cppm:41`); the lone
  `registerCommand` (`control_server.cpp:32`) pushes a row and records its insertion index;
  `dispatch` (`control_server.cpp:213`) is non-generic and adds the `{ok,error,result,id}`
  envelope — handlers return only the bare payload. There is no templated overload anywhere.
- 65 `registerCommand` calls across `control_commands_render.cpp` / `_scene.cpp` /
  `_asset.cpp`, registered in render → scene → asset order
  (`registerBuiltinCommands`, `control_server.cpp:140`); insertion order is meaningful
  (help/list iterate it).
- Dual named/positional input via `positionalOr` (`control_server.cpp:50`): a param arrives
  as a named key **or** as the index-th element of `params["args"]` (strings from the CLI).
  Many handlers add per-field coercion (string→u64, multi-type bool) on top.
- Wire id discipline: u64 ids cross as decimal **strings** via `uuidToJson`
  (`json.cppm:72`), read back by `jsonU64` (`json.cppm:92`, accepts string or number), for
  JS precision past 2^53. `entityRef` (`control_server.cpp:134`) uses bare `std::to_string`
  while `list-entities` uses `uuidToJson` — functionally equal, inconsistent helper.
- `JSON_NOEXCEPTION` (`cmake/Dependencies.cmake:118`) turns every unchecked `.at()` /
  `.get<T>()` / missing-key `operator[]` into `std::abort()`. The `json.cppm` gateway exists
  precisely to type-check before extracting; all new serde must follow it.
- The committed-generated precedent is `editor/src/protocol/index.ts` (banner, produced by
  `gen-protocol.ts` from the schemas). Freshness today is by **regeneration** in the build
  (`bun run build` runs `gen:protocol` first), not by diff — there is no `git diff
  --exit-code` step in `check.sh`. This plan adds one for the C++ artifact.
- The `:Command` partition imports Core, Window, Rendering, Scene, SceneEdit, Assets
  (`command.cppm:14-19`) — **not** `Saffron.Json` (that import lives in the implementation
  `.cpp`s, e.g. `control_commands_scene.cpp:14`). The new `:Dto` partition imports Core +
  Json directly; both modules already exist in the build (Json is `command.cppm`'s sibling
  in the `CXX_MODULES` set), so adding the import is a clean addition, not a new dependency
  edge for the whole module.
- Module rules: the control/json modules use classic `#include` in the global module
  fragment and do **not** `import std` (mixing breaks the TU — `command.cppm:2-4`,
  `json.cppm:3-4`); builds run `-j1` because parallel intermittently hits a clang
  module-BMI ICE (root `AGENTS.md`, `check.sh`).

## Keep-current obligations

- **`se` CLI / control:** every migrated command keeps its positional and named param forms
  (the generated parser preserves declaration-order positionals). The `se` CLI links only
  `nlohmann_json`, has no engine dependency, and stays manifest-free (its per-command text
  printers read ids as strings — phase 4/5 confirm no field renames break them silently).
- **`docs/`:** the contract docs now describe the DTO-first / typed-handler flow, the
  generated OpenRPC + manifest artifacts, and the generated scene component serde.

## Out of scope / deferred

- **Asset-catalog / project-file serde** (`catalogToJson` / `saveProject` in `assets.cppm`)
  is not a control DTO; it is left hand-authored unless a later plan revisits it.
- **Reflective/meta commands:** `help` stays dynamic and carries a skip reason in the
  manifest. `dump-schema` was retired after generated scene component serde made it redundant.
- **C++26 static reflection** is not available in Clang 21 + libc++; the generator parses
  the restricted DTO source textually rather than reflecting, so nothing here waits on it.

## Cross-plan coordination

`plans/scene-hierarchy/` is in flight and touches the same surface (it adds a
`RelationshipComponent`, bumps `SceneVersion` 2→3, adds a `set-parent {entity, parent?}`
command returning `EntityRef`, adds an optional `parentId` to `list-entities`, edits
`entity-ref`/`entity-list` schemas, regenerates `@saffron/protocol`, and widens the
contract-test u64 allowlist — all in its phase 4). Its new command + `parentId` raise the
registered command count and the entity-list shape this plan migrates, so phase 2 must add
`set-parent` to its worklist (and the completeness count) if scene-hierarchy lands first; its
component-serde work collides with **this plan's phase 6**, and its schema/contract-test edits
collide with **phase 5**. Sequence: land scene-hierarchy's schema work first, or have this
plan's generator already cover `RelationshipComponent`, `set-parent`, and `parentId`. Phases
1–4 here do not touch component serde or `list-entities`'s shape, so they can proceed in parallel with
scene-hierarchy's engine phases (1–3) without collision.

## Open questions

1. **Generated C++ serde — separate TU vs. partition impl.** The generator can emit the
   parse/toJson functions either as a non-module `.cpp` implementation unit of
   `Saffron.Control` (regular `PRIVATE` source, like `control_commands_*.cpp`) or fold them
   into the `:Dto` partition. Recommendation: a separate generated impl `.cpp` so the
   committed `:Dto` interface stays hand-written and small, and the generated body is a leaf
   that does not participate in the BMI dependency scan. Phase 1 settles it.
2. **OpenRPC fidelity vs. JSON Schema reuse.** OpenRPC embeds JSON Schema for params/results;
   the generator can emit schemas inline or `$ref` a shared `$defs` block. Recommendation:
   inline a `components.schemas` `$defs` block keyed by DTO title (mirrors the existing
   `gen-protocol` bundle shape) so the document is self-contained for docs tooling. Phase 1
   produces a minimal document; phase 5 makes it the published lingua franca.
3. **Merge-over-current params (`set-transform`, `set-material`, `set-environment`,
   `set-camera`, `set-component-field`).** These read the current serialized value and
   overlay only provided keys — a flat DTO with defaults would reset omitted fields. The DTO
   must express "absent vs. default" via `std::optional<T>` per overlayable field, and the
   generated parser must leave `nullopt` when the key is absent so the handler's
   read-modify-write still only patches present fields. Phase 2 designs each such DTO around
   `std::optional`; this is a constraint, not a fork.
