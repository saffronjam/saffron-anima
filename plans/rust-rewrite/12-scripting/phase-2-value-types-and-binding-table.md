# Phase 2 — The `sa.Vec3` value type and the declarative binding-descriptor table

**Status:** COMPLETED

**Depends on:** 12-scripting:phase-1-vm-sandbox-budget, 02-math-and-geometry

## Goal

Build the `sa.Vec3` value userdata (the only value type scripts construct) with its math + operator
surface, and stand up the **declarative `sa.*` binding-descriptor table** — the single source that both
registers the API with the VM and feeds area 9's `.luau` type emitter. This phase wires the value type +
the *table shape* and a registration walk that registers the free functions / globals it can register
without the scene (`sa.vec3`, `sa.lerp`, `sa.look_at`, `sa.log`); the entity methods and scene-dependent
globals are added to the same table in later phases.

## Why this shape (NO LEGACY)

- **`sa.Vec3` is a `glam::Vec3`-backed `UserData`.** The C++ bound `glm::vec3` directly as a LuaBridge
  value class with `addPropertyReadWrite` x/y/z and a metamethod set (`registerScriptValueTypes`,
  `script_runtime.cpp:992`–1058). In Rust, a `UserData` impl on a `Vec3` newtype (or `glam::Vec3`
  wrapper) exposes `x`/`y`/`z` fields (`add_field_method_get`/`_set`) and methods `length`,
  `normalized`, `dot`, `cross`, `lerp` (`add_method`), plus the metamethods `Add`/`Sub`/`Mul`/`Unm`/
  `Eq`/`ToString` (`add_meta_method`). `glam`'s xyzw quaternion deletes the GLM swizzle hazard; the
  `look_at` helper returns euler-ZYX radians (`quat_to_euler_zyx(quat_look_at(dir, up))`) so it feeds
  `set_rotation` (`script_runtime.cpp:1046`).
- **The dual-operand `__mul` collapses to one `add_meta_method`.** C++ needed a raw `lua_CFunction`
  thunk to handle `scalar * vec` and `vec * scalar` because a class member rejects a non-class first arg
  (`script_runtime.cpp:1010`–1022). `mlua`'s `MetaMethod::Mul` handler receives both operands as
  `mlua::Value` and dispatches on which is the number — one safe handler, no thunk (the exact
  binding-DSL simplification feasibility 4.4 calls out).
- **The binding-descriptor table is the single source (the re-evaluated declarative registry).** Per
  README §3: an explicit ordered table of descriptors (name, arg types, return type, doc), expressed as
  Rust data. The registration walk binds each entry; area 9's xtask emitter reads the same table to emit
  the Luau defs. This is the shape the C++ plan rejected for LuaBridge3 (deduced-type thunks made it
  "strictly worse") but which `mlua`'s typed `IntoLua`/`FromLua` makes the right answer — there is no
  second hand-written copy to drift, so `check-script-defs` is deleted (README §3).
- **Value types register in BOTH the runtime VM and the throwaway schema VM.** C++ called
  `registerScriptValueTypes` in both `newScriptVm` and `startScripts` so a `properties` default of
  `sa.vec3(0,1,0)` resolves at edit time too (`script_runtime.cpp:989`). The Rust `register_value_types`
  + the no-scene-dependency descriptors register the same way (phase 1's VM + phase 8's schema VM both
  call it).
- **No proc-macro.** The table is explicit and ordered (the PP-7 / area-10 discipline): emit order is
  deterministic, the whole set is needed at once by the emitter. A `binding!` helper trims per-entry
  boilerplate without hiding the set.

## Grounding (real files / symbols)

- `engine-old/source/saffron/script/script_runtime.cpp`: `registerScriptValueTypes` (the `sa.Vec3` class
  + the `vec3`/`lerp`/`look_at` free functions, 992–1058), the dual-operand `__mul` thunk
  (1010–1022), `readVec3Userdata`/`isVec3Userdata` (740–770).
- `engine-old/source/saffron/script/script.cppm`: `sa.log` bound directly on the global table in
  `newScriptVm` (285–288) — the base log (phase 7 overrides it with the log-sink variant).
- area 10 phase-6 README: the shared `Rust-type → Luau` mapper this table's emitter (phase 9) calls.

## Acceptance gate

- `cargo build --workspace` succeeds; `#![deny(unsafe_code)]`; clippy + fmt clean.
- `#[test]`: in a VM, `sa.vec3(1,2,3)` round-trips x/y/z; `a + b`, `a - b`, `2 * v`, `v * 2`, `-v`,
  `a == b`, `tostring(v)`, `v:length()`, `v:normalized()`, `a:dot(b)`, `a:cross(b)`, `a:lerp(b,t)`,
  `sa.lerp(a,b,t)`, `sa.look_at(eye,target,up)` all evaluate to the values `glam` computes (a fixed
  vector compared to the direct Rust math).
- `#[test]`: the binding-descriptor table contains every value-type/no-scene `sa.*` entry, each with a
  resolved Rust arg/return type, in a stable order across builds; registering the table into a VM makes
  those globals callable.
- `#[test]`: `sa.vec3` resolves in a sandboxed schema-style VM (value types registered without a scene).
