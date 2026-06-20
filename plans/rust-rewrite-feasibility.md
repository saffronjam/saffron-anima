# Feasibility Study: Rewriting the Saffron Anima Engine from C++26 to Rust

**Status:** Decision-grade assessment, 2026-06-15
**Scope:** The `Saffron::Anima` engine (~50k LOC C++26) and its supporting toolchain. The Tauri/React editor (`editor/`) is in scope only as a constraint, not a rewrite target.

---

## 1. Executive Summary

**A faithful Rust rewrite is feasible. It is not obviously worth it, and it is a multi-quarter undertaking that must not be undersold.** Every hard dependency this engine leans on has a viable Rust answer in 2026 — `ash` for Vulkan, `bevy_ecs` (or `hecs`) for the entt scene, `mlua` for Lua 5.5, `glam`/`serde`/`bytemuck` for the commodity layers — and a custom `cxx`/JoltC bridge keeps Jolt with its determinism intact. None of the user's two hard constraints (ECS iteration speed; cross-platform-deterministic physics) is a blocker. The codebase is also written in a near-Rust dialect already (error-as-value, no inheritance, no exceptions, free functions over explicit state), so a large fraction of the translation is mechanical and several idioms come out *shorter* in Rust.

The honest counterweight is threefold. **First, the engine is feature-complete and large** — a full forward+ PBR pipeline with RT/DDGI/ReSTIR/TAA, skeletal animation with ragdoll, physics, scripting, a control plane, and a working editor. A rewrite recreates working, validated software; the payoff is memory-safety and toolchain simplicity, not new capability. **Second, the two biggest subsystems are exactly the shared-mutable patterns Rust rejects**: the ~80-field by-reference `Renderer` god-aggregate and the per-frame entt-scene + asset-cache graph need genuine ownership *re-architecture*, not transcription — this is where the schedule actually goes. **Third, every load-bearing contract fails silently if it drifts** — the render-graph barrier derivation, the std430 GPU layouts, the cross-process shm seqlock ABI, the decimal-string-u64 wire format — so the rewrite is gated on reproducing the existing validation/e2e/contract suite, not on getting it to compile.

The headline risks, ranked: (1) the renderer ownership re-architecture and its silent-failure barrier/layout/ABI surface; (2) proving Jolt physics is still bit-identical across machines after re-binding through FFI; (3) ECS iteration throughput on the real per-frame `forEach` paths (satisfiable, but the user calls it non-negotiable, so it must be *measured*, not assumed); (4) schedule — a realistic ~80–120 person-weeks of subsystem engineering plus integration and overhead, dominated by rendering, physics, and the host integration apex.

**Recommendation: phased, with two hard go/no-go gates before any large commitment.** Do not big-bang. Do not attempt an incremental FFI bridge that keeps the C++ engine alive behind a C ABI — the engine's seams are deep by-reference aggregates, not C-ABI-friendly, so a bridge would FFI-wrap nearly the whole surface for little gain and violate the project's own no-compat-shims rule. Instead: run a 4–6 week **spike** that de-risks the three things that decide everything (physics determinism cross-arch; renderer bring-up + barrier graph + shm ABI against the *unchanged* Tauri presenter; ECS iteration benchmark). If the spike clears, proceed leaf-up (core → geometry → scene → animation/physics/script → rendering → assets → control → host), reproducing the e2e/contract gate continuously. If the determinism spike fails to reach bit-exactness, the lockstep-netcode premise collapses and the decision should change.

---

## 2. Why This Codebase Is Unusually Well-Positioned (and Where It Isn't)

### Tailwinds

- **Go-flavored C++ maps almost 1:1 to idiomatic Rust.** `CONVENTIONS.md` mandates no inheritance, no exceptions, no operator overloading, free functions over explicit state, error-as-value. There are no virtual hierarchies to untangle; the only hand-rolled vtables (`ComponentTraits`, `CommandTraits` — structs of `std::function`) become trait objects or fn-pointer tables. `enum class` discriminators become Rust enums — a *win*, since Rust enums carry data and replace manual `switch`+union. The codebase reads like Rust with C++ syntax.
- **`std::expected<T,std::string>` is already the universal error model.** ~1179 `Err(` sites and ~744 `Result<` returns translate to `Result<T, String>` with the `?` operator — strictly shorter than the C++ check-immediately idiom. No exception-to-Result conversion work exists because there are no exceptions.
- **The editor's native side (`editor/src-tauri/`) is ALREADY Rust** (~1623 LOC) and is entirely engine-agnostic. It couples to the engine through two wire contracts (JSON-over-unix-socket control plane; POSIX-shm BGRA8 frame ring) glued by env vars and a child spawn — **never** through C++ symbols. A Rust engine that byte-matches those contracts needs *zero* editor changes, frontend or src-tauri. The single source-level coupling is the protocol code generator, and that re-points cleanly.
- **The JSON wire contract decouples the editor from the engine.** The `{id,cmd,params}`/`{ok,error,result}` envelope, string entity ids, and command names are the binding surface — language-agnostic and reproducible.
- **The clean-slate, no-legacy policy removes migration burden.** There is no field data, no downstream, no back-compat to preserve. A rewrite can break and rebuild each flow in one move. Project data migration is explicitly out of scope (start a fresh project), so there is no save-format migration tax beyond keeping the *new* engine's serde byte-compatible with its own files.
- **The C++26-modules + `import std` + toolbox build complexity is pure liability that Cargo deletes.** The experimental CMake UUID gate, `CXX_MODULE_STD` per target, gnu++26 BMI matching, the `-pthread`/`-mavx2` flag isolation on `physics.cpp` to protect the std.pcm BMI, the two-ninja `.pcm` Bus-error race — all of it vanishes. The 15-module DAG re-encodes as a Cargo workspace crate graph.

### Headwinds

- **The renderer is a single ~80-field by-reference aggregate** mutated by every free function, with per-frame code taking `Image&`/`ViewTargets&` sub-references while sibling fields mutate. This is illegal under Rust's one-`&mut` rule and requires deliberate sub-state splitting or interior mutability — re-architecture, not translation.
- **The scene/asset layer is object-soup**: generational entt handles, uuid-keyed caches with null-Ref negative-cache sentinels, handles aliased between the authored registry and the play-mode JSON-duplicate, self-referential gizmo snapshots. The hot per-frame `forEach` iteration speed cannot regress.
- **`host.cppm` is a 1615-line god-module** fusing lifecycle wiring, ~900 lines of CPU overlay geometry, and ~12 closures that capture `shared_ptr<HostState>` by value, mutate it across frames, *and* are stored on other subsystems. This aliasing-mutable-shared-state pattern is exactly what the borrow checker forbids; it becomes `Arc<Mutex>`/`Rc<RefCell>` and must be ported last.
- **Three FFI re-bindings give Rust little leverage**: Vulkan (`ash` holds raw unsafe handles with no crate RAII — you still write Drop wrappers and convert thousands of `checked()` sites), Jolt (the only deterministic option, re-bound via a custom `cxx` shim because published crates lack ragdoll/CharacterVirtual/SwingTwist coverage), Lua (mlua is a *win*, but LuaBridge3's DSL must be re-expressed).
- **Silent-failure contracts everywhere**: the RgUsage→barrier derivation, the std430 layouts hashed by raw bytes for material dedup, the shm seqlock byte layout + release-fence ordering, the decimal-string-u64 wire encoding. Each fails as a data race / torn frame / corrupted id — never a compile error — so only the validation layer and the e2e/contract suite catch regressions.
- **No GitHub-hosted CI is possible** (a stock runner can't reproduce the toolbox). The toolbox container stays the reproducibility boundary in Rust too — Cargo replaces CMake but not the Vulkan SDK / slangc / SDL3 / headless-weston substrate.

---

## 3. Subsystem-by-Subsystem

| Subsystem | LOC | Target Rust crates | Feasibility | Effort (1 exp. dev, deps ported) | Top risk |
|---|---|---|---|---|---|
| core-foundation | ~512 | std, serde_json, base64, rand, log (+ hand-rolled SubscriberList) | **Trivial** | 0.5–1 wk | `Ref`→`Arc` vs `Arc<Mutex>` decision cascades engine-wide |
| geometry | ~2,269 | glam, gltf, tobj, image/stb, bytemuck/zerocopy | **Moderate** | 4–6 wk | Byte-exact triple-contract layouts + glTF node-order/dedup determinism |
| scene + sceneedit | ~2,730 | hecs **or** bevy_ecs, glam, serde | **Moderate** | 7–10 wk | ECS choice; byte-compat JSON serde; euler ZYX stability |
| animation | ~1,326 | glam, the chosen ECS | **Straightforward** | 2.5–4 wk | Dependency order; two-bone IK numerical fidelity |
| script | ~1,951 | mlua (lua55), glam, serde_json | **Moderate** | 4–6 wk | Borrowed-pointer lifetime invariant vs borrow checker |
| physics | ~1,840 | **cxx bridge to vendored Jolt 5.3.0**, glam, parking_lot | **Hard** | 7–11 wk | Cross-machine bit-exact determinism; binding-coverage gap |
| rendering | ~16,557 | ash, vk-mem/gpu-allocator, sdl3/winit, glam, image, resvg | **Hard** | 26–38 wk | Renderer aggregate re-architecture; silent barrier/layout/ABI drift |
| assets | ~7,020 | serde_json, glam, bytemuck, walkdir, `std::process::Command` | **Hard** | 10–16 wk | `renderScene` coupling; negative-cache semantics; GPU lifetime ordering |
| control | ~13,498 (+3.5k gen.ts +0.6k sa) | serde + serde_with, nix, schemars, ts-rs, clap | **Moderate** | 7–9 wk | Must be ported *last*; decimal-string-u64 wire gate |
| host + app + window | ~2,323 | ash, glam, sdl3/winit, nix/rustix, serde_json, vk-mem | **Hard** | 8–13 wk | Integration apex (13 deps); shm ABI; teardown order |
| build-toolchain | (config) | cargo, cc, bindgen, build.rs/xtask for slangc | **Moderate** | 3–5 wk | Slang has no Cargo home; Jolt determinism flags on FFI shim |
| editor (codegen re-point only) | — | schemars + ts-rs + in-house emitter | **Trivial** | 0.5–1.5 wk | Mis-scoping as a "port"; u64-as-string id corruption |

**core-foundation** is the DAG root and a conventions-setting exercise more than a code task. The leverage is enormous: getting `Result<T,String>`, `Ref=Arc`, `Uuid(u64)` (deriving `Eq`/`Hash`, serialized as a decimal *string*), and the JSON union readers right makes ~20 downstream modules mechanical. The one trap is `Ref`→`Arc`: `shared_ptr` allows shared mutation, `Arc` does not, so any mutated-through-the-handle `Ref` becomes `Arc<Mutex>`/`Arc<RwLock>` — a per-site call, not an alias.

**geometry** is tractable but exacting. glam, gltf, tobj, image/stb cover the parsing/decode/math (~50–60%); the determinism-critical glue (the `cgltf_node_transform_world` reconstruction over the gltf crate's index-only API, the `BTreeMap` first-seen OBJ dedup, the FNV-1a `subIdFor`) and the `.smesh`/`.sanim`/`.smodel` byte format + validation are an in-house 1:1 port. The structs are a triple contract (disk format == container payload == GPU vertex buffer) — `#[repr(C)]` + `bytemuck` + size asserts, and pin glam `Vec3` (12B), never `Vec3A` (16B).

**scene + sceneedit** is one of the lower-risk subsystems despite the ECS dependency — it is pure CPU + math, SDL-free and Rendering-free. The deliberately tiny entt surface (one `view` site, one `storage()` walk, no groups/signals/snapshot) favors **hecs** for a close 1:1 over the heavier, hierarchy-opinionated bevy_ecs. The cost is faithful reproduction: byte-compatible JSON (field names, decimal-string uuids, enum spellings, all SceneVersion 1→4 migrations — the code is at 4; AGENTS.md saying 3 is stale), the gizmo numeric edge cases, the ZYX euler stability glam doesn't give for free, and the play-mode *JSON-roundtrip* duplicate (not a `World::clone`).

**animation** is the easiest meaningful port: pure CPU pose math, zero FFI, and glam's `Quat::from_xyzw` *deletes* the worst glm hazard. The real blockers are dependency order (it reads geometry's `.sanim` and scene's components) and the numerically delicate two-bone IK (signed-atan2 pole twist, a thicket of epsilons). Port the ~430-line self-test first as the oracle. Reject ozz-animation-rs — it would impose a second clip format and a jobs/SoA re-architecture and still lacks foot IK.

**script** is Moderate and a net safety win. mlua 0.11.6 supports Lua 5.5 (`lua55`) and *erases* the dominant hazard — the raw `lua_State` stack discipline and setjmp/longjmp boundary — which is the bulk of the C++. The hard part Rust *adds* is the borrowed-pointer lifetime invariant (`currentScene` non-null only inside a callback; userdata caches a raw `ScriptHost*`), which must be re-encoded as a scoped session guard, not a `&Scene` lifetime in `'static` userdata.

**physics** is Hard, and the difficulty is binding coverage + determinism, not LOC. The Jolt-free POD boundary is near-ideal to re-expose from Rust, but published crates (`joltc-sys`/`rolt`) pin Jolt 5.0.0 and lack CharacterVirtual, Ragdoll, Skeleton, SwingTwist+motors, RotatedTranslatedShape, ExtendedUpdate — ~40% of this subsystem's hardest logic. The port must author a `cxx` bridge to vendored 5.3.0 with C++-side shim classes for the ContactListener and filter interfaces (cxx can't synthesize virtual subclasses), then port the orchestration (fixed-step, ragdoll drive/blend/writeback, contact ring) 1:1 in Rust. glam's xyzw quaternion means the GLM-wxyz swizzle is *deleted*, not ported.

**rendering** is the single hardest part of the whole rewrite — but the difficulty is *not* `ash`. ash is the easy, mechanical layer and an unusually faithful match for the no-exceptions/no-RAII/PFN-dispatch style. The cost is (a) re-architecting the ~80-field by-reference aggregate into borrow-checker-legal sub-state, and (b) porting the RgUsage barrier engine, RAII drop-order, std430 layouts, shm ABI, and concurrency points (gpuQueueMutex/bindlessMutex/thread-local pool) with *zero* semantic drift, because each fails silently. Do not consider wgpu (can't express bindless-at-scale, RT pipelines, custom barriers, the exact ABIs) or vulkano (auto-sync fights the hand-derived barrier graph).

**assets** is Hard mainly because it can't be ported in isolation — it is orchestration on top of geometry (the real importers/codec) and rendering (GpuMesh/GpuTexture + ~30 scene setters). It contains the engine's single highest-coupling function, `renderScene`. The Rust port is genuinely *safer* in two places (GPU-resource teardown ordering via Drop/guard types; `std::process::Command` vs the hand-quoted slangc shell string) and neutral elsewhere. Preserve the negative-cache as `HashMap<u64, Option<Arc<T>>>`.

**control** is one of the *best* rewrite candidates — serde collapses ~7k LOC of DTO/generated serde to derives, the socket layer ports ~1:1 over `nix`, and the gen.ts fragility evaporates — *but* it is the integration hub (`EngineContext` holds live refs into six subsystems and ~142 handlers call deep into all of them), so it can only be ported **last** over already-Rust subsystems. Keep the synchronous, single-threaded, drain-once-per-frame model; do **not** introduce tokio.

**host + app + window** is Hard purely because of integration scope — it transitively needs all 13 modules, and runHost can't run until Physics(Jolt) and Script(Lua) exist behind FFI. Every low-level primitive has a mature crate, and the highest-risk surface (the shm seqlock) is *de-risked by an executable oracle*: the already-Rust `wayland_viewport.rs` consumer validates the exact bytes a producer must emit.

**build-toolchain**: Cargo cleanly subsumes CMake+FetchContent+presets and the entire import-std apparatus disappears. What it does *not* replace stays: the toolbox container (Vulkan SDK, slangc, SDL3, weston, no GitHub CI — inherited verbatim), the 40-shader slangc fan-out + lighting-module trick (hand-ported into build.rs/xtask), and the Jolt determinism flags re-applied to the FFI shim by hand.

**editor**: not a port at all. Re-point the protocol generator at Rust DTOs (schemars + ts-rs + a thin in-house OpenRPC/manifest emitter), keep the runtime wire byte-identical, and the editor is unchanged end-to-end.

---

## 4. Focus-Area Findings

### 4.1 ECS / entt iteration speed (hard constraint)

**Verdict: Rust can match — and on this engine's actual access patterns, plausibly beat — entt, without writing your own ECS.**

**Evidence.** The engine uses a *tiny, portable* subset of entt: exactly **one** `registry.view<C...>` site (inside `forEach<C...>`), exactly **one** `registry.storage()` iteration site (in `serializeEntity`), generational handles with `valid()`, `emplace_or_replace`/`all_of`/`try_get`, and `type_hash` as an in-memory join key. It uses **none** of entt's hard-to-replicate features — no groups, no entt signals, no `entt::observer`, no `snapshot`. The per-frame hot paths (`forEach` for transform sync / draw enumeration / light gather, `updateWorldTransforms`, `jointMatrices`, `renderScene`) are **iteration-dominated**, which is archetype/Table storage's strength; the paths that favor entt's sparse-set (`relinkHierarchy`, `enterPlay`, script add/remove) are **edge-triggered, not per-frame**. `bevy_ecs` 0.18 (standalone, no App/render machinery) offers per-component `#[component(storage = "SparseSet")]`, so you can keep cache-friendly columns for iterated components *and* recover entt-class O(1) add/remove for churny ones (e.g. per-frame-written `PoseOverrideComponent`) — a hybrid no single-model crate offers. Published numbers show modern archetype designs (gaia-ecs, same family as bevy Table) already match entt on random-access get (~2ns vs ~3ns) and batched add/remove, so the "archetype = slow structural change" worry is a solved problem.

**Caveat (honest).** The canonical Rust ECS benchmark suite was archived in 2022; the cleanest cross-language numbers are C++ (entt/flecs/gaia); no published 2026 apples-to-apples bevy_ecs-vs-entt benchmark exists for *this* engine's workload.

**Recommendation.** Use **bevy_ecs 0.18 standalone** if you want the per-component storage knob and a maintained, batteries-included ECS; use **hecs** if you prefer minimalism and the closest 1:1 with the free-function/`forEach` idiom (the scene-port mapping leans hecs for exactly this reason — the two recommendations differ in taste, and both are defensible). **Avoid `flecs_ecs`** (alpha binding, known aliasing-soundness hole, re-introduces a C-FFI dep) and **legion** (unmaintained). **Do not write your own** — no measured requirement justifies it today. **Mandatory before commit:** port `forEach<C...>` + a representative few-thousand-entity scene onto the chosen ECS and benchmark per-frame iteration *and* the `enterPlay`/`relinkHierarchy` structural paths against the current entt build. The user calls this non-negotiable; do not trust third-party numbers for the verdict.

### 4.2 Vulkan bindings

**Verdict: `ash` + `gpu-allocator` (or `vk-mem-rs`) + a hand-rolled bootstrap. Reject vulkano and wgpu.**

**Evidence.** ash is a thin, near-zero-overhead wrapper at the *same* abstraction level as Vulkan-Hpp (raw handles, explicit everything, `Result<T, vk::Result>` matching the engine's `checked()→Result` seam). It exposes everything used: KHR acceleration structure (all the PFN entry points `RtDispatch` resolves today), ray-tracing pipeline, descriptor-indexing bindless, dynamic rendering, sync2, timeline semaphores, calibrated timestamps, debug-utils labels — all loaded via the same explicit per-device extension pattern the engine uses now. vulkano does **automatic** GPU synchronization, which directly contradicts the hand-rolled RgUsage→derived-barrier graph and adds per-call validation overhead. wgpu's RT is experimental/ray-query-only with no RT pipelines, and its abstraction hides the barrier/bindless/timeline control the engine is built on.

**Version gap (the one real caveat).** ash stable is 0.38 (Vulkan 1.3.281); the engine targets 1.4. 0.39/1.4 is an in-progress, semver-breaking milestone. Start on 0.38 (all *used* extensions exist; 1.4 mostly promotes already-used KHR/EXT to core) and budget ~25% schedule risk for the 0.39 bump landing mid-port (and possible `vk-mem-rs` lag behind it).

**Allocator.** `vk-mem-rs` (FFI to the real AMD VMA) for behavioral 1:1 — keeps `vmaGetHeapBudgets` telemetry, persistent mapping, invalidate, defrag identical, at the cost of a C++ dep the engine already has. `gpu-allocator` (pure Rust) if dropping the C++ dep matters, but spike the budget/telemetry surface and note it has no defrag. **Bootstrap:** hand-roll device/swapchain selection + the `enable_*_if_present` feature-probe chain on ash (~150 LOC); no maintained Rust vk-bootstrap exists.

The difficulty is not ash — it is the renderer re-architecture (§3), the RAII drop order (device before allocator; bindless-slot reclaim before view destroy), the byte-exact std430/shm ABI, and the barrier-derivation fidelity, all of which ash lets you keep at exactly the abstraction level they live at today.

### 4.3 Physics determinism (hard constraint)

**Verdict: Keep Jolt, re-bind via a custom `cxx`/JoltC FFI. Reject rapier3d.**

**Evidence.** Both Jolt and rapier are *conditionally* deterministic, but only Jolt preserves the engine's exact, already-validated bit-exact-across-machines contract *and* its full feature set (CharacterVirtual stair/stick-to-floor, motor-driven SwingTwist ragdoll, the 5 shapes, sensors, contact events). rapier is the only mature pure-Rust engine and would remove the entire C++ FFI subproject — but it has no native motor-driven ragdoll/CharacterVirtual abstraction (you'd rebuild the hardest ~60% by hand on joints), and its `enhanced-determinism` mode is mutually exclusive with `parallel` and SIMD, i.e. *single-threaded* — a hard regression vs Jolt's `JobSystemThreadPool`. Switching engines also discards the bit-exact validation against Jolt 5.3.0 that lockstep/replay depends on.

**The determinism risk is not "which engine"** — it is that an off-the-shelf Rust Jolt crate builds Jolt *without* `JPH_CROSS_PLATFORM_DETERMINISTIC` / `-ffp-model=precise` / confined `-mavx2`, silently breaking bit-exactness. Published crates (`joltc-sys` 0.3.1+Jolt-5.0.0, `rolt`) also pin the *wrong Jolt version* and don't cover the advanced API the engine uses.

**Recommendation.** Vendor Jolt 5.3.0, own its build from `cxx-build`/build.rs with the determinism flags re-applied to the Jolt+shim TUs, author the `cxx` bridge (lifting `joltc-sys`/`rolt` rigidbody-core + filter-trait code where it covers), and **re-establish the determinism gate as a blocking CI test early**: run a fixed stacking/ragdoll scenario through both the C++ engine and the Rust bridge and diff sim traces for bit-exactness across x86 and ARM *before* porting gameplay on top. Pin Jolt 5.3.0 and treat version bumps as replay-format migrations. Consider upstreaming the missing CharacterVirtual/Ragdoll/SwingTwist coverage into JoltC to share the maintenance burden.

**Open question:** no authoritative benchmark of rapier-deterministic (SIMD/parallel off) vs Jolt-deterministic (AVX2 on) was found; the perf delta is inferred, not measured.

### 4.4 Lua scripting

**Verdict: mlua. Lua 5.5 is not a blocker. A net safety win.**

**Evidence.** mlua 0.11.6 (stable, Jan 2026) supports Lua 5.5 via the `lua55` feature and bundles lua-5.5.0 through lua-src-rs — the *same* upstream the engine vendors, so it's a binding swap, not a runtime swap. rlua is deprecated (archived, re-exports mlua). mlua is safe-by-default (replacing the manual `luaL_openselectedlibs` curation), adds `set_memory_limit` + instruction-budget `set_hook` (sandboxing the current runtime lacks), and — crucially — **eliminates the subsystem's dominant hazard**: the raw `lua_State` stack discipline and the setjmp/longjmp boundary (mlua guards both internally, converting Rust panics in callbacks to Lua errors). UserData/RegistryKey/metamethods map LuaBridge3's DSL cleanly; the dual-operand `__mul` becomes one `add_meta_function(MetaMethod::Mul, ...)`. mlua wraps the reference C VM, so execution speed equals native Lua 5.5; it is the fastest of the Rust embedded options and the only maintained Lua binding.

**Recommendation.** Port to mlua with `lua55` + `vendored`/external-link + `serde`. Re-encode the borrowed-pointer lifetime invariant (`currentScene` non-null only inside a callback) as a scoped session guard — the part Rust *adds*. Preserve the deferred-structural-op ordering, the SchedulerPrelude (verbatim Lua, installed via `raw_set` onto the read-only `sa` table), and rewrite the `check-script-defs` regex tripwire for Rust binding syntax. Reject rhai/rune — they'd force rewriting every `.lua` gameplay script for a slower, less-proven runtime.

### 4.5 Window + shm viewport bridge

**Verdict: `winit` + `raw-window-handle` (not the sdl3 crate) for the thin window layer; KEEP the two-process shm bridge as-is.**

**Evidence.** The engine needs only init/create/poll/get-instance-extensions/create-surface from the window — winit covers all of it, is ~250× more downloaded than the explicitly-WIP sdl3 crate, and feeds ash a surface via raw-window-handle. Better: in editor mode the window is *hidden and never presented* (shm publish path), and the surface is load-bearing only for present-capable device selection — a Rust port can create a **headless** instance in editor mode (select device by feature, not surface) and only spin up a window for the standalone present-only host. The shm bridge is the *simple* CPU-memcpy POSIX seqlock ring, **easier** to reproduce in Rust (`rustix`/`memfd` for shm_open/mmap, `std::sync::atomic::fence(Release)` for the seqlock) than in C++, and the reader (`wayland_viewport.rs`) is already Rust and unchanged — an executable oracle for the byte layout.

**Two-process vs collapse: keep two processes.** Folding the engine into the Tauri process is feasible (same `wl_subsurface` trick) but buys nothing: the Wayland fragility lives in the subsurface compositing (already isolated), not the process boundary, and Graphite/tauri#9220 show in-process webview+GPU surface contention is its own flicker hazard. The split gives a clean crash boundary, the existing parent-death watch, trivial standalone-host reuse, and a debuggable seam. Treat udmabuf/zero-copy as a deferred optimization, not part of the rewrite.

**Recommendation.** winit + raw-window-handle + ash; headless in editor mode; reproduce the shm header/ring/fence ordering and BGRA8 byte order exactly; keep the AF_UNIX control socket framing exactly (one newline-terminated reply per request, drained once per frame, answered within 5s).

### 4.6 Control-plane codegen + wire contract

**Verdict: DTOs as Rust structs deriving `serde` + `schemars` + `ts-rs`; replace gen.ts with a small Rust emitter.**

**Evidence.** This eliminates the bespoke 3504-line regex parser (which *throws* on a member containing `(`/`)`/`=`) **and** all ~5.7k LOC of generated C++ serde, because the Rust compiler is the parser. schemars 1.x (JSON Schema draft 2020-12, exactly what the engine declares) + `preserve_order` (field order == sa-CLI positional arg order) + ts-rs 12 (serde-compat TS emission) regenerate the editor-facing artifacts from one source of truth — collapsing the three-hand-tables-per-enum and four-place-scene-component hand-sync traps into derives. The `JSON_NOEXCEPTION` abort firewall (the whole reason `Saffron.Json` exists) disappears since serde returns `Result`.

**The load-bearing wire detail:** u64 ids cross as **decimal strings** (JS 2^53 limit); the contract test (`assertRawU64`) checks raw bytes. `serde_with`'s `PickFirst<(DisplayFromStr, _)>` replicates `uuidToJson` + `readWireUuid` exactly in one attribute (emit string, accept string-or-number). A default serde `u64` emits a JSON *number* and silently fails the gate. OpenRPC is the thin spot — `typed-openrpc` is early-stage; hand-roll ~100 lines over schemars fragments rather than depend on it.

**Recommendation.** Make Rust DTOs the source of truth with a `Uuid(u64)` newtype (`#[serde_as(... DisplayFromStr ...)]`, ts-rs `#[ts(type="string")]`). Keep the synchronous, single-threaded, drain-once-per-frame socket model over `nix` (`set_nonblocking`, `poll(POLLOUT)`, `MSG_NOSIGNAL`); **no tokio**. Carry every command's fixture/skip forward and keep the boots-a-headless-engine contract test green as the acceptance gate.

### 4.7 Whole-codebase idiom translation

**Verdict: idioms translate well — several save code — but "mechanical" hides a real ownership tax in the two largest subsystems.**

Realistic split of ~50k engine LOC: **~55–65% mechanical** (leaf modules, serde, error plumbing, POD/byte types, math — these come out *shorter* in Rust), **~25–30% ownership re-architecture** (the Renderer state split, the scene/asset graph, `host.cppm`, the self-referential/aliasing caches), **~10–15% FFI re-binding** with little Rust leverage (ash/Jolt/Lua + the cross-process ABIs). `Result`+`?`, Drop, data-carrying enums, trait/closure objects, serde-derive, and the Cargo crate graph are all net wins. `Ref→Arc` *bites* (per-site `Arc<Mutex>` analysis for shared-mutable handles, already marked by the existing `bindlessMutex`/`gpuQueueMutex`). Drop is a clean fit but cross-object teardown *order* is a runtime UAF if wrong, not a compile error. `SubscriberList` has no crate matching its stop-propagation + re-entrant-snapshot contract — hand-roll the ~50 lines. **Do not scope the Renderer aggregate, the scene/asset graph, or `host.cppm` as mechanical** — that is where the schedule actually goes.

---

## 5. Dependency Replacement Matrix

| C++ dependency | Rust equivalent | Maturity | Risk |
|---|---|---|---|
| EnTT 3.16 | `bevy_ecs` 0.18 (standalone) **or** `hecs` 0.11 | High (bevy_ecs huge/active; hecs mature/minimal) | **Medium** — must benchmark `forEach` throughput; rewrite `serializeEntity` storage walk; play-duplicate handle aliasing |
| Vulkan-Hpp (`vk::`, NO_EXCEPTIONS) | `ash` 0.38 (→0.39 for Vulkan 1.4) | High (~2.5M dl/mo, ecosystem standard) | **Medium** — stable is Vk 1.3; 0.39/1.4 is an in-progress breaking bump; raw handles, no RAII |
| VMA 3.3 | `vk-mem-rs` 0.5 (real VMA) **or** `gpu-allocator` 0.28 (pure Rust) | vk-mem: Medium (single-maintainer fork, pinned to ash 0.38). gpu-allocator: High | **Medium** — vk-mem bus-factor + ash-version lag; gpu-allocator diverges (no defrag, different budget API) |
| vk-bootstrap | **none** — hand-roll on ash | N/A | **Low** — ~150 self-contained LOC; the feature-probe/degradation chain must be hand-ported branch-for-branch |
| SDL3 3.4 | `winit` 0.30 + `raw-window-handle` 0.6 (or `sdl3`/`sdl3-sys`) | winit: High. sdl3 crate: pre-stable WIP | **Low** — engine uses a tiny window surface; editor mode can go headless |
| JoltPhysics 5.3.0 | **custom `cxx` bridge to vendored Jolt 5.3.0** (lift from `joltc-sys`/`rolt`) | `cxx`: High. `joltc-sys`/`rolt`: Low, pin Jolt 5.0.0, miss ragdoll/character/constraints | **High** — coverage gap + determinism must be proven bit-exact cross-arch |
| Lua 5.5.0 | `mlua` 0.11.6 (`lua55`, `vendored`) | High (lua55 stable Jan 2026; same upstream) | **Low** — strong fit; deletes the hand-written stack code |
| LuaBridge3 | `mlua` UserData/metamethod API | High | **Low–Medium** — re-express the binding DSL; rewrite the drift tripwire |
| nlohmann/json (`JSON_NOEXCEPTION`) | `serde` + `serde_json` (+ `serde_with`) | High (gold standard) | **Low** — but decimal-string-u64 + lenient `*Or` readers must be preserved exactly |
| GLM 1.0 | `glam` 0.30+ | High (de-facto game math) | **Low** — no global `DEPTH_ZERO_TO_ONE` (use `*_rh` 0..1 per-projection); quaternion is xyzw (often *deletes* a swizzle); ZYX euler stability hand-ported |
| cgltf | `gltf` 1.4 | High | **Medium** — index-only API; reconstruct parent map + world-transform walk + node ordering by hand |
| tinyobjloader | `tobj` 4.0 | High | **Low–Medium** — hand-port the `BTreeMap` first-seen dedup to preserve determinism |
| stb_image / stb_image_write | `image` 0.25 (or an stb binding for bit-parity) | High | **Low** — pure-Rust decode can differ at bit level; use stb binding if existing texture hashes must match |
| nanosvg / nanosvgrast | `resvg` + `usvg` + `tiny-skia` | High (more complete than nanosvg) | **Low** — may rasterize icons slightly differently |
| Slang (`slangc`) | **keep the `slangc` binary** via build.rs/xtask (NOT `shader-slang`/`shaderc`) | slangc: pinned binary. `shader-slang` crate: immature (0.1.0) | **Low–Medium** — Cargo compiles no shaders; hand-port the 40-shader fan-out + lighting-module trick with its own staleness tracking |

---

## 6. Migration Strategy

**Three options weighed:**

**(A) Big-bang rewrite.** Port everything, switch over once. *Rejected as the primary plan.* The engine is too large (~50k LOC) and too feature-complete to go dark for the months a big-bang implies, and you'd discover the renderer/physics/host integration risks only at the end.

**(B) Incremental FFI bridge** — keep the C++ engine behind a C ABI and replace module-by-module from Rust. *Rejected.* The engine's seams are deep by-reference aggregates (`Renderer&`, `Scene&`, `EngineContext` of six live references), not C-ABI-friendly boundaries. A bridge would force FFI-wrapping nearly the whole engine surface (especially to feed `EngineContext` to control, or to share the renderer aggregate), buying little while doubling the maintenance surface — and it directly violates the project's no-compat-shims / one-code-path rule. The *only* clean C-ABI boundaries that already exist (the shm ring, the control socket) are cross-*process*, and those are exactly what the editor uses — so the natural "bridge" is the existing wire contract, not a per-module C ABI.

**(C) Hybrid: phased pure-Rust rewrite that reproduces the cross-process contracts. ✅ Recommended.** Build a *new* Rust engine binary that speaks the identical shm + control-socket contracts the editor already consumes. The editor never knows the difference. Within the engine, port leaf-up so each layer compiles and self-tests before its consumers arrive; the editor stays on the C++ `SaffronAnima` (via `SAFFRON_ANIMA_BIN`) until the Rust binary passes the full e2e/contract gate, then you flip the binary. This is incremental at the *binary* boundary (you can run either engine) without an in-process FFI bridge.

**Build/toolchain change.** Cargo replaces the entire CMake + FetchContent + CMakePresets + C++26-modules + `import std` + BMI-matching + two-ninja-`.pcm`-race apparatus — a **real, large simplification**, and the most unambiguous win of the whole effort. **What stays container-bound and unchanged:** the toolbox (Vulkan 1.4 SDK, SDL3, headless weston), `slangc` (invoked from build.rs/xtask exactly as `CompileShaders.cmake` does, including the `lighting.slang`→module precompile), the Jolt determinism build flags (re-applied to the FFI shim by hand), and the no-GitHub-CI / self-hosted-runner constraint. Carry the Makefile/toolbox environment lore (NVIDIA ICD `VK_ADD_DRIVER_FILES`, `WEBVIEW_HW`, host-runnable `sa`) into a justfile verbatim.

---

## 7. Effort & Risk Register

**Total rough effort.** Summing the per-subsystem mapping estimates (each assuming its dependencies are already ported) gives ~**80–120 person-weeks** of subsystem engineering. Add integration of the host apex, the spike, toolchain, the determinism/e2e/contract gates, and normal overhead (~25–40%), and a realistic figure for **one experienced Rust+graphics dev is roughly 2–3 years; for a small focused team (2–3), roughly 9–18 months.** Rendering, physics, and assets+host dominate. A dev strong in graphics but new to Rust: roughly double the rendering/physics slices. This is a major investment whose return is memory-safety, a vastly simpler build, and a unified Rust/Tauri stack — *not* new engine capability.

**Ranked risks:**

1. **Renderer ownership re-architecture + silent-failure surface (rendering).** The ~80-field aggregate is a genuine design problem, and the barrier graph / std430 layouts / shm ABI / concurrency points all fail silently. *Highest cost and highest correctness risk.*
2. **Physics determinism cross-arch (physics).** The lockstep-netcode premise hinges on bit-exactness surviving a from-source Jolt FFI rebuild; the binding coverage gap (ragdoll/CharacterVirtual/SwingTwist) is real hand-engineering.
3. **ECS iteration throughput (scene).** Satisfiable but the user's non-negotiable constraint — must be benchmarked, not assumed.
4. **Schedule / scope underestimation.** "Mechanical" hides the ~25–30% ownership-redesign tax; the host integration apex can't even run until 13 deps exist.
5. **Wire-contract regression (control/editor).** Decimal-string-u64 and BGRA byte order corrupt silently; the e2e/contract gate is the only detector.
6. **Crate-version churn.** ash 0.38→0.39 mid-port; vk-mem lag; mlua `lua55` is stable but recent; `typed-openrpc`/`shader-slang` immature (correctly avoided).
7. **Toolchain residue.** Slang and the toolbox don't go away; the Jolt flags must be hand-carried.

**What would most reduce risk:** a **time-boxed spike** (below) that converts the three top risks from unknowns into measured facts *before* the large commitment. Secondarily, treating the existing validation-layer-clean log, the headless e2e suite, and the `check-control-schema` contract test as **first-class, continuously-green deliverables** from day one — they are the only automated detectors for the entire silent-failure class.

---

## 8. Recommended First Steps (De-Risking Spike Sequence)

A ~4–6 week spike, before any go decision. Each step has a binary pass/fail that informs go/no-go.

1. **Foundation + conventions (week 1).** Port `core`/`signal`/`json` to Rust (`Result<T,String>`, `Arc`, `Uuid(u64)` serialized as decimal string, hand-rolled `SubscriberList`, serde-based JSON gateway). Write the conventions doc that pins `Ref→Arc` vs `Arc<Mutex>`, the `?`-vs-check-immediately style, and the JSON union readers. *Pass: self-tests green, decisions documented.* Cheap, high-leverage, sets the whole translation tone.

2. **Physics determinism gate (weeks 1–3, parallel).** Stand up `cxx-build` compiling vendored Jolt 5.3.0 with `CROSS_PLATFORM_DETERMINISTIC` + single precision + matching arch/FP flags under clang+libc++; get **one** SwingTwist-motor ragdoll and **one** `CharacterVirtual::ExtendedUpdate` working through `cxx` with a Rust-callback `ContactListener`; run a fixed scenario through both the C++ engine and the Rust bridge and **diff sim traces for bit-exactness across x86 and ARM.** *Pass: bit-identical traces + the two hard features bound.* **If this fails, the lockstep premise collapses and the decision should change.**

3. **Renderer bring-up + barrier graph + shm ABI (weeks 2–5).** On ash 0.38 + vk-mem-rs + winit (headless), hand-write the feature-probe chain and RAII Drop wrappers, reach a **validation-clean** clear+present; port the RgUsage barrier engine literally and run it under the validation layer continuously; reproduce the POSIX-shm publish and validate it **frame-by-frame against the unchanged Tauri presenter.** *Pass: validation-clean offscreen render shows live in the real editor.* This exercises the single hardest subsystem's foundation and the highest-risk cross-process contract against an executable oracle.

4. **ECS iteration benchmark (week 3, parallel).** Port `forEach<C...>` + a representative few-thousand-entity scene onto the chosen ECS (start with hecs for fit, fall back to bevy_ecs if the sparse-set knob is needed); benchmark per-frame iteration *and* the `enterPlay`/`relinkHierarchy` structural paths against the current entt build. *Pass: per-frame iteration within ~10% of entt.*

5. **Control + editor seam (week 5–6).** Stand up a minimal Rust control server (serde + nix, single-threaded drain) speaking the exact envelope with decimal-string ids; re-point the protocol generator (schemars + ts-rs + thin OpenRPC emitter) and confirm the editor compiles unchanged and a slice of the e2e suite passes. *Pass: editor drives the Rust stub over the unchanged wire.*

**Go/no-go after the spike:** proceed to the full phased rewrite only if steps 2, 3, and 4 all pass. If the determinism gate (2) fails, stop and reconsider (or descope lockstep netcode). If the renderer/shm spike (3) reveals the aggregate re-architecture is larger than scoped, re-estimate before committing. The spike's whole purpose is to spend ~5 weeks buying certainty on the three things that otherwise dominate a multi-year risk.

Relevant absolute paths for the spike: `/var/home/saffronjam/repos/SaffronEngine/engine/source/saffron/rendering/` (renderer + `renderer_capture.cpp` shm publish), `/var/home/saffronjam/repos/SaffronEngine/engine/source/saffron/physics/physics.cpp` (the sole Jolt TU), `/var/home/saffronjam/repos/SaffronEngine/engine/source/saffron/scene/scene.cppm` (`forEach`/`serializeEntity`), `/var/home/saffronjam/repos/SaffronEngine/engine/source/saffron/control/control_dto.cppm` (codegen source of truth), and `/var/home/saffronjam/repos/SaffronEngine/editor/src-tauri/src/wayland_viewport.rs` (the shm reader that is the producer's byte-exact oracle).