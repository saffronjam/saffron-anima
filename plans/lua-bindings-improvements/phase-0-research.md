# Phase 0 — SOTA research notes + the binding-table decision record

**Status:** COMPLETED (research captured; informed the design and Phases 1–7)

The grounding for every later phase. Three reference scripting layers, then the binding-idiom
literature, then the decision on hand-written vs generated `sa.lua`.

## Roblox / Luau

- **Property access is direct assignment on userdata:** `part.Position = Vector3.new(0, 10, 0)`,
  `part.Name`, `part.BrickColor`. The engine exposes object instances as userdata whose properties are
  get/set through metamethods, and `Vector3` is a **real value type** with operators (`a + b`, `v * 3`,
  `v:Dot(w)`, `v:Cross(w)`, `v.Magnitude`, `v.Unit`).
- **Events use a connect model:** `signal:Connect(function(...) … end)` returns a connection
  (`:Disconnect()`); `Instance.Changed` fires on any property change, and
  `Object:GetPropertyChangedSignal(prop): RBXScriptSignal` returns a per-property signal that only fires
  for that property (and returns the *same* signal object on repeated calls for the same property — a
  cache, not a fresh allocation). The lesson: an event surface is a first-class object you connect
  callbacks to, not a polled flag.
- **The `task` scheduler is cooperative coroutines, not OS threads:** `task.wait(dt)` yields the running
  thread until `dt` seconds have elapsed (resumes on the next Heartbeat past the deadline);
  `task.spawn(fn, …)` resumes a function/coroutine immediately in the current phase; `task.defer(fn)`
  resumes it at the end of the current resume point this frame; `task.delay(dt, fn)` resumes after a
  delay. All are sugar over `coroutine.create/resume` driven by the engine's frame loop. The lesson:
  build timers on the coroutine lib + the tick, never on wall-clock sleeps.
- **API dump → autocomplete:** the Roblox API types are *partly generated, partly hand-written* from the
  API dump + docs, and `luau-lsp` (the open Luau language server) consumes them for autocomplete, hover,
  and `--!strict` checking. The lesson maps onto LuaLS for us: ship a definition file the editor's
  language server reads.

## LÖVE (love2d)

- A **flat callback model**: `love.load()` once at start, `love.update(dt)` every frame,
  `love.draw()` to paint, plus input callbacks `love.keypressed(key)`, `love.mousepressed(x, y, button)`,
  etc. `love.run` is the overridable main loop; the framework provides blank placeholders you override by
  defining a same-named function.
- Maps cleanly onto our **per-instance handler shape**: `on_create(self)` ≈ `love.load`,
  `on_update(self, dt)` ≈ `love.update`, `on_contact/on_trigger_*` ≈ the input/event callbacks. We keep
  the colon-method/`self`-table form (per-entity state) rather than LÖVE's flat globals — the right
  choice for an ECS where many entities run the same script.

## Binding idioms — sol2 / LuaBridge3

- **Userdata classes:** `beginClass<T>("Name") … endClass()` (LuaBridge3) /
  `lua.new_usertype<T>("Name", …)` (sol2). The library builds the metatable; the C++ object lives inside a
  Lua-GC'd userdata block.
- **Operator overloading is metamethods.** LuaBridge3: `.addFunction("__add", +[](const Vec3& a, const
  Vec3& b){ return a + b; })` and likewise `__sub`, `__mul`, `__unm`, `__eq`, `__tostring`, `__len`.
  sol2: `sol::meta_function::addition` → a lambda. Lua dispatches `a + b` through `__add` on either
  operand's metatable.
- **Constructors / static factories.** LuaBridge3: `.addConstructor<void(float,float,float)>()` makes
  `Vec3(x,y,z)` callable as a Lua constructor, **or** `.addStaticFunction("new", +[](float x,float y,float
  z){ return Vec3{x,y,z}; })` for the Roblox-style `Vec3.new(x,y,z)`. Prefer the static `new` factory —
  it matches the Roblox idiom scripters expect and reads better than calling the type as a function.
- **Properties.** `.addProperty("x", &Vec3::x, &Vec3::x)` (read-write, getter+setter pointers) or
  `.addProperty("y", &Vec3::y)` (read-only). For a trivial POD vector, member pointers are enough.
- **Value vs reference is the cost story.** A bound function that **returns by value** (`Vec3 f()`) mints
  *fresh userdata each call* — a heap allocation plus GC bookkeeping. Returning by `&`/`*` holds a pointer
  into a C++ object (no copy, but the C++ object must outlive the Lua reference). For a per-frame math
  type that is created/destroyed thousands of times a tick, **value semantics on a tiny trivially-copyable
  struct is correct** (the userdata block is `sizeof(Vec3)` ≈ 12 bytes, cheap), and the GC reclaims them in
  batches. The anti-pattern is returning **tables** for vectors (what we do today): `lua_createtable` +
  three `settable` per vector, a fresh GC table object every call, and no operator support. A userdata
  Vec3 is *both* cheaper per-op (no field-name hashing) *and* gives operators.
- **`Stack<T>` / `push`:** LuaBridge3 routes every C++→Lua conversion through a `Stack<T>` specialization;
  the default for a registered class pushes userdata, a custom specialization can push a table. The
  takeaway: a Vec3 userdata is registered once and every `glm::vec3`-shaped return can convert through it
  uniformly (one `Stack<glm::vec3>`-style seam), instead of hand-building a table at each call site as
  `getPosition`/`raycast` do today.

## Decision record — `sa.lua`: hand-written vs generated

**Context.** The repo's precedent (`tools/gen-control-dto/gen.ts`) is one-source-of-truth: the DTO file
is authored once, and C++ serde + `@saffron/protocol` TS + OpenRPC + the manifest are all generated; the
contract test refuses to pass if the generated artifacts are stale. The Lua binding surface has the same
drift hazard: a hand-written `sa.lua` silently diverges from the imperative `.addFunction(...)` calls.

**Options weighed.**

1. **Hand-write `sa.lua` now.** Fast, no tooling. Drifts the moment someone adds an `.addFunction` and
   forgets the def file. Drift is invisible (autocomplete just lacks an entry; no test fails).
2. **One declarative binding table** consumed by *both* the LuaBridge registration loop *and* an `sa.lua`
   generator. Zero drift by construction (the `gen.ts` model). Cost: a non-trivial binder — every binding
   becomes a table row with a name, a C++ thunk, and a typed signature, and the registration loop reads
   the table instead of fluent `.addFunction` chains. That is a real rewrite of `startScripts`'s
   registration block and a new generator.

**Decision: phase it.** Hand-write `sa.lua` in Phases 1–6 **and** add a cheap runtime tripwire to
`tools/check-control-schema/check.ts` (or a new `tools/check-script-defs/`): boot the engine, introspect
the live `se` global table + the `sa.Entity` metatable for every exposed name, parse `sa.lua` for its
`---@field`/method names, and **fail if the live surface has a name the def file lacks** (and warn on the
reverse). This catches the only failure mode that matters (a binding with no annotation) with ~30 lines
and no binder rewrite. Then in **Phase 7**, once the surface has stabilized across Phases 1–6, lift the
list into one declarative table that the registration loop and an `sa.lua` generator both consume — at
which point the tripwire becomes a "generated file is fresh" check exactly like the DTO one, and the
hand-written `sa.lua` is deleted (no-legacy: the generated file replaces it).

**Why not table-first immediately:** the high-value pure bindings (component write, Vec3, lifecycle)
should not wait on a binder rewrite, and designing the declarative schema is easier once the real surface
exists. The tripwire makes the interim safe.

## Sources

- Roblox `Vector3` / property access — https://create.roblox.com/docs/reference/engine/datatypes/Vector3
- `GetPropertyChangedSignal` / `:Connect` — https://create.roblox.com/docs/reference/engine/classes/Object ,
  https://devforum.roblox.com/t/new-api-instancegetpropertychangedsignal/36212
- `task` scheduler — https://create.roblox.com/docs/reference/engine/libraries/task ,
  https://create.roblox.com/docs/scripting/scheduler
- Luau types + API dump + `luau-lsp` — https://luau.org/types/ , https://github.com/JohnnyMorganz/luau-lsp
- LÖVE callbacks — https://love2d.org/wiki/Tutorial:Callback_Functions , https://love2d.org/wiki/love.run
- LuaBridge3 idioms — https://kunitoki.github.io/LuaBridge3/Manual ,
  https://github.com/kunitoki/LuaBridge3/blob/master/Manual.md
- sol2 usertypes / meta_function / userdata memory — https://sol2.readthedocs.io/en/latest/usertypes.html ,
  https://sol2.readthedocs.io/en/latest/api/usertype_memory.html , https://github.com/ThePhD/sol2/issues/418
- LuaLS meta files / `.luarc.json` / `workspace.library` —
  https://github.com/LuaLS/lua-language-server/wiki/Libraries ,
  https://github.com/LuaLS/lua-language-server/wiki/Configuration-File , https://luals.github.io/wiki/configuration/
