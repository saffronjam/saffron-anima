# Phase 8 — `read_script_schema` and the Inspector field contract

**Status:** COMPLETED

**Depends on:** 12-scripting:phase-2-value-types-and-binding-table

## Goal

Port `read_script_schema`: in a throwaway sandboxed VM (no gameplay runs), load a script, read its
declared `properties` table, infer each field's edit-time type from its default, and return the fields
sorted by name. This is what feeds the editor Inspector (the per-slot field UI + override storage) via
the `get-script-schema` control command, whose `GetScriptSchemaResult` DTO this phase ties into.

## Why this shape (NO LEGACY)

- **A throwaway sandboxed schema VM, side-effect-free.** C++ ran `readScriptSchema` in a fresh
  `newScriptVm` (sandboxed, value types registered so a `sa.vec3(...)` default resolves), loaded+ran the
  chunk to get the class table, and only read `properties` — the declaration must build tables, not run
  gameplay (`readScriptSchema`, `script_runtime.cpp:1565`–1617). In Rust this is a phase-1 sandboxed
  `Lua` + phase-2's `register_value_types` (no scene, no host bridge), running the file and reading the
  `properties` table.
- **Field-type inference from the default value.** `infer_field` maps the default at the top of the
  stack: number → `Number`, boolean → `Bool`, string → `String`, an `sa.Vec3` userdata → `Vec3`
  (captured as a 3-number JSON array — the shape the Inspector + override storage use). Anything else is
  **not a field** (skipped with a logged note) (`inferField`, `script_runtime.cpp:1533`–1562). The
  `ScriptField {name, type, default_value}` + `ScriptFieldType {Number, Bool, String, Vec3}` enum port
  directly; `script_field_type_name` returns `"number"|"bool"|"string"|"vec3"` (`script.cppm:178`–201,
  `script_runtime.cpp:1512`–1526). Fields come back sorted by name.
- **The Inspector contract is the `GetScriptSchemaResult` DTO.** The host's `get-script-schema` command
  maps each `ScriptField` to a `ScriptFieldDto {name, type, default}` and returns
  `GetScriptSchemaResult {fields}` (`host.cppm:1009`–1034, `control_dto.cppm:637`–651). Per the
  one-registration-place rule and 08-host-and-viewport's decision, the **command registers at the host**
  (it needs the Lua schema reader, and the host is the only crate that may import scripting); this phase
  owns the `read_script_schema` function + the `ScriptField`/`ScriptFieldType` types and confirms they
  map cleanly to the area-10 `GetScriptSchemaResult`/`ScriptFieldDto` DTOs. The `default` field rides as
  an opaque `serde_json::Value` (a scalar or a 3-number array for vec3), matching the C++
  `nlohmann::json defaultValue`.
- **The override storage shape is the `ScriptSlot.overrides` `Value`.** `inject_fields` (phase 5) reads
  these overrides; a vec3 override is a 3-number array, a scalar is the scalar — the same shape this
  phase emits as the default, so the Inspector round-trips default → override → inject consistently.

## Grounding (real files / symbols)

- `engine-old/source/saffron/script/script_runtime.cpp`: `readScriptSchema` (1565–1617), `inferField`
  (1533–1562), `scriptFieldTypeName` (1512–1526), `isVec3Userdata`/`readVec3Userdata` (740–770).
- `engine-old/source/saffron/script/script.cppm`: `ScriptField`/`ScriptFieldType` (178–201).
- `engine-old/source/saffron/host/host.cppm`: the `get-script-schema` command (1009–1034).
- `engine-old/source/saffron/control/control_dto.cppm`: `GetScriptSchemaParams`/`ScriptFieldDto`/
  `GetScriptSchemaResult` (637–651) — the area-10 DTOs (`saffron-protocol`).
- 03-ecs-and-scene: `ScriptSlot.overrides` (the override `Value` shape `inject_fields` consumes).

## Acceptance gate

- `cargo build --workspace` succeeds; `#![deny(unsafe_code)]`; clippy + fmt clean.
- `#[test]`: a fixture `.luau` with `properties = { speed = 5, name = "x", on = true, offset =
  sa.vec3(0,1,0) }` yields fields `name`(string), `offset`(vec3, default `[0,1,0]`), `on`(bool),
  `speed`(number), sorted by name; a `properties` entry with an uninferable default (a function/table)
  is skipped.
- `#[test]`: a script with no `properties` table yields an empty field list; a load/run failure returns
  a typed `Err`; the schema VM never runs `on_create`/`on_update` (no gameplay side effects).
- `#[test]`: each `ScriptField` maps to a `ScriptFieldDto` (`name`, `type` string, `default` `Value`) and
  the `default` for a vec3 is a 3-number JSON array — the shape the Inspector + `ScriptSlot.overrides`
  round-trip.
