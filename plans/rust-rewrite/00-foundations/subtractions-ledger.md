# Subtractions ledger — what the Rust port deletes or collapses

**Status:** LOCKED (design annex, no implementation phases)

This is the honest scope-shrink: a single accounting of every C++ artifact the Rust rewrite **deletes**
or **collapses** into a derive / a language feature / a crate boundary, plus the inverse — the handful of
things Rust **forces us to add** that have no C++ counterpart. It exists so a reviewer reading the
implementation phases knows what *not* to look for (no port phase will ever recreate these) and can sanity-
check that a subtraction was actually taken, not silently transliterated.

Every row is grounded in a symbol read from `engine-old/`. LOC counts are `wc -l` of the real file or a
measured span, not estimates. The rule everywhere: **NO LEGACY** — a subtraction is taken in the phase
that supersedes it, never deferred. A feature is not done while its deleted C++ apparatus still exists in
the tree (the tree only frees it at cutover when `engine-old/` is removed, but no Rust crate recreates it).

This ledger has no phases of its own. The deletions land inside the area phases that supersede them; this
doc is the cross-cutting tally those phases point back at. References to the apparatus this annex consumes:
[`00-foundations/conventions.md`](./conventions.md), [`00-foundations/dependency-adoption.md`](./dependency-adoption.md),
and the build area [`01-build-and-toolchain/`](../01-build-and-toolchain/).

---

## 1. Build / toolchain apparatus that vanishes (Cargo subsumes it)

The single largest, most unambiguous win of the rewrite. None of this is "ported" — Cargo + `build.rs` +
`xtask` make the entire C++26-modules + `import std` + BMI-matching build machine unnecessary. The toolbox
container itself stays (Vulkan SDK, `slangc`, SDL3 substrate, headless weston) — only the build *mechanism*
inside it is replaced.

| Removed apparatus | Where it lives | Rust replacement |
|---|---|---|
| The experimental `import std` UUID gate (`CMAKE_EXPERIMENTAL_CXX_IMPORT_STD = d0edc3af-…`) + `CMAKE_CXX_STANDARD 26` + the gnu++26 BMI-match constraint | `CMakeLists.txt:3-13` | Gone — Rust's `std` is always present, no per-target opt-in, no BMI to match. Edition pinned once in `[workspace.package]` (PP-1 `fileConventions`). |
| Per-target `CMAKE_CXX_MODULE_STD ON`; the named-module BMI dependency graph; the rule that consumers must compile `gnu++26` or the BMI is rejected (`CMAKE_CXX_EXTENSIONS` left ON) | `engine-old/CMakeLists.txt`, every `*.cppm` header comment ("classic includes, no `import std`") | Gone — Cargo's crate = compilation unit; `rlib` metadata replaces `.pcm` BMIs; no mixing rule, no "this module does NOT `import std`" discipline. The 15-module DAG re-encodes as the workspace crate graph (PP-1 `crateGraph`). |
| The two-ninja `.pcm` Bus-error race (a single shared `build/debug` corrupts another ninja's mmap-read `.pcm`) | `AGENTS.md:62-63,79` (the concurrent-builds hazard) | Gone — Cargo has no shared-mutable BMI file another process mmap-reads; `cargo build -jN` is the only parallelism and is race-free. The whole "private `build/<name>` dir per agent" workaround disappears. |
| `FetchContent_Declare`/`MakeAvailable` for EnTT, glm, VulkanMemoryAllocator, vk-bootstrap, nlohmann_json, lua, LuaBridge3, JoltPhysics (8 declarations) | `cmake/Dependencies.cmake:13-88` | `[workspace.dependencies]` pins from crates.io (or vendored for vk-mem/Jolt), `dep.workspace = true` per member (PP-1 `fileConventions`). One pin table replaces eight `FetchContent` blocks + the version-tag plumbing. |
| `CMakePresets.json` (`debug`/`release` pinning clang++, `-stdlib=libc++`, lld, Ninja) | `CMakePresets.json` (37 LOC) | `[profile.dev]`/`[profile.release]` in the virtual workspace (PP-12 owns the section; PP-1 reserves it). No compiler/linker/stdlib pin — `rustc` + the default linker. |
| `enable_language(C)` (added solely for Lua's C build) | `cmake/Dependencies.cmake:41` | Gone — `mlua` (luau, vendored) brings its own build; no C TU in the workspace except the two FFI build scripts' own invocations. |
| The five hand-written single-header impl TUs: `vma_impl.cpp`, `stb_impl.cpp`, `cgltf_impl.cpp`, `tinyobjloader_impl.cpp`, `nanosvg_impl.cpp` + the `saffron_third_party` INTERFACE aggregate library that links them | `cmake/Dependencies.cmake:113-160`, `cmake/*_impl.cpp` | Gone — `vk-mem`, `image`/(stb optional), `gltf`, `tobj`, `resvg` are crates with their own build; no `#define …_IMPLEMENTATION` TU to author, no aggregate target. |
| The `-pthread`/`-mavx2` flag isolation on `physics.cpp` (Jolt's INTERFACE options must NOT reach every TU because `-pthread` flips the per-target `import std` POSIX-thread langopt and would reject the std BMI) — incl. `list(REMOVE_ITEM SAFFRON_JOLT_COMPILE_OPTIONS "-pthread")` | `cmake/Dependencies.cmake:91-109` | Collapses to a crate boundary: `saffron-physics-sys`'s `build.rs` applies the Jolt determinism + arch flags to **only its own TUs** (PP-11 / `01-build-and-toolchain:phase-4`). The `-pthread`-vs-`import std` special case disappears entirely — there is no BMI to protect; only the `-ffp-model`/`-mavx2`/`CROSS_PLATFORM_DETERMINISTIC` set (which is ABI/determinism-relevant) is kept, confined by `build.rs`. |
| `GLM_FORCE_DEPTH_ZERO_TO_ONE` global compile-def | `cmake/Dependencies.cmake:163` | Gone — `glam` has no global depth flag; per-call `Mat4::perspective_rh` (02-math-and-geometry). |
| `JSON_NOEXCEPTION` compile-def (on `saffron_third_party` and on `tools/sa`) | `cmake/Dependencies.cmake:163`, `tools/sa/CMakeLists.txt:6` | Gone — see §3 (serde returns `Result`, no abort firewall). |

**Net:** the entire `cmake/` build layer + `CMakePresets.json` + every `.cppm` "does/doesn't `import std`"
discipline note is reference-only; the Rust workspace re-expresses the build with `Cargo.toml` profiles, two
`build.rs` (the FFI seams) and one `xtask` (shaders + codegen). No phase recreates any of it.

---

## 2. Generated / codegen apparatus that collapses to derives

The C++ wire/serde surface is machine-generated by a bespoke TypeScript regex parser feeding two large
generated `.cpp` files, kept honest by a hand-synced overlay and a drift tripwire. Rust's `derive` +
`serde` make the compiler the parser; the whole pipeline collapses to `#[derive(...)]` plus a thin
`xtask` emitter (PP-7 owns `10-protocol-codegen`).

| Removed artifact | LOC | Where | Rust replacement |
|---|---|---|---|
| `control_dto_serde.generated.cpp` — hand-shaped serde for all 236 DTOs + 17 enums | **4974** | `engine-old/source/saffron/control/control_dto_serde.generated.cpp` | `#[derive(Serialize, Deserialize)]` on the DTO structs in `saffron-protocol` (PP-7). The serde code *is* the derive expansion; nothing is committed. |
| `scene_component_serde.generated.cpp` — per-component JSON serialize/deserialize | **727** | `engine-old/source/saffron/scene/scene_component_serde.generated.cpp` | `#[derive(Serialize, Deserialize)]` on the component structs + the registry fn-pointer rows (03-ecs-and-scene phase-5/6). |
| `script_component_defs.generated.hpp` — the `:get_component(name)` typed-table Luau defs appended to `library/sa.lua` | **194** | `engine-old/source/saffron/assets/script_component_defs.generated.hpp` | Emitted by the single-source Luau typegen from the binding/registry source (PP-8 `12-scripting`); no committed generated header. |
| `gen.ts` — the regex DTO parser (parses `control_dto.cppm` → C++ serde + `@saffron/protocol` TS + OpenRPC + manifest; *throws* on a member containing `(`/`)`/`=`) | **3504** | `tools/gen-control-dto/gen.ts` | `xtask` (PP-7): `schemars` 0.8 for JSON Schema fragments, `ts-rs` 10 for `@saffron/protocol` TS, a hand-rolled ~100-line OpenRPC/manifest emitter over the schemars fragments. The Rust compiler parses the DTOs; the regex parser and its `(`/`)`/`=` fragility are gone. |
| `tools/sa/args.hxx` — the vendored Taywee/args header-only CLI parser | **5135** | `tools/sa/args.hxx` | `clap` 4 (derive) in the `sa` crate (PP-9 `11-sa-cli`); the arg tree is `#[derive(Parser)]`. (Vendored, not engine-authored, but deleted from the tree all the same.) |
| `check-script-defs/check.ts` — the script-API drift tripwire (regex-asserts every live `.addFunction("…")`/`rawset(sa,"…")` name and every `registerComponent<…>("Name")` appears in the `SaLuaDefs` LuaLS string) | **69** | `tools/check-script-defs/check.ts` | **Deleted, no behavioral replacement** — the Luau defs are *generated* from the binding source (PP-8), so there is no hand-written second copy to drift from. The freshness it guarded becomes a "regen is byte-stable" check folded into the gen-freshness diff (`01-build-and-toolchain:phase-6`); the name-coverage tripwire itself is dead (`check-script-defs` step removed from the gate, per PP-12 key decisions). |
| `SaLuaDefs` — the hand-written `---@meta` LuaLS overlay string (the `library/sa.lua` body, kept in sync by hand with the imperative `.addFunction` bindings) | ~107 (`assets.cppm:1078-1185`) | `engine-old/source/saffron/assets/assets.cppm:1078`, written to `library/sa.lua` at `assets.cppm:1211` | Generated from the single binding source (PP-8) — the `library/sa.lua` write stays (projects still get autocomplete), but its content is a build output, never hand-edited. The hand-maintained string is deleted (the §0 locked decision: "no hand-written overlay, no drift tripwire"). |
| The per-enum three-table hand-sync + the four-place scene-component registration trap (a new component touched DTO, serde-gen, registry, and the Lua alias separately) | spread across the above | One registration site: `register_component::<C>` (03-ecs-and-scene phase-5) + the derives. Adding a component touches one place; the registry-completeness `#[test]` is the new guard. |

**Net deleted/collapsed generated+codegen LOC:** 4974 + 727 + 194 (committed generated C++/hpp) + 3504
(`gen.ts`) + 5135 (`args.hxx`) + 69 (`check.ts`) + ~107 (`SaLuaDefs`) = **~14,710 LOC** removed, replaced
by `#[derive]` attributes + one ~100-line OpenRPC emitter + one `clap` derive tree + the Luau typegen. The
headline "~5.7k generated + ~3.5k gen.ts" figure is the engine-authored core (5701 + 3504); the wider tally
above includes the vendored CLI parser and the overlay that also disappear.

---

## 3. Idiom ceremony that evaporates (language features replace boilerplate)

These are not generated; they are hand-written C++ that exists only to compensate for what the language
lacks. Rust has the feature, so the ceremony disappears.

| Removed ceremony | Where | Rust replacement |
|---|---|---|
| `JSON_NOEXCEPTION` abort firewall — the *entire reason `Saffron.Json` was structured the way it is*: nlohmann is built no-throw so a parse failure aborts instead of throwing, and the gateway wraps every access to convert that into a `Result` | `json.cppm:17`, the `Saffron.Json` gateway design | `serde_json` returns `Result` natively. `saffron-json` keeps the **lenient typed readers** (`json_u64`/`string`/`f64`/`bool` + `*_or`) and the decimal-string-u64 wire encoding (those are *semantics*, frozen), but the "no-throw + manual `Result` wrapping to dodge an abort" rationale is gone (PP-1 `crateGraph` saffron-json role). |
| The check-`Result`-immediately discipline (`Result<T,std::string>` + `Err("msg")` + an `if (!x) return Err(...)` after every fallible call) | every fallible function across `engine-old/` | The `?` operator **is** the immediate check; typed `thiserror` enums per crate compose via `#[from]`. There is no unchecked `Result` in Rust, so the discipline is enforced by the type system, not by hand (PP-1 `errorModel`). `Result<T,String>` carry-over is explicitly forbidden. |
| The physics `pimpl` — `struct PhysicsWorldImpl;` forward-declared, `std::unique_ptr<PhysicsWorldImpl> impl_;`, the `impl()`/`impl() const` accessor pair, the private ctor + `friend createPhysicsWorld()` — all to keep Jolt headers out of consumers' TUs | `physics.cppm:23,29-51` | Gone — module/crate privacy is free in Rust. The Jolt headers live behind the `saffron-physics-sys` crate boundary; `saffron-physics` exposes a safe `World` with private fields. The pimpl-as-header-firewall pattern has no Rust analog because there is no header to firewall (PP-11 key decision: "the C++ pimpl seam becomes the crate boundary"). |
| Manual move-only RAII boilerplate — per GPU wrapper: deleted copy-ctor + deleted copy-assign + move-ctor + move-assign + hand-written `~Dtor` (Image, Image3D, Buffer, GpuMesh, GpuTexture, Pipeline, AccelerationStructure — 7 types × ~4 deleted/defaulted special members each) | `renderer_types.cppm` (`= delete`/`~Image`/`~Buffer`/… at :103,128,165,203,251,302,351,389,434,464,495,528,1527,…) | `impl Drop` only. Rust types are move-only by default, so the deleted-copy / defaulted-move boilerplate (~4 lines × 7 types) evaporates — each wrapper is a struct with one `impl Drop` (PP-1 `idiomRules`, 06-rendering phase-3). |
| The two hand-written vtable structs of `std::function` (the "Go-interface-as-itable" pattern) — `ComponentTraits` (10 `std::function` fields: `has`/`addDefault`/`remove`/`copyTo`/`serialize`/`deserialize` + the **always-no-op `drawInspector`** the host registers empty) and `CommandTraits` (`name`/`help` + one `run` `std::function`) | `scene.cppm:1209-1222`, `command.cppm:42-47` | `ComponentTraits` → a struct of monomorphic **fn-pointers** keyed by `TypeId`, one `register_component::<C>` site (03-ecs-and-scene phase-5); the `drawInspector` field is **DELETED outright** (NO LEGACY — it was always a no-op since the inspector is the React editor). `CommandTraits` → `Command { name, help, handler: Box<dyn Fn(&mut EngineContext,&Value)->Result<Value>> }` in a `Vec`+`HashMap` registry (09-control-plane phase-1). The `Layer` struct of optional closures (`onAttach`/`onUpdate`/`onRender`/`onUi`/`onRenderGraph`/`onDetach` at `app.cppm:16-21`) → `trait Layer` with default-empty methods (PP-1 `layerModel`). |
| `std::variant` + `switch`+`std::get` discriminators; `std::optional` + the `has*`-bool-plus-parallel-blob pattern (e.g. `ImportedMaterial`'s `has*` bools beside byte blobs) | scattered (geometry import, EnvSource, MotionType, …) | Data-carrying `enum` + `match` (the switch+`std::get` collapses to one `match`); `Option<T>` carries the payload so a bool can never disagree with its blob (PP-1 `idiomRules`; 02-math-and-geometry key decision on `Option<TextureSource>`/`Option<SkinPayload>`). |

---

## 4. Self-test functions — deleted as runtime code, re-expressed as `#[test]`

The locked §0 rule: **no in-engine runtime self-test functions survive.** The host runs ~15 self-tests at
startup (`host.cppm:1314-1345`); each is deleted as a runtime symbol and its assertions become real Rust
`#[cfg(test)]` units (and the wire-driven e2e harness for cross-cutting cases). This is a *behavioral*
subtraction (the engine no longer self-tests at boot) with a *test-suite* replacement, not a deletion of
the assertions themselves.

| Deleted self-test (runtime symbol) | Where | Re-expressed as |
|---|---|---|
| `runSignalSelfTest` | `signal.cppm:61` | `#[cfg(test)]` in `saffron-signal` (00-foundations phase-3). |
| `runSceneSerializationSelfTest`, `runSceneHierarchySelfTest` | `scene.cppm:1658,1854` | 03-ecs-and-scene phase-4 (hierarchy/transform) + phase-7 (serialization). |
| `runPlayModeSelfTest` | `scene_edit_play.cpp:232` (decl `scene_edit_context.cppm:347`) | 03-ecs-and-scene phase-10. |
| `runGeometrySelfTest`, `runTranslateDeterminismSelfTest`, `runContainerSelfTest`, `runPickMathSelfTest` | `geometry.cppm:2186,1981,2024,2137` | 02-math-and-geometry phases 2/3/6/7. |
| `runCatalogLinkageSelfTest`, `runContainerMetadataSelfTest`, `runBakeModelSelfTest`, `runChunkLoaderSelfTest`, `runInstantiateSelfTest`, `runExtractSelfTest`, `runReimportSelfTest` | `assets.cppm:549,754,4531,4639,5068,5162,5246` | 07-assets-and-materials units (their own area). |
| `runAnimationSelfTest` (~430 LOC, the IK/sampling oracle) | `animation.cpp:766` (decl `animation.cppm:128`) | 04-animation phases 3 (math+IK oracle) + 5 (runtime). |
| `runScriptSelfTest` | `script.cppm:303` (decl :58) | 12-scripting units. |
| `runPhysicsSelfTest` | `physics.cpp:1533` (decl `physics.cppm:206`) | 05-physics units; the determinism trace becomes the blocking gate (05-physics phase-5). |

The entire `host.cppm:1314-1345` self-test dispatch block (and the `if (auto x = run…SelfTest(); !x)`
error-propagation around the four `Result`-returning ones) is deleted with no runtime replacement — `cargo
test` is the harness.

---

## 5. Rust-specific non-needs (ordering hacks that designed-Drop makes unnecessary)

These are C++ choreography sequences that exist only because C++ has no guaranteed-and-designed destruction
order across an aggregate; once `Drop` order is *designed* (field order / explicit `Drop`), the manual dance
is unnecessary as a *hand-sequenced runtime concern* — though the *ordering itself* remains a design
obligation (see §6, it does not come for free).

| Removed hand-choreography | Where | Why it shrinks |
|---|---|---|
| The `onExit` teardown lambda's manual ordering: stop the thumbnail worker → `destroyControlContext` → `stopScripts` → `physics.reset()` → null the `simTick`/signal subscriptions → `destroySceneEditContext` → (Jolt globals shut down last) | `host.cppm:1574-1600+` | The *sequence* is preserved as a designed `Drop` order (PP-10 / 06-rendering phase-3), but it stops being a hand-written lambda the client must remember to register; field order + `impl Drop` encode it once. The `static_cast<void>(app)` unused-param noise and the null-then-reset two-step disappear. |
| `newSceneEditContext()` / `destroySceneEditContext()` heap-ownership dance ("heap-owned so the heavy entt/json destructor is instantiated here, not in the client TU; the editor holds only the pointer") | `scene_edit_context.cppm:359-360` | Gone — there is no "destructor must be instantiated in the right TU" problem in Rust; `SceneEditContext` is an owned value, `Drop` is automatic (03-ecs-and-scene phase-8 key decision). |
| Manual `Ref`-drop-in-`onExit` choreography for GPU resources (drop client `Ref`s in `onExit` so nothing outlives `vmaDestroyAllocator`, with `run()` calling `waitGpuIdle` first) | `host.cppm` `onExit` + `AGENTS.md` Resources note | The `waitGpuIdle`-before-teardown stays a real requirement, but it becomes a single host run-loop responsibility (PP-10) + designed `Drop` order, not a per-`Ref` reset choreography the client hand-writes. |

---

## 6. The flip side — what Rust *forces us to add* (no C++ counterpart)

Honesty cuts both ways: a few things have no analog in the C++ tree and are net-new obligations the port
must carry. These are small and localized, but they are not "subtractions" — list them so reviewers don't
flag them as gratuitous.

| Added construct | Why Rust needs it | Where |
|---|---|---|
| `Arc<Mutex<T>>` at the proven multi-thread shared-mutable sites | C++ used free-function `std::mutex` singletons (`gpuQueueMutex()` at `renderer_types.cppm:33`, `bindlessMutex()` at :42, taken at :402) over plain shared state; Rust makes the shared-mutable explicit at the site. Exactly two such sites in rendering: the graphics queue (shared with the thumbnail worker) and the bindless table + free-list. `GpuTexture` holds a clone of the `Arc<Mutex<Vec<u32>>>` free-list so its `Drop` returns its slot thread-safely. | PP-1 `refPolicy`; 06-rendering phases 3/4. Everything else is `Arc<T>` (read-shared) or owned-and-`&mut` — no global `Arc<Mutex>` sweep. |
| A scoped **session guard** re-encoding the borrowed-pointer invariant | The C++ script host reaches the scene only through `host->currentScene` (`script.cppm:123`, a raw `Scene*` non-null only inside a callback, checked by hand at `script_runtime.cpp:182,187,198`). Rust cannot hold a raw borrowed pointer across the VM boundary safely; a scoped guard sets/clears the borrow for the duration of the callback so the non-null-only-inside-a-callback invariant is type-enforced. | PP-8 `12-scripting` (the "part Rust adds"). The host-callback POD bridge (`sa.raycast`/`sphereCast`) becomes a host-implemented trait so `saffron-script` stays free of physics/animation deps. |
| Explicit **`Drop` order** as a design concern | C++ relied on the `onExit` lambda + member declaration order; Rust drops fields in declaration order, so the device-before-allocator / `waitGpuIdle`-before-teardown ordering must be *designed* into struct field order or an explicit `Drop` impl. It is cheaper than the C++ choreography (§5) but it is **not free** — it is a thing the renderer/host areas must get right or the validation layer flags a use-after-free at teardown. | PP-1 `idiomRules` (the move-only/Drop rule explicitly flags teardown order as "a design concern… not free"); 06-rendering phase-3; PP-10 host run loop. |
| Per-crate `thiserror` `Error` enums + `Result<T>` aliases | Replacing `Result<T,std::string>` with typed errors means each library crate authors its own `enum Error` + `#[from]` composition — more up-front type declaration than `Err("string")`, traded for matchable, composable errors. | PP-1 `errorModel`. (`anyhow` only in `[[bin]]` crates + `xtask`.) |
| The two FFI `build.rs` + the `#![allow(unsafe_code)]` justifications | The three unsafe seams (`saffron-physics-sys`, the `saffron-rendering` ash seam, the `saffron-host` shm publisher) each carry an explicit allow + a top-of-file justification naming the seam; the workspace lint is `unsafe_code = deny` everywhere else. C++ had no such gate (all of it was implicitly unsafe). | PP-1 `fileConventions` / `idiomRules`. |

---

## 7. Tally

- **Build/toolchain apparatus removed (§1):** the entire `cmake/` layer + `CMakePresets.json` + the
  `import std`/BMI/two-ninja/FetchContent machine — replaced by `Cargo.toml` profiles + 2 `build.rs` + 1
  `xtask`. Not LOC-countable as one number; it is the headline deletion.
- **Generated + codegen LOC removed (§2):** ~14,710 (5701 engine-generated C++/hpp + 3504 `gen.ts` + 5135
  vendored `args.hxx` + 69 `check.ts` + ~107 `SaLuaDefs`) → `#[derive]` + ~100-line OpenRPC emitter + `clap`
  derive + Luau typegen. The pre-plan's "~5.7k generated + ~3.5k gen.ts" is the engine-authored core subset.
- **Idiom ceremony removed (§3):** `JSON_NOEXCEPTION` firewall, check-`Result`-immediately discipline, the
  physics `pimpl`, the move-only RAII boilerplate (7 wrappers), the two hand vtable structs
  (`ComponentTraits` incl. the always-no-op `drawInspector`, `CommandTraits`) + the `Layer` closure struct,
  `std::variant`/`std::optional` ceremony.
- **Self-tests removed as runtime code (§4):** ~15 `run*SelfTest` functions (incl. the ~430-LOC animation
  oracle) → `#[cfg(test)]` + e2e. The `host.cppm:1314-1345` dispatch block is deleted outright.
- **Choreography that shrinks (§5):** the `onExit` teardown lambda, the `newSceneEditContext`/`destroy…`
  heap dance, the `Ref`-drop-in-`onExit` ordering.
- **What Rust adds (§6):** two `Arc<Mutex>` sites, the script session guard, designed `Drop` order, per-crate
  `thiserror` enums, the two FFI `build.rs` + unsafe-allow justifications.
