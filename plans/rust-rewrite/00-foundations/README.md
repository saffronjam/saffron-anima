# 00 — Foundations: workspace, house style, and the leaf crates

**Status:** COMPLETED — phases 1-5 done (workspace + core/signal/json/window crates build and test green).

This is the keystone area of the Rust rewrite. It locks two things every other area obeys:

1. **The Cargo workspace crate graph** — how the C++26 `Saffron.<Area>` named-module DAG maps onto
   Cargo build units, where the crate boundaries fall for low coupling and fast incremental builds,
   and what is a `lib` vs the engine `bin` vs an FFI-bridge crate.
2. **The Rust house style** — the idiom translation rules (errors, ownership, sum types, events,
   RAII, naming) decided once here and applied verbatim everywhere. This replaces the Go-flavored
   `CONVENTIONS.md` wholesale; that document is retired (it stays only as historical reference for
   reading `engine-old/`).

It also ports the three leaf modules — `Saffron.Core`, `Saffron.Signal`, `Saffron.Json` — into their
Rust crates, because they are the DAG root and a conventions-setting exercise. Getting `Result`,
`Uuid`, `Ref` → `Arc`, the `SubscriberList` hand-roll, and the serde JSON gateway right here makes
~20 downstream modules mechanical.

The companion document [`conventions.md`](./conventions.md) is the full idiom translation table; this
README is the workspace architecture + the design rationale. They are read together.

---

## 1. Why a crate graph at all (NO LEGACY)

The C++ engine is **one** static library, `SaffronAnimaLib` (`Saffron::Anima`), built from a 23-file
`CXX_MODULES` file set with a hand-ordered DAG declared in `engine-old/CMakeLists.txt`. The module
boundaries (`Saffron.Core` … `Saffron.Host`) are *logical* — the compiler enforces import direction,
but everything links into one archive, and a single change recompiles every BMI consumer downstream.

Cargo gives us real, enforced compilation units. We make each logical module a **crate** so:

- the dependency DAG is enforced by Cargo, not by convention (a crate physically cannot `use` a crate
  not in its `[dependencies]`);
- incremental builds are per-crate — touching `rendering` does not recompile `scene`;
- the FFI seams (`ash`, Jolt `cxx`, the shm ring) are isolated in their own crates so `unsafe` is
  confined and auditable;
- the test gate is per-crate (`cargo test -p saffron-scene`) and the workspace stays green leaf-up.

We do **not** collapse modules to fewer crates "to reduce Cargo.toml count." One crate per logical
module is the default; we only deviate where the C++ module is *already* split into partitions that
are tightly coupled (rendering) or where a sub-unit is a distinct build concern (the Jolt C++ shim).
There is exactly one crate that owns each responsibility — no duplicate "util" crates, no shim crates
that re-export another crate's types.

---

## 2. The crate graph

All crates live under `engine/crates/` in a virtual workspace rooted at `engine/Cargo.toml`. Crate
names are `saffron-<area>` (the published/internal name); the directory is `engine/crates/<area>/`;
the `lib.rs` crate-root identifier is `saffron_<area>` (Rust's `snake_case`). One namespace discipline
from C++ (`sa::`) becomes "one crate per area, re-exported from a thin `saffron` facade only if the
`bin` wants a single `use`" — but crates depend on each other directly, never through a facade.

```
saffron-core      (leaf)   ← Result alias, Uuid, Ref=Arc, log, base64, TimeSpan, fixed-width type notes
saffron-signal             → core                  SubscriberList event primitive
saffron-json               → core                  serde_json gateway + WireUuid + lenient readers
saffron-window             → core, signal          winit + raw-window-handle thin wrapper
saffron-geometry           → core                  glam, .smesh/.sanim/.smodel byte formats, gltf/obj/image import
saffron-scene              → core, json, (ecs)     hecs/bevy_ecs world, components, JSON project serde
saffron-animation          → core, geometry, scene pose/clip/sampler, player runtime, IK
saffron-physics-sys (ffi)  → (cc/cxx + vendored Jolt) the C++ JoltC shim + determinism build; unsafe boundary
saffron-physics            → core, geometry, scene, animation, physics-sys   safe Jolt wrapper
saffron-script             → core, scene           mlua/Luau VM + typed bindings + generated defs
saffron-rendering          → core, window, geometry  ash, allocator, render graph, all passes (large; submodules, one crate)
saffron-assets             → core, json, geometry, rendering, scene   catalog, .smat, codegen, thumbnails
saffron-sceneedit          → core, signal, scene, json   selection/gizmo/play context
saffron-protocol (lib)     → core (serde + schemars + ts-rs)  the DTO crate (shared by engine + sa CLI + codegen)
saffron-control            → core, json, window, rendering, scene, sceneedit, assets, physics, protocol
saffron-app                → core, window, rendering   run-loop scaffolding
saffron-host (bin)         → core, app, window, rendering, sceneedit, control, scene, animation, physics, script, assets
sa (bin)                   → protocol only            the native `sa` control CLI (no engine dep)
xtask (bin)                → protocol, schemars, ts-rs  codegen + slangc driver (workspace tooling, not shipped)
```

### 2.1 Decisions that differ from the 1:1 module map

- **`saffron-protocol` is a new first-class crate**, pulled out of `Saffron.Control`. In C++ the DTOs
  live in `control_dto.cppm` *inside* the control module; in Rust the DTO structs (deriving
  `serde`/`schemars`/`ts-rs`) are the single source of truth for the wire types **and** the `sa` CLI
  **and** the protocol codegen. The CLI must link the DTOs *without* the engine (feasibility §4.6, the
  CLI is "engine-dependency-free"), so the DTOs cannot live in `saffron-control`. `saffron-control`
  depends on `saffron-protocol`; so does `sa`; so does `xtask`.
- **`saffron-physics-sys` is split from `saffron-physics`.** The C++ side is one TU (`physics.cpp`)
  with a `pimpl` hiding Jolt; in Rust the unavoidable `unsafe` + the `cc`/`cxx`-built vendored Jolt
  with its determinism flags is a `*-sys` crate (`build.rs` owns the Jolt + shim TUs and re-applies
  `JPH_CROSS_PLATFORM_DETERMINISTIC` + `-mavx2` to *only* that crate's TUs, mirroring the
  `SAFFRON_JOLT_COMPILE_OPTIONS` isolation in `engine-old/CMakeLists.txt`). `saffron-physics` is the
  safe Rust wrapper above it. This area (00) only *reserves* the split; PP-11 designs it.
- **`saffron-rendering` stays one crate** despite being four C++ partitions
  (`render_graph`/`renderer_types`/`renderer_detail`/`renderer` + nine `renderer_*.cpp` units). They
  share the ~80-field renderer aggregate and cannot be cut without exposing its internals across a
  crate boundary; PP-5 re-architects the aggregate into private submodules *within* the crate. Splitting
  it would force `pub` on internal sub-state. So: one crate, many `mod`s.
- **`xtask` replaces `tools/gen-control-dto/gen.ts` and `CompileShaders.cmake`.** Codegen and the
  `slangc` fan-out are workspace tooling, run via `cargo run -p xtask <task>`, not a shipped crate.
  PP-7 (codegen) and PP-12 (build) own its design; 00 only reserves it in the graph so the workspace
  member list is complete from day one.
- **`saffron-window` depends on `signal`** because the C++ `Window` exposes typed `SubscriberList`
  signals (`onResize`, `onKeyPressed`, …). That edge is `Window → {Core, Signal}` in the C++ DAG and
  it is preserved.
- **No `app`/`host` merge.** The C++ DAG keeps `App` (the lifecycle scaffold) separate from `Host`
  (the apex `bin`). We keep both: `saffron-app` is a `lib` with the run-loop/Layer scaffolding,
  `saffron-host` is the `[[bin]]` that wires every subsystem. The feasibility study calls host the
  "integration apex"; keeping it a thin `bin` over an `app` lib matches that.

### 2.2 The DAG is acyclic and leaf-up

The build order PP-14 linearizes from this graph starts at `saffron-core` (no deps) and ends at
`saffron-host`. Every crate compiles on top of only its declared dependencies — that is what makes the
"workspace green at every phase" gate satisfiable. The walking-skeleton milestone (PP-10/PP-14) is the
first point `saffron-host` links: core + signal + json + window + a stub scene + a blank-frame shm
publisher + a `ping` control handler.

### 2.3 Workspace `Cargo.toml` shape

The placeholder at `engine/Cargo.toml` (a bare virtual workspace) is **replaced** by phase 1 with:

- `[workspace] resolver = "3"`, `members = ["crates/*", "xtask"]`;
- a `[workspace.package]` block pinning `edition = "2024"`, `rust-version`, `license`, `version`;
- a `[workspace.dependencies]` table pinning every third-party crate version **once** (the
  PP-2 pin list), so member crates write `glam.workspace = true` and never drift versions;
- `[workspace.lints]` carrying the lint policy (`unsafe_code = "deny"` at the workspace level, with
  the three FFI crates opting back in via `#![allow(unsafe_code)]` + a documented justification).

The `[profile.*]` blocks live here too (PP-12 owns the exact knobs; 00 reserves the section).

---

## 3. House style (summary — full table in `conventions.md`)

The retired `CONVENTIONS.md` was Go-flavored C++: free functions over methods, `Result<T,std::string>`
everywhere, `?:` banned, manual itable structs, one `sa::` namespace. **None of that survives.** The
Rust house style is *idiomatic Rust*, decided per construct in `conventions.md`. The load-bearing
decisions, stated once:

- **Errors: typed per-crate `thiserror` enums, propagated with `?`.** No `Result<T, String>`
  transliteration (that would carry a C++ habit for no reason), no blanket `anyhow` in library crates
  (`anyhow` is allowed only in the `bin`s and `xtask` at the top of the call stack). Each crate
  defines its own `Error` enum; cross-crate errors compose via `#[from]`. The C++ `Err("message")` +
  immediate-check discipline becomes a typed variant + `?`.
- **`Ref<T> = shared_ptr<T>` → `Arc<T>` by default; `Arc<Mutex<T>>` *only* at a proven
  shared-mutable site.** This is the single most cascading decision (feasibility §3 core-foundation,
  §4.7). A `Ref` that is read-shared after construction is `Arc<T>`. A `Ref` mutated *through the
  shared handle* (the renderer's bindless table, the GPU queue, the asset caches) is `Arc<Mutex<T>>`
  (or `Arc<RwLock<T>>` for read-heavy). `Rc<RefCell<T>>` only for single-thread-confined graphs (e.g.
  the host's per-frame overlay state) where `Send` is not needed. The C++ `gpuQueueMutex()` /
  `bindlessMutex()` free-function singletons (`renderer_types.cppm:33,42`) are the *explicit markers* of
  which `Ref`s become `Arc<Mutex>` — they tell us the shared-mutable sites up front.
- **`std::variant` and `enum class` → Rust `enum`** (data-carrying where the C++ used a tagged union).
  A net simplification: the manual `switch`+union collapses to `match`.
- **`std::function` itable structs → traits with `dyn` objects, or fn-pointer tables, or enums** —
  decided per case in `conventions.md`. The `Layer` struct-of-closures becomes a `trait Layer`;
  component/command "traits" structs become a registration table; small closed sets become enums.
- **Move-only RAII wrappers → `Drop`.** Vulkan handle wrappers, file handles, shm mappings implement
  `Drop`; the C++ `waitGpuIdle`-before-teardown choreography becomes a designed `Drop` *order* (a
  field-order or explicit-drop concern, PP-10).
- **`SubscriberList<Args...>` → a hand-rolled generic events type in `saffron-signal`** preserving the
  exact contract (handler returns `bool` to stop propagation; `publish` iterates a snapshot so a
  handler can sub/unsub mid-dispatch). No crate matches this contract; it is ~60 lines (§4.7).
- **Naming:** `snake_case` files and functions (the C++ `camelCase` functions become `snake_case`),
  `PascalCase` types/traits/enum variants, `SCREAMING_SNAKE_CASE` consts. The C++ `newThing()`
  free-function constructors become associated functions (`Thing::new` / `Thing::from_*`) or trait
  impls (`Default`, `From`) where idiomatic.
- **Tests:** unit tests inline `#[cfg(test)] mod tests` in the same file as the code under test;
  cross-crate / integration tests in `tests/`. **No in-engine self-test functions** — the C++
  `runSignalSelfTest` (`signal.cppm:61`) becomes `#[test]`s, not a runtime function.

---

## 4. Grounding (What | File | Symbols)

| What | File (engine-old) | Symbols |
|------|-------------------|---------|
| Fixed-width aliases, `Ref`, `Result`/`Err`, engine name/version | `source/saffron/core/core.cppm` | `u8..f64`, `Ref<T>`, `Result<T>`, `Err`, `EngineName`, `EngineVersion` |
| Time + identity primitives | `source/saffron/core/core.cppm` | `TimeSpan`, `toMilliseconds`, `Uuid`, `newUuid` (reserves `<1024`) |
| Logging (subsystem-tagged) | `source/saffron/core/core.cppm` | `LogLevel`, `log`, `logInfo/Warn/Error`, `logSubsystem` (path → subsystem) |
| Base64 for binary-over-JSON | `source/saffron/core/core.cppm` | `base64Encode` (RFC 4648, used by thumbnail replies) |
| The event primitive | `source/saffron/signal/signal.cppm` | `SubscriberList<Args...>`, `SubscriptionId`, `subscribe`/`unsubscribe`/`publish`, snapshot-iterate |
| The signal self-test (oracle → `#[test]`) | `source/saffron/signal/signal.cppm` | `runSignalSelfTest` (fan-out, stop-prop, unsubscribe, re-entrant self-unsub) |
| The JSON gateway | `source/saffron/json/json.cppm` | `Json`, `parseJson`, `dumpJson`, `uuidToJson` (decimal string), `jsonU64/String/F64/Bool`, the `*Or` readers, `findField` |
| Decimal-string-u64 wire detail (the silent-failure contract) | `source/saffron/json/json.cppm` | `uuidToJson` emits `std::to_string(value)`; `jsonU64` accepts number *or* decimal string |
| The single-static-lib module DAG (the crate-graph source) | `engine-old/CMakeLists.txt` | `FILE_SET CXX_MODULES` ordering; `SAFFRON_JOLT_COMPILE_OPTIONS` on `physics.cpp` (the `*-sys` split rationale) |
| The shared-mutable markers (which `Ref`→`Arc<Mutex>`) | `source/saffron/rendering/renderer_types.cppm` | `gpuQueueMutex()`, `bindlessMutex()` |
| The retired style (reference only) | `CONVENTIONS.md` | Go-flavored C++ — superseded by `conventions.md` |

---

## 5. Phases in this area

| Phase | Crate(s) | Depends on | Status |
|-------|----------|------------|--------|
| `phase-1-workspace-scaffold` | the workspace `Cargo.toml` + empty member crates that compile | — | COMPLETED |
| `phase-2-core-crate` | `saffron-core` | phase-1 | COMPLETED |
| `phase-3-signal-crate` | `saffron-signal` | phase-2 | COMPLETED |
| `phase-4-json-crate` | `saffron-json` | phase-2 | COMPLETED |
| `phase-5-window-crate` | `saffron-window` | phase-2, phase-3 | COMPLETED |

Phases 2–4 can be authored in any order after phase 1; phase 5 follows phase-3 (it reuses the
`SubscriberList` for its typed signals). The README's phase table is the canonical intra-area
sequence. PP-14 interleaves these into the global linear order (foundations is the first block —
nothing compiles until phase 1 lands).
