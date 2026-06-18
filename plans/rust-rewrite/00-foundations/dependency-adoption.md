# Dependency adoption & mapping (PP-2 annex)

This annex locks every third-party crate the Rust rewrite adopts: a confirmed pick, a pinned
version, a one-paragraph integration verdict, and an explicit list of **what the crate does NOT
cover** (the gap we hand-roll). It is the source of truth for the `[workspace.dependencies]` table
that `01-build-and-toolchain/` writes; member crates pull every dep via `dep.workspace = true` (the
single-place pin that replaces `cmake/Dependencies.cmake`'s `FetchContent` tags). It refines the
feasibility study's [§5 replacement matrix](../../rust-rewrite-feasibility.md) against the real C++
usage in `engine-old/`; it does not re-litigate the picks the pre-planning already settled (Luau,
two-process, frozen wire).

This is an annex, not a phase: no `phase-N` files, no acceptance gate of its own. The pins land in
the workspace as part of `phase-1-workspace-scaffold.md`; each gap below is discharged by the phase
of the area that owns the subsystem (cited per row).

Two pins stay **placeholders** here by PP-1's open questions and are owned elsewhere: the ECS crate
(`hecs` vs `bevy_ecs`) is PP-4's benchmark-gated call (`03-ecs-and-scene/`), and the Vulkan
allocator (`vk-mem` vs `gpu-allocator`) is decided in this annex's §3 but its rendering integration
belongs to PP-5 (`06-rendering/`). Versions are the latest stable as of 2026-06; the workspace pins
exact minor versions and `Cargo.lock` is committed (the binary-reproducibility replacement for
`GIT_TAG` shallow clones).

---

## 1. The pin table (one place, mirrors `[workspace.dependencies]`)

| C++ dep (`Dependencies.cmake`) | Rust crate | Pin | Used by crate(s) | Gap owner |
|---|---|---|---|---|
| Vulkan-Hpp (raw C API) | `ash` | `0.38` | saffron-rendering | §2 |
| — (ash window-surface glue) | `ash-window` | `0.13` | saffron-rendering | §2 |
| SDL3 `SDL_Window` | `raw-window-handle` | `0.6` | saffron-window, saffron-rendering | §5 |
| VMA 3.3 | `vk-mem` | `0.4` | saffron-rendering | §3 |
| vk-bootstrap 1.4.352 | **none — hand-roll** | — | saffron-rendering | §4 |
| SDL3 3.4 (window) | `winit` | `0.30` | saffron-window | §5 |
| EnTT 3.16 | `hecs` **or** `bevy_ecs` (PP-4) | deferred | saffron-scene | §6 |
| GLM 1.0 | `glam` | `0.30` | saffron-geometry (+ most) | §7 |
| Lua 5.5 + LuaBridge3 | `mlua` (`luau`, `vendored`) | `0.11` | saffron-script | §8 |
| nlohmann/json `JSON_NOEXCEPTION` | `serde` + `serde_json` | `1.0` / `1.0` | saffron-json (+ protocol) | §9 |
| (decimal-string-u64 wire) | `serde_with` | `3` | saffron-protocol | §9 |
| (JSON Schema emit) | `schemars` | `0.8` | saffron-protocol, xtask | §9 |
| (`@saffron/protocol` TS) | `ts-rs` | `10` | saffron-protocol, xtask | §9 |
| JoltPhysics 5.3.0 | `cxx` + `cxx-build` + vendored Jolt | `cxx 1.0`, Jolt `v5.3.0` | saffron-physics-sys | §10 |
| cgltf 1.15 | `gltf` | `1.4` | saffron-geometry | §11 |
| tinyobjloader 1.0.6 | `tobj` | `4.0` | saffron-geometry | §11 |
| stb_image / stb_image_write | `image` | `0.25` | saffron-geometry, saffron-rendering | §11 |
| nanosvg / nanosvgrast | `resvg` (+ `usvg`, `tiny-skia`) | `0.44` | saffron-rendering | §11 |
| Slang `slangc` | **keep the binary** (build.rs/xtask) | pinned binary | xtask | §12 |
| (control socket syscalls) | `rustix` | `0.38` | saffron-control, saffron-host | §13 |
| (`tools/sa` arg parse) | `clap` (`derive`) | `4` | sa | §13 |
| (`#[repr(C)]` GPU structs) | `bytemuck` (`derive`) | `1` | saffron-rendering, saffron-geometry | §14 |
| `core::base64Encode` | `base64` | `0.22` | saffron-core | §15 |
| (bin-crate error ergonomics) | `anyhow` | `1` | saffron-host, sa, xtask | §9 |
| (typed errors) | `thiserror` | `2` | every lib crate | §9 |

`enable_language(C)` + the four impl TUs (`vma_impl.cpp`, `stb_impl.cpp`, `cgltf_impl.cpp`,
`tinyobjloader_impl.cpp`, `nanosvg_impl.cpp`) and the `saffron_third_party` aggregate interface
target all **disappear**: each is replaced by a crate that owns its own build (or, for `vk-mem` and
Jolt, by the crate's / `*-sys`'s own `cc`/`cxx-build`).

---

## 2. `ash` 0.38 — Vulkan

**Verdict.** Confirmed. `ash` is the ecosystem-standard thin Vulkan binding: raw handles, no RAII,
every call `unsafe`. It is a 1:1 substrate for the C++ tree, which already drives the **raw C API**
(`find_package(Vulkan)`, "we use the raw C API, not vulkan.hpp/raii"), so there is no
`vk::`-to-`ash` semantic gap — the renderer already thinks in `Vk*` structs. The C++ tree targets
**Vulkan 1.4** (`renderer.cppm:138` `require_api_version(1, 4, 0)`, `VK_API_VERSION_1_4` for VMA at
`:422`) and chains `VkPhysicalDeviceVulkan11/12/13/14Features`. `ash` 0.38 already exposes the 1.4
core symbols and all four `PhysicalDeviceVulkan1xFeatures` builders, plus the KHR
acceleration-structure / ray-query / deferred-host-ops extension structs (`renderer.cppm:196-243`)
and `VK_EXT_memory_budget` / `VK_EXT_calibrated_timestamps` / pipeline-statistics surfaces
(`renderer.cppm:204-217`). Pinned at **0.38** (not the in-progress 0.39 bump) per the feasibility
risk note; the 0.38→0.39 churn is a known mid-port hazard tracked by §16. `#![allow(unsafe_code)]`
with a "ash handle seam" justification — this is one of the three FFI crates.

**Does NOT cover (hand-roll):**
- **No builder / loader convenience beyond the raw entry points.** `ash::Entry`/`Instance`/`Device`
  load function pointers; everything else (instance creation, device creation, queue selection) is
  raw `create_*` calls — which is exactly the vk-bootstrap gap in §4.
- **No allocator** — §3.
- **No `vk::Result` → `std::expected` auto-wrap.** `ash` returns `VkResult` / `Result<T, vk::Result>`;
  the renderer's per-call `Err(...)`-on-the-spot discipline becomes a `Result<T, rendering::Error>`
  with a `From<vk::Result>` (the typed-error model of `conventions.md`), checked via `?`.

## 2b. `ash-window` 0.13 — surface from a window handle

**Verdict.** Confirmed. The C++ tree builds the surface via `SDL_Vulkan_CreateSurface`
(`renderer.cppm:154`) and the instance-extension list via `SDL_Vulkan_GetInstanceExtensions`
(`renderer.cppm:133`). With `winit` (§5) replacing SDL, `ash-window` is the matching glue:
`ash_window::create_surface(&entry, &instance, raw_display_handle, raw_window_handle, None)` and
`ash_window::enumerate_required_extensions(raw_display_handle)` reproduce both calls 1:1 over
`raw-window-handle` 0.6. Lives inside saffron-rendering (the FFI crate), not saffron-window.

**Does NOT cover:** the **headless** path. In editor mode the engine creates a headless instance and
selects the device by feature, never by surface (PP-10) — there is no window, so `ash-window` is used
only by the standalone present-only host. The headless instance-extension list (no
`VK_KHR_surface`/platform surface) is assembled by hand.

## 3. Allocator — `vk-mem` 0.4 (real VMA), NOT `gpu-allocator`

**Decision (PP-2's call, per Open Decision §5 of the pre-plan).** Pick **`vk-mem`** (the Rust binding
to the real AMD VMA, the same C++ library the engine vendors at `v3.3.0`), not `gpu-allocator`. The
deciding factor is the **telemetry + behavioral-parity surface the renderer already depends on**:
`VK_EXT_memory_budget` feeds `vmaGetHeapBudgets` for per-frame VRAM telemetry (`renderer.cppm:200`,
`:203`), and the VMA allocator is created with `vulkanApiVersion = VK_API_VERSION_1_4`
(`renderer.cppm:422`) and the instance/device wired in (`:419-421`). `gpu-allocator` is pure-Rust
(no C++ dep, attractive) but diverges: no defragmentation, a different budget API, and different
block-placement heuristics — adopting it would force re-validating every allocation path and lose
`vmaGetHeapBudgets` parity. `vk-mem` is a behavioral 1:1: same library, same budget call, same flags.

**Verdict.** Confirmed `vk-mem` 0.4. Cost accepted: it is a single-maintainer fork pinned to **ash
0.38** — which *aligns* with our ash pin (§2), turning the feasibility "ash-version lag" risk into a
reason the ash pin stays at 0.38 until `vk-mem` moves (tracked §16). It builds its own bundled VMA C++
via `cc`, so the `vma_impl.cpp` TU + `VMA_IMPLEMENTATION` dance is deleted.

**Does NOT cover (hand-roll):**
- **The allocator is not the device.** `vk-mem::Allocator` wraps the VMA handle but the engine still
  owns instance/device/queue creation itself (§4).
- **Drop order.** VMA must be destroyed *before* the `VkDevice` (`vmaDestroyAllocator` then device).
  The C++ relies on Ref-drop choreography in `onExit` + `waitGpuIdle`; in Rust this is an explicit
  `Drop`-sequence / field-order concern, owned by PP-10 (`08-host-and-viewport/`), not free.

## 4. vk-bootstrap — **NO crate, hand-rolled on `ash`**

**Verdict.** There is no Rust vk-bootstrap; the feature-probe / graceful-degradation chain is
hand-ported branch-for-branch onto `ash`. This is the single largest "looks like a dep, is actually
code" item and it is **not low-effort prose** — it is the load-bearing device-selection logic in
`renderer.cppm:135-261`:
- instance: app/engine name, `require_api_version(1,4,0)`, validation layers + debug callback
  (`onVulkanMessage`), instance extensions from the window (`renderer.cppm:135-145`);
- physical-device select: `set_minimum_version(1,4)` + **required** `features11/12/13/14`
  (`shaderDrawParameters`, the bindless `runtimeDescriptorArray` /
  `descriptorBindingSampledImageUpdateAfterBind` / `shaderSampledImageArrayNonUniformIndexing`,
  `bufferDeviceAddress`, `dynamicRendering`, `synchronization2`) — selection **fails with a clear
  error** if absent (`renderer.cppm:160-189`);
- **optional** extension probing that must NOT gate selection: RT (`VK_KHR_acceleration_structure` +
  `ray_query` + `deferred_host_operations`, enabled only when both feature bits read back true via a
  manual `vkGetPhysicalDeviceFeatures2` chain, `renderer.cppm:191-243`), `VK_EXT_memory_budget`,
  `VK_EXT_calibrated_timestamps`, `pipelineStatisticsQuery`, `fillModeNonSolid` (each
  `enable_*_if_present`, recording a bool the renderer reads later);
- device + queue: build, fetch graphics queue + family index (`renderer.cppm:245-261`);
- swapchain: `vkb::SwapchainBuilder` (`renderer_detail.cppm:130`).
**Cited gap owner:** PP-10 / `06-rendering/` device-bring-up phase. The `vkb::Instance`/`vkb::Device`
RAII holders (`renderer_types.cppm:1038-1039`) become plain `ash` handles with explicit teardown.

## 5. `winit` 0.30 + `raw-window-handle` 0.6 — window

**Verdict.** Confirmed `winit` over the `sdl3` crate (still pre-stable WIP). The engine's window
surface is tiny (the SDL usage is `SDL_Vulkan_GetInstanceExtensions` + `SDL_Vulkan_CreateSurface` +
the typed event signals in `Saffron.Window`), and editor mode is headless (no window at all), so
`winit`'s event-loop model is a clean fit. `raw-window-handle` 0.6 is the handle `ash-window` (§2b)
consumes. saffron-window wraps `winit` behind the typed-signal facade (`onResize`, `onKeyPressed`, …)
the hand-rolled `SubscriberList` events drive.

**Does NOT cover (hand-roll):**
- **The `winit` 0.30 `ApplicationHandler` ownership model differs from a poll loop.** The C++
  `run(config)` is `poll → onUpdate → … → present`; `winit` 0.30 drives via `ApplicationHandler`
  callbacks. Reconciling the run loop into this model (or using `pump_events` for a poll-shaped loop)
  is PP-10's design (`08-host-and-viewport/`), not winit-provided.
- **Headless has no winit window** — the editor path constructs no window; only the present-only host
  does (§2b).
- **The X11 child-window embedding** (`find_package(X11)`, the native-viewport bridge) is **gone** in
  the two-process/shm model — not a winit gap, a subtraction (PP-3 ledger).

## 6. ECS — `hecs` 0.11 vs `bevy_ecs` 0.18 — **deferred to PP-4 (benchmark-gated)**

**Verdict.** Pin deferred. PP-1 fixed saffron-scene's dependency *edge* but not the crate behind it;
PP-4 picks by benchmark (per-frame `forEach` within ~10% of entt). This annex only records the tiny
real surface both must satisfy so neither pick is a surprise: one `registry.view<C...>` iteration
site, the `registry.storage()` walk in `serializeEntity`, generational handles,
`emplace_or_replace`/`all_of`/`try_get`, `type_hash` joins, the play-mode JSON-roundtrip duplicate
(not a `World::clone`) — **no** groups/signals/observer/snapshot. `hecs` is the closer 1:1 to the
`forEach` idiom; `bevy_ecs` has the `SparseSet` storage knob. **Gap owner:** PP-4 / `03-ecs-and-scene/`.

## 7. `glam` 0.30 — math

**Verdict.** Confirmed. `glam` is the de-facto game-math crate and replaces GLM 1.0 directly.
Two pins matter and are load-bearing against real layouts:
- **No global `GLM_FORCE_DEPTH_ZERO_TO_ONE`.** The C++ sets it on `saffron_third_party`
  (`Dependencies.cmake:163`) so every `glm::perspective` emits Vulkan [0,1] depth. `glam` has no
  global flag; use the per-projection `*_rh` 0..1 constructors (`Mat4::perspective_rh`) at each call
  site — a mechanical per-site change, not a build flag.
- **Quaternion is xyzw.** GLM is wxyz. The `.sanim`/`.smodel` byte format stores quaternions
  **w,x,y,z** (`geometry.cppm` `importedNodesToJson`: "quaternion as w,x,y,z"), so the format
  reader/writer must reorder on the byte boundary even though `glam::Quat` is xyzw in memory. Same
  for the Jolt bridge — glam's xyzw matches Jolt's `Quat`, which *deletes* the GLM-wxyz swizzle
  (PP-11).

**Does NOT cover (hand-roll):**
- **`Vec3` vs `Vec3A` is a hard `#[repr(C)]` pin.** The std430 GPU structs use standalone
  `glm::vec3` members at 12-byte stride inside otherwise-vec4 structs (`renderer_types.cppm:240-241`,
  `:564`, `:1174`, etc.), and `static_assert(sizeof(MaterialParamsData) == 96)`
  (`renderer_types.cppm:1891`). `glam::Vec3` is 12 bytes (matches `glm::vec3`); `glam::Vec3A` is
  16-byte aligned. The GPU-struct mirrors must use **`Vec3`** (or raw `[f32;3]`) to keep stride, and
  the `bytemuck`+size-assert strategy (§14) re-encodes every `static_assert(sizeof…)`. Pin owned by
  PP-5 (`06-rendering/`).
- **ZYX euler decomposition stability.** GLM's euler extraction order/stability is hand-ported where
  the engine relies on it (gizmo / inspector rotation display); glam's euler helpers differ.

## 8. `mlua` 0.11 (`luau`, `vendored`) — scripting

**Verdict.** Confirmed, and the VM flips from stock Lua 5.5 to **Luau** (locked in pre-plan §0; the
editor `todo.md` wants the gradual type system). `mlua` with the `luau` feature provides Luau's
gradual types, built-in sandboxing (`Lua::sandbox`), determinism (relevant to the lockstep premise),
and the instruction-budget interrupt hook. This **deletes** the entire LuaBridge3 dependency
(`#include <LuaBridge/LuaBridge.h>`, `script.cppm:12`) and the vendored `lua_static` C target
(`Dependencies.cmake:35-66`): `mlua`'s trait-based `IntoLua`/`FromLua` + `UserData` metamethod API
replaces LuaBridge's `getGlobalNamespace().beginNamespace("sa").addFunction(...)` surface
(`script.cppm:283-285`), and the manual `luaL_newstate`/`luaL_openselectedlibs`/`luaL_loadbufferx`
stack code (`script.cppm:277-300`) collapses into safe `mlua` calls.

**Does NOT cover (hand-roll):**
- **The typed `sa.*` surface is NOT a crate output.** mlua registers bindings but emits no Luau type
  defs. The single-source binding-plus-typegen layer (one Rust source that both registers with the VM
  and emits `.d.luau`) is designed by PP-8 (`12-scripting/`), reusing PP-7's codegen skeleton; this
  deletes the hand-written `library/sa.lua` overlay and its drift tripwire.
- **The borrowed-pointer session guard.** The C++ `currentScene` raw-pointer-valid-only-inside-a-
  callback invariant (`script_runtime.cpp`) has no mlua analogue; the scoped session guard is the
  part Rust *adds* (PP-8).
- **`luabridge::LuaRef`-based JSON marshalling.** `jsonToLua`/`luaToJson`
  (`script_runtime.cpp:46-92`) re-expressed over `mlua::Value` + serde.

## 9. serde stack — JSON, wire, schema, errors

**`serde` 1.0 + `serde_json` 1.0.** Confirmed. Replaces nlohmann/json. The `JSON_NOEXCEPTION`
abort-firewall (`Dependencies.cmake:162`) **vanishes**: serde returns `Result`, and the engine's
parse failures become typed `saffron_json::Error` variants (a `String` parser-message payload is
acceptable per the error model). saffron-json owns the lenient typed readers (`json_u64`/`string`/
`f64`/`bool` + `*_or`) and the imperative decimal-string-u64 encoding.

**Does NOT cover (hand-roll):**
- **Decimal-string-u64 ids.** The wire encodes `u64` entity/asset ids as **decimal strings**
  (`assertRawU64`, the `Uuid(u64)` newtype); the editor reader is frozen, so this is byte-exact. In
  saffron-protocol the `Uuid` newtype derives `serde_with::PickFirst<(DisplayFromStr, _)>` (emit
  string, accept string-or-number); `serde_with` 3 provides exactly that adapter. saffron-json
  provides the imperative `uuid_to_json`/`json_u64` helpers; a contract test (PP-7/PP-13) asserts the
  derive and the helpers emit byte-identical wire output (open question carried from PP-1).
- **`schemars` 0.8 / `ts-rs` 10** are the JSON-Schema and `@saffron/protocol` TS emitters
  (replacing `gen.ts`); they cover the schema/TS surface but **not** OpenRPC or the command manifest
  — those are hand-rolled ~100-line emitters over schemars fragments. Gap owner: PP-7
  (`10-protocol-codegen/`).

**Error model crates: `thiserror` 2 + `anyhow` 1.** Confirmed (matches `conventions.md`): every
library crate derives a `thiserror::Error` enum + `Result<T>` alias; `anyhow` only in `[[bin]]`
crates (saffron-host, sa) and xtask. This replaces the C++ `Result<T, std::string>` + `Err("msg")` +
check-immediately model wholesale.

## 10. `cxx` + vendored Jolt 5.3.0 — physics FFI

**Verdict.** Confirmed: a custom `cxx`/JoltC bridge to **vendored Jolt `v5.3.0`** (the same tag the
C++ vendors, `Dependencies.cmake:85`), NOT the published `joltc-sys`/`rolt` crates — those pin Jolt
**5.0.0** and miss CharacterVirtual, Ragdoll, Skeleton, SwingTwist+motors, RotatedTranslatedShape,
and ExtendedUpdate, all of which the engine uses (the physics status in AGENTS.md). `cxx` 1.0 +
`cxx-build` compile the bridge and Jolt itself from `build.rs`. This is the `saffron-physics-sys`
crate (an FFI `*-sys` crate, `#![allow(unsafe_code)]`).

**Does NOT cover (hand-roll) — fully designed by PP-11 (`05-physics-jolt-bridge/`):**
- **The determinism build flags.** `CROSS_PLATFORM_DETERMINISTIC ON`, `DOUBLE_PRECISION OFF`,
  `-ffp-model=precise`, and the **confined `-mavx2`** (the C++ captures Jolt's
  `INTERFACE_COMPILE_OPTIONS` and re-applies them to only the physics TU, dropping `-pthread`:
  `Dependencies.cmake:103-109`). In Rust these live in `build.rs` and are confined to the Jolt + shim
  TUs only — the `*-sys` crate is the whole arch-flag blast radius (the C++ `pimpl`/single-TU
  isolation becomes crate-boundary isolation, free).
- **The C++ shim classes `cxx` cannot synthesize.** Virtual subclasses — `ContactListener`, the
  object-layer/broadphase filter interfaces — are written as C++ in the shim, routing to Rust
  callbacks. `cxx` bridges POD + opaque types, not arbitrary vtable subclassing.
- **The `-Werror` drop** Jolt needs (`Dependencies.cmake:93`): `build.rs` must not pass `-Werror` to
  Jolt's TUs (clang 21 flags `-ffp-model=precise + -ffp-contract=off` under `-Woverriding-option`).

## 11. Importers & images — `gltf`, `tobj`, `image`, `resvg`

**`gltf` 1.4.** Confirmed. **Does NOT cover (hand-roll):** the crate is **index-only** — it does NOT
provide `cgltf_node_transform_world` (`geometry.cppm:891`), which walks the node parent chain to
produce each mesh node's **world** matrix. The Rust port reconstructs the parent map and walks
local→world by hand, and preserves the first-seen material-slot ordering
(`std::map<const cgltf_material*, u32> materialSlots`, `geometry.cppm:746`) and the mesh-node
iteration order. Gap owner: PP-2's geometry phase (`02-math-and-geometry/`).

**`tobj` 4.0.** Confirmed. **Does NOT cover (hand-roll):** the deterministic dedup. The C++ keys
unique vertices with `std::map<std::array<int,3>, u32>` (`geometry.cppm:1146`) and material slots
with `std::map<int, u32>` first-seen (`geometry.cppm:1188`). `tobj` returns flat index arrays; the
`BTreeMap` first-seen dedup must be hand-ported to keep byte-deterministic `.smesh` output.

**`image` 0.25.** Confirmed for decode/encode (replaces stb_image / stb_image_write, the four
`stbi_load*`/`stbi_loadf*` paths at `geometry.cppm:1327-1392` and PNG screenshots). **Key finding —
the bit-parity worry does NOT apply here:** the asset content hash is FNV-1a over the **encoded file
bytes** (`assets.cppm:hashFileFnv`, "FNV-1a 64-bit of a file's bytes"), and the import dedup hash is
FNV over field bytes (`geometry.cppm:1294`), **not** over decoded pixels — so `image`'s pure-Rust
decode differing from stb at the pixel level cannot change any existing hash. The feasibility "use an
stb binding if texture hashes must match" caveat is therefore **moot** for this codebase; `image` is
adopted without an stb fallback. (One residual: HDR/`stbi_loadf` float decode parity for IBL is a
visual-golden concern, owned by PP-5, not a hash concern.)

**`resvg` 0.44 (+ `usvg`, `tiny-skia`).** Confirmed. Replaces nanosvg/nanosvgrast (icon
rasterization to GPU textures; the `#include <nanosvg.h>`/`<nanosvgrast.h>` in
`renderer.cppm:13-14`). resvg is strictly more complete than nanosvg. **Does NOT cover:** it
rasterizes icons **slightly differently** than nanosvg (different rendering of strokes/gradients) —
acceptable since icons have no byte-exact contract, but a golden-image note for PP-5.

## 12. Slang `slangc` — **keep the binary, no crate**

**Verdict.** Cargo compiles no shaders, and the `shader-slang` (0.1.0) / `shaderc` crates are too
immature. Keep the pinned `slangc` binary (from the toolbox) and drive it from `build.rs`/`xtask`.
**Does NOT cover (hand-roll):** the entire `CompileShaders.cmake` fan-out — ~40 `*.slang → SPIR-V`
invocations **plus** the `lighting.slang` module-precompile trick — re-expressed in xtask with its
own staleness tracking (Cargo has no shader dependency graph). Gap owner: PP-12 (`01-build-and-toolchain/`).

## 13. Syscalls & CLI — `rustix`, `clap`

**`rustix` 0.38.** Confirmed over `nix` (more `#![no_std]`-friendly, actively maintained, same
syscall surface). The control server is a synchronous, single-threaded, drain-once-per-frame,
newline-framed AF_UNIX server — **no tokio**. The exact surface is grounded in
`control_server.cpp`: `::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)` (`:175`),
`bind`/`listen(fd, 8)` (`:190`,`:197`), `recv(..., MSG_DONTWAIT)` (`:268`),
`send(..., MSG_NOSIGNAL)` with short-write retry (`:308`), `poll` (`:316`). rustix exposes every one.
**Does NOT cover (hand-roll):** the newline-framing / 5s reply budget / drain-once-per-frame loop is
application logic over the syscalls, not a rustix feature (PP-6, `09-control-plane/`).
The shm seqlock ring (`shm_open`/`ftruncate`/`mmap MAP_SHARED`, the 32-byte header, the
`std::atomic_thread_fence(release)` seq bump in `renderer_capture.cpp:160-302`) is also rustix
(`shm_open`/`mmap`) + `std::sync::atomic::fence` — designed by PP-10 (`08-host-and-viewport/`), the
shm publisher being the third `#![allow(unsafe_code)]` seam.

**`clap` 4 (`derive`).** Confirmed for the `sa` CLI (replaces the hand-rolled `tools/sa/args.hxx`).
The derive command tree mirrors the control commands and links **only** `saffron-protocol` (engine-
dependency-free, host-runnable). Gap owner: PP-9 (`11-sa-cli/`).

## 14. `bytemuck` 1 (`derive`) — GPU struct casts

**Verdict.** Confirmed. The GPU upload path casts `#[repr(C)]` structs to byte slices; `bytemuck`'s
`Pod`/`Zeroable` derives give the safe cast. Every C++ `static_assert(sizeof(T) == N)` over a std430
struct (`renderer_types.cppm:1891`, and the InstanceData/MaterialParamsData/light structs) becomes a
`const _: () = assert!(size_of::<T>() == N)` next to the `#[repr(C)]` mirror. **Does NOT cover:**
bytemuck does not *verify* std430 alignment rules — getting the layout right (the `Vec3` vs `Vec3A`
pin from §7, explicit padding fields) is the author's job, asserted by the size checks. Gap owner: PP-5.

## 15. `base64` 0.22 — control-protocol blobs

**Verdict.** Confirmed (PP-1 left the pin-vs-hand-roll as a PP-2 call; pin the crate). The C++
`core::base64Encode` (`core.cppm:90`) is **standard RFC 4648** with the `ABC…+/` alphabet and `=`
padding (`core.cppm:92`) — `base64`'s `STANDARD` engine is byte-identical, so the encode output
matches the existing wire (thumbnail PNGs over JSON). saffron-core re-exports a thin `base64_encode`.
**Does NOT cover / note:** the C++ surface is **encode-only** (no decode); add a `base64_decode` only
if a reader needs it (carried open from PP-1) — the `base64` crate provides both either way, so no
hand-roll regardless.

---

## 16. Version-churn risk register

| Risk | Trigger | Mitigation |
|---|---|---|
| `ash` 0.38→0.39 (Vulkan 1.4 in-progress breaking bump) | a mid-port `cargo update` or a 1.4 symbol only on 0.39 | pin **exactly** `=0.38`; `vk-mem` 0.4 is also pinned to ash 0.38, so they move together — re-evaluate the pair only when `vk-mem` ships an ash-0.39 build |
| `vk-mem` bus-factor (single-maintainer fork) | upstream stalls / ash moves past it | the pin couples to ash 0.38 above; if it goes unmaintained, the fallback is `gpu-allocator` (re-validating budget/defrag per §3) — recorded, not chosen |
| `mlua` `luau` recency | a Luau-feature gap vs stock Lua 5.5 | PP-8 confirms Luau parity before the VM phase is declared done; mlua's luau is stable as of the pin |
| `gltf`/`tobj` determinism drift | a crate update changes iteration order | the hand-rolled parent-walk + first-seen dedup (§11) own ordering, not the crate — `.smesh` golden tests (PP-13) catch any drift |
| Jolt 5.3.0 vendored vs `cxx` ABI | a `cxx` bump changes the bridge codegen | pin `cxx =1.0.x`; the determinism gate (PP-11) is the blocking re-check |
| `resvg` icon raster differs from nanosvg | any resvg update | icons have no byte contract; visual-golden note only (PP-5) |

`Cargo.lock` is committed; the toolbox is the build environment but every crate above builds from
crates.io (or vendored sources for `vk-mem`/Jolt), so there is no host-toolchain requirement beyond
the `slangc` binary and the C++ compiler the two FFI crates' build scripts invoke.
