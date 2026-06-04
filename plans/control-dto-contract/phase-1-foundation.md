# Phase 1 — Foundation

**Status:** IN PROGRESS

**Depends on:** phase 0

## Goal

Land the machinery the whole migration rides on: the `WireUuid` / `EntitySelector` wire
types, the `:Dto` partition with a handful of pilot DTOs, the bun generator v1 (parser +
C++ serde emitter + freshness gate), the typed `registerCommand<Params, Result>` overload,
and 2–3 pilot commands migrated end to end. After this phase one real command is driven
through a generated parser and serializer, the generator round-trips, and `check.sh` enforces
freshness — but the other 59 commands are untouched and still pass through the raw path.

## Steps

### Wire types

1. **`WireUuid`.** In the `:Dto` partition, a struct wrapping `u64 value`. Its only wire
   encoding is a decimal string: the generated `toJson(WireUuid)` routes through `uuidToJson`
   (`json.cppm:72`), and `parse` of a `WireUuid` field routes through the `jsonU64` logic
   (`json.cppm:92`, accepts string or number). Internal code reads `.value` to get the `u64`
   it hands to `resolveEntity` / the registry. Replace `entityRef`'s bare `std::to_string`
   (`control_server.cpp:134`) with `WireUuid` so the two id encodings unify.

2. **`EntitySelector`.** The wire type for `resolveEntity`'s id-or-name input. The generated
   parser keeps the raw selector json and the handler passes it to the **existing**
   `resolveEntity(ctx, params)` (`control_server.cpp:72`) — phase 1 does not reimplement
   resolution, it only types the field so the parser does not abort on a string-or-number.

### The `:Dto` partition

3. **Create `source/saffron/control/control_dto.cppm`** as `export module
   Saffron.Control:Dto;`, classic `#include <nlohmann/json.hpp>` in the global module
   fragment, **no `import std`**, importing `Saffron.Core` + `Saffron.Json` (`Core` is
   already in the `:Command` partition's set; `Json` is a new import for the partition, used
   today only by the implementation `.cpp`s — `command.cppm:14-19`). Add it to
   `engine/CMakeLists.txt`'s `FILE_SET CXX_MODULES` `FILES` list (`engine/CMakeLists.txt:8-26`).
   **List position is irrelevant** — CMake scans every module source and derives the BMI
   build order from the `import`/`export` graph, so the `FILES` order is not a build order;
   no entry needs to precede `command.cppm`. Keep it `CXX_MODULE_STD ON` / `gnu++26` like its
   siblings; do **not** set `CMAKE_CXX_EXTENSIONS OFF` (the std-module BMI would be rejected).

4. **Pilot DTOs in the partition.** Hand-write the param + result structs for the pilot
   commands chosen in step 8, in the restricted subset (phase 0). These are the only
   hand-written DTOs the parser reads; the serde body is generated.

### The generator v1

5. **`tools/gen-control-dto/gen.ts`**, a self-contained `#!/usr/bin/env bun` script with
   zero deps and no `bun install`, mirroring `tools/check-control-schema/check.ts`'s shape.
   It parses `control_dto.cppm` textually (the restricted grammar from phase 0), validates
   every field type against the vocabulary (reject + nonzero exit on anything else), and
   emits, deterministically (sorted iteration, stable formatting):
   - a generated C++ serde TU (`control_dto_serde.cpp`, a non-module `module
     Saffron.Control;` implementation unit) with one `auto parse<Params>(const Json&) ->
     Result<Params>` and one `toJson(const ResultDto&) -> Json` per DTO. Each `parse`
     emits **per-field, type-checked extraction routed through the `json.cppm` gateway**
     (never raw `.at()` / `.get<T>()` / `NLOHMANN_DEFINE_TYPE_*`) and a per-field positional
     fallback matching `positionalOr`. Output is clang-format stable (the tree runs
     `.clang-format` via `make format`).
   - register `control_dto_serde.cpp` in `engine/CMakeLists.txt`'s `PRIVATE` sources
     (`engine/CMakeLists.txt:28-44`), not the module file set.

   **`json.cppm` helper inventory — what exists vs. what the generator must add.** The
   gateway exports `Result`-returning (error-on-mismatch) reads for only four types:
   `jsonU64`, `jsonString`, `jsonF64`, `jsonBool` (`json.cppm:42-45`). It also exports
   `value-or-default` fallbacks `jsonU64Or` / `jsonStringOr` / `jsonF32Or` / `jsonBoolOr`
   (`json.cppm:49-52`) — but those swallow a type mismatch into the fallback, so they are
   **wrong** for a required field that must surface a parse error, and `jsonF32Or` is the
   only float-narrowing helper (there is **no** `Result`-returning `f32`/`i32`/`u32` read at
   all). The DTO vocabulary has `i32`, `u32`, `f32` as first-class field types, so the
   generator's emitted parser must supply small `std::expected`-returning reads for them
   that the gateway lacks:
   - `f32`: read via `jsonF64` (the existing `Result<f64>`) then `static_cast<f32>` — do
     **not** route a required `f32` through `jsonF32Or`, which hides a wrong type.
   - `i32` / `u32`: there is no `jsonI32` / `jsonU32` today. Emit a checked read that
     verifies `is_number_integer()` (and, for `u32`, non-negative + in range) before
     narrowing — mirroring the range checks `jsonU64` already does for the string/signed
     cases (`json.cppm:99-120`). Add these as generator-emitted helpers (in the generated
     TU, or as a tiny hand-written addition to `json.cppm` if shared) rather than assuming
     the gateway provides them.
   - `WireUuid`: route through `jsonU64`; `bool`/`std::string`/`u64` map to the existing
     `jsonBool` / `jsonString` / `jsonU64`. Optional fields (`std::optional<T>`) leave the
     field `nullopt` when the key is absent and only error on a present-but-wrong-typed
     value.

6. **Wire the generator into `check.sh`** before the engine build
   (`tools/ci/check.sh`, the `cmake --build` step): run `( cd tools/gen-control-dto && bun
   run gen.ts )`, then `git diff --exit-code` on the generated C++ TU + `se-types.ts` so a
   stale committed artifact **fails** the gate. This adds the freshness check the existing
   `index.ts` lacks (there is no diff step today — README "Current anchors"). Determinism is
   load-bearing: sorted iteration + clang-format-stable output, or the diff flaps.

### The typed overload

7. **`registerCommand<Params, Result>(reg, name, help, handler)`** declared in
   `command.cppm` and defined in `control_server.cpp` (or the generated serde TU if it needs
   the per-DTO functions). It builds the erasure thunk from phase 0 — `parse<Params>` →
   `handler(ctx, params)` → `toJson(result)` — and forwards to the existing
   `registerCommand` (`control_server.cpp:32`). `CommandTraits` (`command.cppm:37`),
   `CommandRegistry`, and `dispatch` (`control_server.cpp:213`) are unchanged; the new
   overload only adds a templated front door.

### Pilot migration

8. **Migrate 2–3 pilot commands end to end.** Pick a regular result-returning one, a flag
   toggle, and one selector-taking command so the pilot exercises `WireUuid`,
   `EntitySelector`, and positional sugar:
   - `ping` (no params; result `{pong, engine, version, pid}` — `control_commands_render.cpp`)
     — proves the result-serialize path with zero param risk.
   - `create-entity` (`name` positional; result `entityRef`,
     `control_commands_scene.cpp`) — proves positional params + a `WireUuid`-carrying result.
   - `set-exposure` (`ev` positional numeric; result `{exposureEv}`,
     `control_commands_render.cpp`) — proves an `f32` param + a scalar result.
   Each pilot keeps its exact wire shape; the contract test and `se` CLI must behave
   identically before and after.

## Validation

- `cmake --build build/debug -j1` green with the new partition + generated TU (watch the
  module-BMI ICE history — the partition adds one BMI node; build serially as mandated).
- `check.sh` green, including the new regenerate-and-diff step: a deliberately stale
  generated TU makes the gate fail; regenerating makes it pass.
- The 3 pilot commands return byte-identical wire output to the pre-migration build (diff
  `se ping -o json`, `se create-entity foo -o json`, `se set-exposure 1.5 -o json`).
- Malformed input does **not** abort: `se` (or a raw socket write) sending
  `set-exposure` with `ev` as a non-number returns an `ok:false` error, not a host crash —
  confirms the generated parser's type-checks, not `.get<T>()`.
- An e2e case (`tests/e2e`, `make e2e`) drives a pilot command with a bad param and asserts a
  validation-clean log + an `ok:false` envelope (the contract test validates output only; the
  new input-parse path needs an e2e guard — README/JSON-BUILD risk).

## Risks

- **`JSON_NOEXCEPTION` abort.** Any generated `.at()` / `.get<T>()` / `operator[]` on
  malformed input is `std::abort()` (`cmake/Dependencies.cmake:118`; `json.cppm:17-21`). The
  generator MUST emit `is_*()`-guarded extraction through the `json.cppm` helpers; this is a
  correctness landmine, not a compile error, so the malformed-input e2e case above is the
  guard.
- **Module BMI / `-j1`.** The `:Dto` partition is a new `CXX_MODULES` interface unit (CMake
  derives its build position from the `import`/`export` graph, so its place in the `FILES`
  list does not matter); the generated serde stays a non-module `.cpp` so it is a leaf and
  does not deepen the BMI scan. Generating an in-CMake `add_custom_command` feeding a *module*
  source is fragile (the scanner reads sources eagerly) — generate via `check.sh` writing the
  committed file instead, exactly as `gen-protocol` writes `index.ts`.
- **Determinism vs. diff-check.** A non-deterministic generator makes the new
  `git diff --exit-code` flap. Sort all iteration and emit clang-format-stable output, the
  same discipline `gen-protocol` uses (sorted file list).
- **Erasure must not double-wrap the envelope.** The thunk returns the bare result payload
  (so `dispatch` adds `{ok,result}`); a generated `toJson` that wraps its own envelope would
  produce `result: {ok:...}`. Pin the thunk contract in step 7 and assert it in the pilot
  diff.
