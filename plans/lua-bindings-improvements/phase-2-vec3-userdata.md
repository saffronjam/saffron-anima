# Phase 2 — a real `sa.Vec3` userdata (operators + math)

**Status:** COMPLETED

Today every vector crossing the Lua boundary is a plain `{x,y,z}` table (`getPosition`, `raycast`'s
`point`/`normal`, the contact manifold, declared `vec3` fields). That has no operators, no math helpers, and
costs a fresh GC table per call. The Roblox idiom is a real `Vector3` value type; sol2/LuaBridge3 make a
trivially-copyable userdata vector both cheaper and far nicer to script.

**No-legacy cutover (LOCKED):** this phase **replaces** the `{x,y,z}`-table representation everywhere a
vector crosses the boundary, in one change — it does not add `Vec3` alongside the tables.

## The type — registration (LOCKED)

Back it with `glm::vec3` (the module already includes `<glm/glm.hpp>`) so the math helpers are one-liners.
Bind it inside `beginNamespace("se")`. Two registration details are load-bearing and were verified against
the vendored LuaBridge3:

1. **Read-write properties via `addPropertyReadWrite`** — the single-arg `addProperty("x", &glm::vec3::x)`
   binds the **read-only** overload (`Namespace.h:886` → `push_property_readonly`), so `v.x = 5` from Lua
   would silently fail. Getters return `Vec3` userdata that scripts mutate in place, so the **read-write**
   form (`Namespace.h:920`) is required.
2. **Dual `__mul` overloads** — Lua dispatches `__mul` on the left operand's metatable first; for
   `scalar * vec` the left operand is a number (no metatable), so Lua falls to the right operand and calls
   `Vec3.__mul(scalar, vec)` — argument order `(number, vec3)`. A single `(vec, scalar)` lambda would fail
   that. Register **both** operand orders as an overload set.

```cpp
.beginNamespace("se")
  .beginClass<glm::vec3>("Vec3")
    .addPropertyReadWrite("x", &glm::vec3::x)
    .addPropertyReadWrite("y", &glm::vec3::y)
    .addPropertyReadWrite("z", &glm::vec3::z)
    .addStaticFunction("new", +[](f32 x, f32 y, f32 z){ return glm::vec3{x,y,z}; })
    .addFunction("__add",      +[](const glm::vec3& a, const glm::vec3& b){ return a + b; })
    .addFunction("__sub",      +[](const glm::vec3& a, const glm::vec3& b){ return a - b; })
    .addFunction("__mul",
        +[](const glm::vec3& a, f32 s){ return a * s; },     // vec * scalar
        +[](f32 s, const glm::vec3& a){ return a * s; })     // scalar * vec
    .addFunction("__unm",      +[](const glm::vec3& a){ return -a; })
    .addFunction("__eq",       +[](const glm::vec3& a, const glm::vec3& b){ return a == b; })
    .addFunction("__tostring", +[](const glm::vec3& a){ return std::format("Vec3({}, {}, {})", a.x,a.y,a.z); })
    .addFunction("length",     +[](const glm::vec3& a){ return glm::length(a); })
    .addFunction("normalized", +[](const glm::vec3& a){ return glm::normalize(a); })
    .addFunction("dot",        +[](const glm::vec3& a, const glm::vec3& b){ return glm::dot(a,b); })
    .addFunction("cross",      +[](const glm::vec3& a, const glm::vec3& b){ return glm::cross(a,b); })
    .addFunction("lerp",       +[](const glm::vec3& a, const glm::vec3& b, f32 t){ return glm::mix(a,b,t); })
  .endClass()
```

Plus free `se` helpers: `sa.vec3(x,y,z)` (alias for `Vec3.new`), `sa.lerp(a, b, t)`, and
`sa.look_at(eye, target, up?) -> sa.Vec3 (euler radians)` — builds a look rotation (`glm::quatLookAt`) then
decomposes via `quatToEulerZYX` (`scene.cppm:977`) so it feeds the euler `set_rotation`. This is the helper
the camera-follow recipe needs (Phase 3).

**Compile-probe note:** the metamethod overload set on a value class is less exercised than a normal method
overload — compile-probe the dual-`__mul` combination once; the single `(vec, scalar)` form definitely
builds, only the dual-arg-order overload needs the check.

**GLM layout note:** `&glm::vec3::x` relies on GLM keeping `x`/`y`/`z` as addressable named members. The
engine config (`cmake/Dependencies.cmake`, no `GLM_FORCE_XYZW_ONLY`/`GLM_FORCE_SWIZZLE`) leaves swizzle
disabled, so `vec3` is the anonymous-union layout `union { T x, r, s; }` and the member pointers are
well-formed (also true under `XYZW_ONLY`). Carry a one-line comment at the registration so a future GLM flag
flip is caught.

**Cost note (from the research):** returning `glm::vec3` by value mints a fresh ~12-byte userdata per call —
cheaper than the current 3-field table, GC-batched. Do **not** return references into components (lifetime
hazard).

## Rotation stays Euler — `sa.Quat` deferred (LOCKED)

`get_world_rotation` is a quat in `Saffron.Scene` (`scene.cppm:897`); decompose it to Euler radians at the
boundary so `sa.Vec3` carries all rotations, matching the existing euler `set_rotation`. **`sa.Quat` is
deferred** (README v1/deferred matrix) — add it only when gimbal-free composition is a real need.

## Dual-VM registration (LOCKED — both TUs)

`sa.Vec3` + `sa.vec3` must be registered in **two** places:

- `startScripts` (`script_runtime.cpp:429`) — the runtime VM where handlers run.
- `newScriptVm` (`script.cppm:230`–`246`, today only `sa.log`) — because `readScriptSchema`
  (`script_runtime.cpp:745`) runs the class chunk in a throwaway `newScriptVm`, so a `properties` default of
  `sa.vec3(0,1,0)` must resolve there. `Vec3` is pure (no host closure), so it binds cleanly in `newScriptVm`
  (unlike the host-closure bindings that force the runtime-only split).

## Threading Vec3 through the existing API (the no-legacy replacements)

Every place that builds or accepts a `{x,y,z}` table switches to `sa.Vec3`, in this change:

- **Returns:** `get_position`, `get_world_position`, and the Phase-1 `get_rotation`/`get_scale`/
  `get_world_rotation` return `sa.Vec3` (push the `glm::vec3` directly — LuaBridge converts via the
  registered class). Replace `getPosition`'s hand-built table (`script_runtime.cpp:116`).
- **`raycast` result:** `point`/`normal` become `sa.Vec3` (replace the hand-built tables in the raycast
  binding).
- **Contact handler args:** `on_contact(self, other, point, normal)` — `point`/`normal` become `sa.Vec3`
  (replace `pushVec3Table`, `script_runtime.cpp:255`, called at `:287`–`:288`).
- **Setters take a single `Vec3`:** `set_position(v)`, `set_rotation(v)`, `set_scale(v)`. The three-float
  overloads are **retired** — this setter-signature change forces every e2e `set_position(x, y, z)` call
  (`script.test.ts:51`–`52`, `:62`–`63`, `:73`, `:82`–`83`, `:109`, `:123`–`124`, `:136`, `:159`–`160`) to
  become `set_position(sa.vec3(...))` / `set_position(p + …)`, and the scaffold orbit math + its
  `Math.hypot` assertion (`:416`) to rebuild `self.center` from `Vec3` — all in this change.
- **Declared `vec3` fields:** `Class.properties = { offset = sa.vec3(0,1,0) }`. `inferField`
  (`script_runtime.cpp:699`) currently keys on `LUA_TTABLE && lua_rawlen == 3` (`:720`). Two edits:
  - **Detection:** also recognize an `sa.Vec3` **userdata** default (a metatable/class check), emitting the
    same `Vec3` `ScriptFieldType`.
  - **Extraction:** build the 3-number JSON array from the userdata's `x`/`y`/`z` (LuaBridge getters or a
    direct cast), **not** `lua_rawgeti` — otherwise the array silently emits zeros. The wire shape stays the
    3-number array the Inspector + override storage expect.
  - `injectFields` (`:359`) constructs a **fresh `Vec3` per instance** by value-pushing the registered value
    type. The `pushTableCopy` table-only branch (`:378`, `lua_type == LUA_TTABLE`) no longer fires for a
    `Vec3` default (it is userdata) — rewire that branch so a userdata default is value-pushed per instance,
    preserving the no-shared-default-aliasing guarantee `pushTableCopy` exists for. Overrides stored as a
    3-number JSON array still round-trip (the Inspector's vec3 widget is unchanged on the wire).

## Watch-out: `luaToJson` Vec3 branch

`luaToJson` (Phase 1) must serialize an `sa.Vec3` userdata to a 3-number array (the `[x,y,z]` shape the
vec3 serde reads) so `set_component("Transform", { translation = pos })` works with a `Vec3` value. Add the
userdata branch in this phase.

## Tests (`tests/e2e/script.test.ts`)

- `local v = sa.vec3(1,2,3) + sa.vec3(0,1,0); self.entity:set_position(v)` → inspect shows `(1,3,3)`. Assert
  `(sa.vec3(3,0,0)):length()` ≈ 3, `dot`, `cross`, `normalized`, and **`2 * sa.vec3(1,1,1)`** (scalar\*vec
  path) equals `sa.vec3(2,2,2)`.
- `get_position()` returns userdata (`p.x`/`p.y`/`p.z` read back **and** `p.x = 9` writes through);
  `tostring(p)` is `Vec3(...)`.
- A declared `offset` field of `sa.vec3(0,1,0)` still surfaces as a `vec3` schema field and injects as a
  `Vec3` (`self.offset.y == 1`). Update any existing declared-field test from `self.offset[2]` to
  `self.offset.y`.

## Docs

Rewrite the vector parts of `script-components-and-runtime.md` (positions are now `sa.Vec3`, not tables) and
add an `sa.Vec3` reference (operators + math helpers + the read-write fields). Flag the scaffold `example.lua`
Vec3 usage here so Phase 7 (which owns the scaffold rewrite) stays consistent. Update `_index.md` if scope
shifts.

## Constraints honored

NO-LEGACY (single Vec3 cutover, no second vector shape), Saffron.Script imports only Core+Scene (glm is a
Core dependency, no new import), sandbox unchanged, errors stay logged-no-op. No generated file is
hand-edited.

## Verification gate

`make engine`, `make prepare-for-commit`, `make e2e` green; the schema VM (`readScriptSchema`) still reads
`vec3` defaults (the dual-VM registration); the dual-`__mul` compiles.
