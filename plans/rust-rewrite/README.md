# Saffron Anima — the Rust rewrite (master plan)

**Status:** IN PROGRESS — phases 1-40 complete (Bands A foundations+build, B math/geometry/leaf
formats, C ECS+scene through the ECS go/no-go gate #20, D animation player runtime, E physics through the
whole Jolt bridge incl. the BLOCKING determinism gate #33, and the first rendering bring-up #39-40). The
ECS gate is **locked**: `hecs` chosen on a genuine measured entt 3.16 side-by-side (per-frame iteration
passes the ~10%-of-entt bar — 7.7× faster on the hot draw walk — and the `PoseOverrideComponent` churn
worst case is 2.7% of a 60 FPS frame, so no `bevy_ecs` escalation; verdict recorded in
`03-ecs-and-scene/phase-2-ecs-benchmark-gate.md`). The **physics determinism gate (#33) is PASSED on
x86_64**: the vendored-Jolt-5.3.0 + `cxx` bridge built with the determinism flags reproduces a frozen
stack+ragdoll+`CharacterVirtual` trace bit-exactly against the committed golden hash
(`crates/physics/tests/determinism.rs`); the cross-arch `Rust-x86 == Rust-aarch64` half is
DEFERRED-NEEDS-HARDWARE (the toolbox is x86_64-only — it must run on the self-hosted aarch64 runner against
the same golden hash). All ten physics phase files (1-10) are COMPLETED. The C++ build repoint (phase 6 /
`01-build-and-toolchain/phase-1`) is verified: every repointed reference resolves to `engine-old/` and the
C++ tree stays green. Workspace gate green at #40: `cargo build --workspace`, `cargo test --workspace`
(245 tests, 0 failed), `cargo clippy --workspace --all-targets -- -D warnings`, `cargo fmt --check`. The
remaining rendering feature phases (#41+) and Bands G-L remain NOT STARTED.

This is the master index for the from-scratch Rust rewrite of the Saffron Anima engine. It is the
output of the pre-planning effort ([`../rust-rewrite-pre-planning.md`](../rust-rewrite-pre-planning.md),
grounded in [`../rust-rewrite-feasibility.md`](../rust-rewrite-feasibility.md)): fourteen area folders,
each with a locked design `README.md` and dependency-ordered `phase-N-*.md` files, and **one master
ordered index** that linearizes every phase across all areas into a single, dependency-correct,
top-to-bottom execution sequence. An implementer walks the index one phase at a time; each phase leaves
the Cargo workspace compiling and the tests-so-far green.

The verdict the project accepted: a faithful idiomatic-Rust rewrite is feasible, neither hard constraint
(ECS iteration speed, cross-platform-deterministic physics) is a blocker, and the payoff is **toolchain
simplicity + memory safety + a unified Rust/Tauri stack**, not new capability.

## Locked ground rules (inherited, not re-litigated)

From pre-plan §0 — every phase obeys these:

- **Purely idiomatic Rust.** The Go-flavored `CONVENTIONS.md` is retired wholesale; the new house style
  is [`00-foundations/conventions.md`](./00-foundations/conventions.md). Typed `thiserror` errors + `?`,
  `Arc` (and `Arc<Mutex>` only at proven shared-mutable sites), data-carrying `enum`s, trait objects,
  `Drop`, iterators.
- **`unsafe` only at three seams** — `ash` (rendering), the Jolt `cxx` bridge (`saffron-physics-sys`),
  the shm ring (`saffron-host`). Everywhere else `#![deny(unsafe_code)]`.
- **NO LEGACY / one code path.** No compat shims; break-and-rebuild in one move; delete the superseded
  path with its callers. The C++ `engine-old/` is reference-only and **deleted at cutover**.
- **The wire is frozen, the editor is not rewritten.** The Rust engine byte-matches the
  JSON-over-unix-socket control envelope and the POSIX-shm BGRA8 frame ring, so the Tauri/React editor
  (frontend **and** the already-Rust `editor/src-tauri/`) runs unchanged. The only editor-adjacent change
  is one file: the protocol generator re-points at the Rust DTOs.
- **Two processes kept** (engine binary + editor), glued by the shm ring and the control socket — not
  collapsed into Tauri.
- **Cargo replaces CMake + C++26-modules + `import std` + BMI-matching + the two-ninja-`.pcm` race.** The
  toolbox stays the reproducibility boundary (Vulkan SDK, `slangc`, SDL3/winit substrate, headless
  weston, no GitHub-hosted CI).
- **No in-engine self-test functions.** Every `run*SelfTest` becomes real Rust `#[test]` / wire-driven
  e2e (the C++ self-test is the *oracle* ported into a `#[test]`, never a runtime function).
- **Scripting is typed Luau via `mlua`**, and the `sa.*` Luau type surface is **generated** from the Rust
  binding source — the hand-written `library/sa.lua` overlay and its drift tripwire are deleted.
- **Greenfield parallel binary.** The Rust engine is built in its own crates alongside the still-shipping
  C++ `SaffronAnima`; nothing flips until the final cutover. So at the end of *every* phase the workspace
  compiles and the test gate for everything built so far passes.

## The crate graph

The C++26 `Saffron.<Area>` named-module DAG re-encodes as a Cargo workspace under `engine/crates/`
(`saffron-<area>` crate name, `engine/crates/<area>/` directory, `saffron_<area>` lib root). One crate
per logical module by default; deviations are documented in
[`00-foundations/README.md`](./00-foundations/README.md) §2.1. Cargo enforces the DAG (a crate cannot
`use` a crate not in its `[dependencies]`), gives per-crate incremental builds, and confines `unsafe` to
the FFI-seam crates.

```
saffron-core      (leaf)   Result/Uuid/Ref policy, log, base64, TimeSpan
saffron-signal             → core                  SubscriberList event primitive
saffron-json               → core                  serde_json gateway + WireUuid + lenient readers
saffron-window             → core, signal          winit + raw-window-handle thin wrapper
saffron-geometry           → core                  glam, .smesh/.sanim/.smodel formats, gltf/obj/image import
saffron-protocol  (lib)    → core                  the 236-DTO crate (serde/schemars/ts-rs); shared by engine + sa + xtask
saffron-scene              → core, json, (ecs)     hecs/bevy_ecs world, 24 components, JSON project serde
saffron-sceneedit          → core, signal, scene, json   selection/gizmo/play/fly-cam context
saffron-animation          → core, geometry, scene pose/clip/sampler, player runtime, two-bone IK
saffron-physics-sys (ffi)  → cc/cxx + vendored Jolt 5.3.0   the JoltC shim + determinism build (unsafe seam)
saffron-physics            → core, geometry, scene, animation, physics-sys   safe Jolt wrapper
saffron-rendering          → core, window, geometry   ash, allocator, render graph, all passes (one crate, many mods; unsafe seam)
saffron-assets             → core, json, geometry, rendering, scene   catalog, .smat, codegen, thumbnails, render_scene
saffron-script             → core, scene           mlua/Luau VM + typed bindings + generated defs
saffron-control            → core, json, window, rendering, scene, sceneedit, assets, physics, protocol
saffron-app                → core, window, rendering   run-loop + Layer scaffolding
saffron-host  (bin)        → every subsystem above   the apex SaffronAnima-replacement binary (unsafe seam: shm)
sa            (bin)        → protocol only           the native sa control CLI (no engine dep, host-runnable)
xtask         (bin)        → protocol, schemars, ts-rs   codegen + slangc driver (workspace tooling)
```

## How the order was derived (and what it guarantees)

The area folders are **navigation, not execution order**. The thing you execute is the master ordered
index below: a single dependency-correct linearization of all 80 phase files. It was produced by
collecting every phase's `**Depends on:**` line (intra-area + cross-area), resolving whole-area
references to the concrete phase that satisfies the need, building the dependency DAG, and
topologically sorting it. The sort was machine-validated: **no cycle, and every phase's dependencies
precede it** — so each phase's acceptance gate is satisfiable from its predecessors alone, which is what
makes "implement one after the other, always green" true rather than assumed.

The order is **leaf-up with the walking skeleton early**, shaped by three forces (pre-plan §2):

- **Foundations + build first** (`00`+`01`): nothing compiles until the workspace + the
  `slangc`/`cxx`/profile build scripts exist.
- **A walking-skeleton milestone right after device + scene bring-up**: the engine boots headless,
  publishes a blank shm frame the *real* editor displays, and answers a control `ping` (index #62/#77 —
  `08:phase-3` shm-ABI gate + `09:phase-1` minimal socket server). Every later phase fleshes out this
  runnable spine.
- **Testing woven in, not trailing.** The test *harness + strategy* phases (`13:phase-1/2/3/8`) depend
  only on foundations (or on the minimal socket server) and are pulled forward into the foundations /
  walking-skeleton bands; each feature phase then carries its own unit/e2e tests in its acceptance gate.
  `13`'s high folder number is topical, not temporal — see the band note in the index.

The three go/no-go gates from the feasibility spike sit early, each before the bulk of work that depends
on it: ECS speed (`03:phase-2`, index #20), physics determinism (`05:phase-5`, index #33, BLOCKING), and
renderer/shm bring-up (`08:phase-3`, index #62). Cutover (`14`) is last.

## Master ordered index

Every phase, in dependency-correct execution order. `Area / phase-file` is the path under
`plans/rust-rewrite/`; **Dep** lists the load-bearing predecessors (intra-area unless an area name is
shown); the acceptance gate of each phase is in its own file (every gate = workspace compiles + named
tests pass + the feature-specific check). **GATE** marks the three go/no-go gates.

### Band A — Foundations + build (the block that makes anything compile)

| # | Area / phase-file | What | Dep |
|---|---|---|---|
| 1 | `00-foundations/phase-1-workspace-scaffold` | workspace `Cargo.toml` + empty member crates compile | — |
| 2 | `00-foundations/phase-2-core-crate` | `saffron-core`: Result/Uuid/Ref policy/log/base64 | 1 |
| 3 | `00-foundations/phase-3-signal-crate` | `saffron-signal`: `SubscriberList` hand-roll | 2 |
| 4 | `00-foundations/phase-4-json-crate` | `saffron-json`: serde gateway + WireUuid + lenient readers | 2 |
| 5 | `00-foundations/phase-5-window-crate` | `saffron-window`: winit + raw-window-handle thin wrapper, typed signals | 2, 3 |
| 6 | `01-build-and-toolchain/phase-1-relocation-repoint` | repoint C++ build refs to `engine-old/` (keep C++ green) | — |
| 7 | `01-build-and-toolchain/phase-2-profiles-and-workspace-build` | `[profile.*]` + `cargo build --workspace` | 1, 6 |
| 8 | `01-build-and-toolchain/phase-3-xtask-shader-pipeline` | `xtask` slangc fan-out + lighting-module + asset copy | 7 |
| 9 | `01-build-and-toolchain/phase-4-physics-sys-build-driver` | `saffron-physics-sys` `build.rs` determinism skeleton | 7 |
| 10 | `01-build-and-toolchain/phase-5-justfile-and-toolbox` | `justfile` carrying Makefile/toolbox lore | 7, 8 |
| 11 | `01-build-and-toolchain/phase-6-reproducible-gate` | `tools/ci/check.sh` rewrite over Cargo + `xtask` | 8, 10 |
| — | `13-testing-and-verification/phase-1-test-conventions-and-coverage-map` | test conventions + per-area coverage map (pull forward; dep `00:1`) | 1 |
| — | `13-testing-and-verification/phase-8-self-test-removal-ledger` | audit: no `run*SelfTest`/`SAFFRON_SELFTEST` survives (dep `13:1`) | 13:1 |

### Band B — Math, geometry, the leaf formats

| # | Area / phase-file | What | Dep |
|---|---|---|---|
| 12 | `02-math-and-geometry/phase-1-crate-scaffold-glam-and-cpu-types` | glam + CPU mesh/skin/ray types | `00` |
| 13 | `02-math-and-geometry/phase-2-picking-math-and-normals` | ray-triangle / AABB slab / generate-normals | 12 |
| 14 | `02-math-and-geometry/phase-3-smesh-byte-format` | `.smesh` (v1+v2) repr(C)+bytemuck+size-asserts | 12 |
| 15 | `02-math-and-geometry/phase-4-sanim-byte-format` | `.sanim` + anim track/clip types | 12, 14 |
| 16 | `02-math-and-geometry/phase-5-gltf-import` | glTF world-transform walk / node order / skin gate | 13, 14, 15 |
| 17 | `02-math-and-geometry/phase-6-obj-import-image-decode-subid` | OBJ `BTreeMap` dedup + image decode + `subIdFor` | 13, 16 |
| 18 | `02-math-and-geometry/phase-7-smodel-container` | `.smodel` SMDL writer/reader/lazy chunks | 14, 15 |
| — | `13-testing-and-verification/phase-2-golden-snapshot-infrastructure` | golden/snapshot infra for the byte-exact formats (dep `13:1`) | 13:1 |

### Band C — ECS + scene core (through the ECS gate)

| # | Area / phase-file | What | Dep |
|---|---|---|---|
| 19 | `03-ecs-and-scene/phase-1-scene-crate-skeleton-and-ecs-adapter` | `saffron-scene` + wrapped `World`/`Entity` | `00:2`, `00:4` |
| 20 | `03-ecs-and-scene/phase-2-ecs-benchmark-gate` | **GATE:** ECS speed vs entt; lock `hecs`/escalate `bevy_ecs` | 19 |
| 21 | `03-ecs-and-scene/phase-3-component-structs-and-glam` | 24 component structs + env/atmosphere/catalog types | 19, `02:1` |
| 22 | `03-ecs-and-scene/phase-4-hierarchy-and-transform-math` | relink / world-transforms / joint-matrices / ZYX euler | 21 |
| 23 | `03-ecs-and-scene/phase-5-component-registry` | fn-pointer `ComponentTraits` + `register_component` | 22 |

### Band D — Animation (pure CPU; consumes geometry + scene)

| # | Area / phase-file | What | Dep |
|---|---|---|---|
| 24 | `04-animation/phase-1-crate-sampling-pose-algebra` | `sample_track`/`sample_clip` + pose algebra | `00:2`, `02:4`, `03:3` |
| 25 | `04-animation/phase-2-two-bone-ik` | `solve_two_bone_ik` (the delicate solver) | 24 |
| 26 | `04-animation/phase-3-ik-sampling-test-oracle` | ported `runAnimationSelfTest` as `#[test]` | 25 |
| 27 | `04-animation/phase-4-player-runtime` | `tick_animation`: transitions / loop-blend / foot-IK | 24, 25 |
| 28 | `04-animation/phase-5-runtime-tests-and-skinning-seam` | runtime tests + skinning-prepass seam contract | 27 (`06` seam doc only) |

### Band E — Physics (the Jolt FFI bridge, through the BLOCKING determinism gate)

| # | Area / phase-file | What | Dep |
|---|---|---|---|
| 29 | `05-physics-jolt-bridge/phase-1-sys-crate-and-jolt-build` | vendor Jolt 5.3.0 + determinism `build.rs` | `00:1`, `01:4` |
| 30 | `05-physics-jolt-bridge/phase-2-cxx-bridge-and-filter-shims` | `cxx` bridge + 4 C++ shim classes | 29 |
| 31 | `05-physics-jolt-bridge/phase-3-world-and-rigidbody-core` | `World` + body creation + step loop | 30, `03:4`, `02:1` |
| 32 | `05-physics-jolt-bridge/phase-4-character-and-bare-ragdoll` | `CharacterVirtual` + passive SwingTwist ragdoll | 31, `04:1` |
| 33 | `05-physics-jolt-bridge/phase-5-determinism-gate` | **GATE (BLOCKING):** x86/ARM bit-exact trace diff | 32 |
| 34 | `05-physics-jolt-bridge/phase-6-shapes-and-autofit` | 5 shapes + mesh-cook seam + auto-fit | 33, `02:1` |
| 35 | `05-physics-jolt-bridge/phase-7-sensors-and-contact-ring` | sensors/triggers + seq-stamped contact ring | 34 |
| 36 | `05-physics-jolt-bridge/phase-8-kinematic-bones` | kinematic bone bodies follow the animated pose | 35, `04:4` |
| 37 | `05-physics-jolt-bridge/phase-9-ragdoll-blend-and-motors` | active/partial ragdoll motor drive + blend | 36, `04:4` |
| 38 | `05-physics-jolt-bridge/phase-10-queries` | raycast/sphereCast + `sa.raycast` POD seam | 37 |

> Physics (Band E) has no edge into rendering and can interleave with Bands D/F on a parallel track; it
> is placed here because the BLOCKING determinism gate (#33) should clear before the heavy renderer
> investment. An implementer with one worker proceeds straight down; the only hard rule is the edges.

### Band F — Rendering (ash bring-up → render graph → every feature)

| # | Area / phase-file | What | Dep |
|---|---|---|---|
| 39 | `06-rendering/phase-1-device-swapchain-bringup` | device/allocator/swapchain + validation-clean clear+present | `00:1`, `00:2`, `00`-window |
| 40 | `06-rendering/phase-2-render-graph` | `RgUsage`→barrier engine (unit-tested) | 39 |
| 41 | `06-rendering/phase-3-gpu-resources` | Drop resource wrappers + `Device` sub-state + teardown order | 39 |
| 42 | `06-rendering/phase-4-bindless-and-samplers` | bindless descriptor table + slot alloc/reclaim | 41 |
| 43 | `06-rendering/phase-5-pso-cache-and-upload` | mesh/texture upload + übershader PSO cache | 42, `02:3` |
| 44 | `06-rendering/phase-6-instancing-and-scene-pass` | draw-list batching + scene/depth passes | 43, 40 |
| 45 | `06-rendering/phase-7-lighting-and-shadows` | clustered cull + directional/spot/point shadows | 44 |
| 46 | `06-rendering/phase-8-ibl-sky-probes` | IBL bake + sky + reflection probes | 45 |
| 47 | `06-rendering/phase-9-screen-space-gi` | G-buffer / GTAO / contact / SSGI | 46 |
| 48 | `06-rendering/phase-10-aa-and-temporal` | motion vectors + TAA + FXAA + MSAA | 47 |
| 49 | `06-rendering/phase-11-tonemap-grid-overlay` | tonemap + grid + editor overlay | 48 |
| 50 | `06-rendering/phase-12-skinning-prepass` | compute skinning prepass + skinned-BLAS | 44, 48 |
| 51 | `06-rendering/phase-13-ray-tracing` | RT TLAS build + ray-query shadows | 50 |
| 52 | `06-rendering/phase-14-ddgi` | voxel-traced dynamic diffuse GI | 46 |
| 53 | `06-rendering/phase-15-restir` | ReSTIR DI many-light | 51, 47 |
| 54 | `06-rendering/phase-16-capture-shm-profiler` | capture + shm publish interface + thumbnails + profiler | 49 |

### Band G — Walking skeleton (host boots, publishes a blank frame the real editor shows, answers `ping`)

| # | Area / phase-file | What | Dep |
|---|---|---|---|
| 55 | `07-assets-and-materials/phase-1-crate-skeleton-and-asset-server` | `AssetServer` + negative-cache + Drop ordering | `00:2`, `00:4`, `03:3`, `06:3` |
| 56 | `07-assets-and-materials/phase-3-container-metadata-and-model-open` | `ContainerMetadata` + `ModelAsset` open | 55, `02:7` |
| 57 | `07-assets-and-materials/phase-4-resolve-and-load-paths` | cache resolve/load over codecs + upload | 56, `02:4`, `06:5` |
| 58 | `07-assets-and-materials/phase-8-import-bake-and-scan` | bake/import/reimport + scan/load catalog | 56, `02:5`, `02:6`, `02:7` |
| 59 | `07-assets-and-materials/phase-9-spawn-and-instantiate` | instantiate/spawn over the scene ECS | 58, `03`, `04:1` |
| 60 | `08-host-and-viewport/phase-1-app-crate-run-loop-and-layer` | `saffron-app`: run loop + `Layer` trait | `00:1`, `00:2`, `00`-window, `06:1` |
| 61 | `08-host-and-viewport/phase-2-shm-seqlock-publisher` | shm mmap/seqlock/fence publisher (frozen ABI) | 60, `06:3` |
| 62 | `08-host-and-viewport/phase-3-shm-abi-gate` | **GATE:** frames shown live in unchanged `wayland_viewport.rs` | 61, `06:16` |
| 63 | `10-protocol-codegen/phase-1-dto-crate-and-derives` | `saffron-protocol`: 236 DTOs + `Uuid` PickFirst newtype | `00:2` |
| — | `13-testing-and-verification/phase-3-bun-e2e-as-parity-harness` | existing bun e2e as cross-engine parity harness (dep `09:1`, `13:1`) | 77, 13:1 |

### Band H — Scene serde + sceneedit + materials + render_scene

| # | Area / phase-file | What | Dep |
|---|---|---|---|
| 64 | `03-ecs-and-scene/phase-6-component-serde-bytecompat` | byte-compatible component JSON serde | 23, `10:1` |
| 65 | `03-ecs-and-scene/phase-7-scene-document-and-migrations` | `scene_to/from_json` + v1→v4 migrations | 64 |
| 66 | `03-ecs-and-scene/phase-8-sceneedit-crate-and-context` | `saffron-sceneedit` + `SceneEditContext` + version stamps | 65, `00:3` |
| 67 | `03-ecs-and-scene/phase-9-fly-camera` | editor fly-camera math + serde | 66 |
| 68 | `03-ecs-and-scene/phase-10-play-mode` | play state machine + JSON-roundtrip duplicate + `simTick` seam | 67 |
| 69 | `03-ecs-and-scene/phase-11-gizmo-math-and-smoothing` | projection/hit-test/drag + `tau=0.025` smoothing | 68 |
| 70 | `07-assets-and-materials/phase-2-material-asset-and-serde` | `MaterialAsset` + `.smat` serde + instances | 55, `10:1` |
| 71 | `07-assets-and-materials/phase-5-node-graph-folding` | `lower_graph_to_params` + `emit_graph_surface` | 70 |
| 72 | `07-assets-and-materials/phase-6-slang-codegen-via-command` | `slangc` via `std::process::Command` (shell-string deletion) | 71 |
| 73 | `07-assets-and-materials/phase-7-render-ready-materials` | build/resolve submesh materials + precedence | 57, 70, `03:3` |
| 74 | `07-assets-and-materials/phase-11-thumbnail-worker` | worker thread + `Arc<Mutex<WorkerState>>` + drain | 57, 73, `06:16` |
| 75 | `07-assets-and-materials/phase-10-project-io` | save/load/create project + idle-before-clear | 58, `03:7`, `06`, 74 |
| 76 | `07-assets-and-materials/phase-12-render-scene-and-pick` | `render_scene` (highest-coupling driver) + `pick` | 73, `06:6`, `03:4`, `04:4` |

### Band I — Control plane + host wiring (control ports last over live subsystems)

| # | Area / phase-file | What | Dep |
|---|---|---|---|
| 77 | `09-control-plane/phase-1-socket-server-and-dispatch` | `rustix` socket server + framing/drain/flush + `ping`/`help` | `00:4`, `10:1`, walking-skeleton host (`08:3`) |
| 78 | `08-host-and-viewport/phase-4-host-crate-lifecycle-wiring` | `saffron-host` apex: `HostLayer` + `EngineContext` + `poll_control` | 60, 61, `03`, `04`, 77 |
| 79 | `08-host-and-viewport/phase-5-native-overlay-geometry` | ~900 LOC native gizmo overlay geometry | 78, `06:11`, 69 |
| 80 | `09-control-plane/phase-2-render-commands` | 29 render commands | 77, `06:6` |
| 81 | `09-control-plane/phase-4-asset-commands` | 52 asset/project commands | 77, 76, 78, `06:11` |
| 82 | `09-control-plane/phase-5-animation-commands` | 13 animation commands | 77, `04:4`, 78, `06:11` |
| 83 | `09-control-plane/phase-6-physics-commands` | 12 physics commands | 77, 38, 78 |
| — | `13-testing-and-verification/phase-5-validation-clean-gate` | validation-layer-clean gate (dep `06:1`, `08:4`) | 39, 78 |

### Band J — Protocol codegen complete + sa CLI

| # | Area / phase-file | What | Dep |
|---|---|---|---|
| 84 | `10-protocol-codegen/phase-2-schemars-fragments-and-special-cases` | schemars fragments + OpenRPC special-cases | 63 |
| 85 | `10-protocol-codegen/phase-3-component-registry-macro` | `register_component!` macro + completeness tripwire | 63, 23 |
| 86 | `10-protocol-codegen/phase-4-command-table` | shared `&'static [CommandSpec]` + fixture/skip tables | 63 |
| 87 | `10-protocol-codegen/phase-5-xtask-emitters-and-editor-repoint` | `xtask gen-protocol`: ts-rs/OpenRPC/manifest + editor repoint | 84, 86 |
| 88 | `10-protocol-codegen/phase-6-luau-typegen-skeleton` | shared Rust→Luau mapper + component-snapshot emitter | 85, 87 |
| 89 | `11-sa-cli/phase-1-crate-and-socket-client` | `sa` bin + clap skeleton + `UnixStream` round-trip | `00:1`, `00:2`, 63 |
| 90 | `11-sa-cli/phase-2-param-coercion` | `build_params`/`coerce` pure functions | 89 |
| 91 | `11-sa-cli/phase-3-text-formatters` | `help` table + ~35 command-keyed formatters | 89, 90 |
| 92 | `11-sa-cli/phase-4-help-completions-and-start` | COMMANDS help-enrich + completions + `start` launcher | 89, 86, `01` |
| — | `13-testing-and-verification/phase-6-control-schema-contract-gate` | decimal-string-u64 contract gate (dep `10:5`, `09:1`) | 87, 77 |
| — | `13-testing-and-verification/phase-4-rust-e2e-driver` | native Rust e2e driver (dep `13:3`, `11:1`) | 13:3, 89 |

### Band K — Scripting (Luau via mlua), the last subsystem

| # | Area / phase-file | What | Dep |
|---|---|---|---|
| 93 | `12-scripting/phase-1-vm-sandbox-budget` | `mlua` Luau VM + sandbox + budget + traceback→`Error` | `00:1`, `00:2`, `03:1` |
| 94 | `12-scripting/phase-2-value-types-and-binding-table` | `sa.Vec3` + declarative binding-descriptor table | 93, `02:1` |
| 95 | `12-scripting/phase-3-session-guard-and-entity-handle` | scoped session guard + `sa.Entity` handle | 94 |
| 96 | `12-scripting/phase-4-component-bridge` | get/set/add/remove/has_component over registry serde | 95, 23 |
| 97 | `12-scripting/phase-5-runtime-lifecycle` | start/tick/stop + class load + field inject + pause-on-error | 96 |
| 98 | `08-host-and-viewport/phase-6-teardown-drop-graph` | cross-object teardown order (the UAF surface) | 78, 31, 97 |
| 99 | `12-scripting/phase-6-scheduler-messages-input` | scheduler prelude + messages + input edges + hierarchy | 97 |
| 100 | `09-control-plane/phase-3-scene-commands` | 47 scene/script commands | 77, 23, 78, 99 |
| 101 | `12-scripting/phase-7-host-bridge-and-contacts` | `ScriptHostBridge` trait + `dispatch_contact` + `move_character` | 97 |
| 102 | `12-scripting/phase-8-schema-and-inspector-contract` | `read_script_schema` + `GetScriptSchemaResult` DTO | 94 |
| 103 | `12-scripting/phase-9-luau-api-typegen` | `sa.*` API `.luau` emitter; delete `library/sa.lua` + tripwire | 94, 88 |

### Band L — Verification rig + cutover

| # | Area / phase-file | What | Dep |
|---|---|---|---|
| 104 | `13-testing-and-verification/phase-7-cross-engine-parity-rig` | golden images / sim traces / serde byte-equality rig | 13:3, 13:2, 33 |
| 105 | `13-testing-and-verification/phase-9-reproducible-gate-orchestration` | the standing-gate orchestrator | 13:2, 13:3, 13:5, 13:6, 13:8, 11 |
| 106 | `14-migration-and-cutover/phase-1-parity-signoff` | qualify the Rust binary against the full gate + parity rig | 104, 105, 98, 83, 92, 103 |
| 107 | `14-migration-and-cutover/phase-2-binary-flip` | flip the `SAFFRON_ANIMA_BIN` default to the Rust host | 106 |
| 108 | `14-migration-and-cutover/phase-3-retire-cpp-tree` | delete `engine-old/`, the C++ tooling, the parity rig | 107 |

**Reading the bands.** The numbered rows are the strict linearization; the unnumbered (`—`) rows are the
woven-in testing-harness phases whose only dependency is foundations or the minimal socket server (`13`'s
folder number is topical, not temporal — pre-plan §2). They are listed in the band where their
dependency is first satisfied, and they carry their own gate; an implementer lands them as soon as their
`Dep` is green rather than waiting for the `13` folder. The strict topological validation (below)
includes them at their dependency-correct positions.

## Validation

The full DAG (all 80 phase files, with whole-area references resolved to concrete phases) was
topologically sorted and checked: **no cycle, and for every edge the dependency's index is strictly less
than the dependent's.** The one apparent cross-area cycle — `09:phase-1` depends on "the walking-skeleton
host" while `08:phase-4` depends on `09:phase-1` — is **not** a cycle: `09:phase-1` needs only the
*minimal* host (`08:phase-1`→`2`→`3`, the run loop + blank-frame publisher + the gate), not the full
lifecycle wiring (`08:phase-4`), which layers control *on top*. The index places `08:phase-1/2/3` (#60-62)
and `09:phase-1` (#77) before `08:phase-4` (#78), so the gate is satisfiable at every step. This
resolution is recorded so an implementer does not mistake the area-level reference for a hard edge into
`08:phase-4`.

## The migration strategy

The cutover is the binary-boundary flip designed in
[`14-migration-and-cutover/README.md`](./14-migration-and-cutover/README.md):

- The Rust engine is a **greenfield parallel binary** that speaks the identical frozen shm + control
  contracts. The editor (`editor/src-tauri/src/lib.rs:186`, `engine_binary()`) and the e2e harness
  (`tests/e2e/harness.ts:18`) both spawn whatever `SAFFRON_ANIMA_BIN` resolves to, defaulting to the C++
  `build/debug/bin/SaffronAnima`.
- The editor stays on C++ until the Rust `saffron-host` passes the **full gate + cross-engine parity
  rig** (phase-1 sign-off): the reproducible gate, the full `tests/e2e` suite against the Rust binary,
  the parity rig's three diffs (golden images, Jolt sim traces C++-vs-Rust, serde byte-equality), and a
  re-confirmation of the three go/no-go gates.
- Then the flip (phase-2): re-point the `SAFFRON_ANIMA_BIN` default to the Rust host — **no editor source
  change, no test source change**, one default + justfile/gate env. There is one editor child process and
  no runtime engine selector (NO LEGACY).
- Then retirement (phase-3): delete `engine-old/`, the C++ tooling (`tools/gen-control-dto`, `tools/sa`,
  `tools/check-script-defs`, `cmd/sa`), the repointed C++ build references, and the parity rig. The tree
  is Rust-only.

There is **no in-process FFI bridge** to the old engine (feasibility option B, rejected): the seams are
deep by-reference aggregates, not C-ABI boundaries, so the only clean bridge is the existing
cross-process wire — which is what the editor already uses.

## The go/no-go gates

Three gates decide whether the bulk of the work is committed to; each sits early and blocks its
dependents:

1. **Physics determinism** (`05-physics-jolt-bridge/phase-5`, index #33, **BLOCKING**). Bit-exact Jolt
   sim traces across x86 **and** ARM through the new `cxx` bridge, with `CharacterVirtual` + a
   motor-driven SwingTwist ragdoll working, *before* any gameplay ports on top. If the traces are not
   bit-identical, the lockstep-netcode premise collapses and the rewrite decision is reconsidered.
2. **ECS speed** (`03-ecs-and-scene/phase-2`, index #20). Per-frame `for_each` iteration within ~10% of
   the current entt build (and the `PoseOverrideComponent` per-frame churn path acceptable). Locks `hecs`;
   escalates to `bevy_ecs` standalone (with the `SparseSet` storage knob) only if the benchmark forces it.
3. **Renderer / shm bring-up** (`08-host-and-viewport/phase-3`, index #62, with `06-rendering/phase-1`).
   A validation-clean offscreen frame published through the byte-compatible shm ring and shown live in
   the **unchanged** `editor/src-tauri/src/wayland_viewport.rs` reader (the executable byte oracle).

At cutover, all three are re-confirmed as part of the parity sign-off (`14/phase-1`).

## The subtractions ledger

The honest scope-is-smaller-than-1:1 accounting lives in
[`00-foundations/subtractions-ledger.md`](./00-foundations/subtractions-ledger.md) (PP-3): every removed
or collapsed C++ artifact with its LOC and its Rust replacement (or "deleted, no replacement"). The
headline deletions: the entire CMake + `import std` + BMI + two-ninja-`.pcm` apparatus; `gen.ts`
(3504 LOC) + `control_dto_serde.generated.cpp` (167 KB) + `scene_component_serde.generated.cpp`
(collapse to serde derives + one registration macro); the vendored `args.hxx` (5135 LOC) + the Python
`cmd/sa` wrapper; the `library/sa.lua` overlay + its drift tripwire; the `JSON_NOEXCEPTION` abort
firewall; every `run*SelfTest`. The flip side (what Rust *forces us to add*) is also there: `Arc<Mutex>`
at the two rendering shared-mutable sites + the thumbnail worker, the script session guard, and the
explicit `Drop` ordering for GPU resources.

## Open items (unresolved critic concerns carried forward)

These are noted so an implementer treats them as live decisions, not settled facts. None blocks starting;
each has a phase that owns its resolution.

- **ECS crate is gated, not locked.** `hecs` is the default but `03-ecs-and-scene/phase-2` may escalate
  to `bevy_ecs`. If it escalates, the `PoseOverrideComponent` churn site (`emplace_or_replace`/`remove`
  every frame on every animated bone) is the reason, and the storage-knob change ripples into the scene
  wrapper — bounded to `saffron-scene` because the ECS is never leaked.
- **Vulkan allocator pick is deferred to PP-2/PP-5** (`vk-mem-rs` real-VMA vs `gpu-allocator` pure-Rust).
  The rendering phases are written against whichever lands (both expose create/destroy/map/budget at the
  call sites); `gpu-allocator` has no defrag, so if budget/telemetry/defrag parity is required, the pick
  is `vk-mem-rs`. Recorded as a phase-1 (`06-rendering`) measurement.
- **ash 0.38→0.39 (Vulkan 1.4) version churn.** Stable `ash` is 0.38 (Vk 1.3); the engine targets 1.4.
  Start on 0.38 (all used extensions exist; 1.4 mostly promotes already-used KHR/EXT) and budget ~25%
  schedule risk for the breaking 0.39 bump landing mid-port, plus possible `vk-mem-rs` lag behind it.
- **stb bit-parity for texture hashes** (`02-math-and-geometry/phase-6`). Pure-Rust `image` decode can
  differ at the bit level from stb; the asset catalog hashes decoded bytes. Start on `image`; if
  `07-assets` proves a hash must be bit-stable against stb-decoded bytes, swap that one decode path to an
  stb binding behind the same `DecodedImage` return type. Unresolved until the hash-stability requirement
  is measured.
- **The shm slot off-by-one** (`08-host-and-viewport/phase-2/3`). The C++ publisher writes the first
  frame (seq 1) into slot `1`, not slot `0` (`next = seq + 1`, then `next % ring_slots`); the reader reads
  `seq % slots`. This must be reproduced exactly or the first displayed frame is wrong — a silent torn
  frame, not a crash. The gate (#62) is the detector; flagged here because it is the single most
  error-prone byte detail in the frozen ABI.
- **Profiler `set-mode`/capture wire fidelity.** The multi-frame profiler capture is the largest single
  reply and the reason the control server's send-flush loop is load-bearing (`09-control-plane/phase-1`).
  The Rust port must port the flush loop intact; a short-write that drops the tail makes the client hang —
  not an error. Flagged as a known silent-failure surface, owned by `09/phase-1` and asserted by an e2e
  test that drives a capture.

## Area folders

| Area | Design README |
|---|---|
| `00-foundations` | workspace, house style, core/signal/json/window crates + the subtractions ledger |
| `01-build-and-toolchain` | Cargo workspace, `xtask` slangc, the Jolt FFI build, justfile, the gate |
| `02-math-and-geometry` | glam, the `.smesh`/`.sanim`/`.smodel` byte formats, gltf/obj/image import |
| `03-ecs-and-scene` | the ECS world, components, registry, JSON project serde, sceneedit/play/gizmo |
| `04-animation` | pose/clip/sampler, the player runtime, two-bone IK, the skinning seam |
| `05-physics-jolt-bridge` | the `cxx`/JoltC bridge, the determinism build, the gameplay/ragdoll layer |
| `06-rendering` | ash bring-up, the render graph, every render feature (16 phases) |
| `07-assets-and-materials` | the catalog, `.smat`, node-graph→Slang codegen, thumbnails, `render_scene` |
| `08-host-and-viewport` | the run loop, the `Layer` model, the shm publisher, the native overlay, teardown |
| `09-control-plane` | the synchronous socket server + the 153-command surface |
| `10-protocol-codegen` | the DTO crate + the `xtask` emitters → `@saffron/protocol` + the Luau skeleton |
| `11-sa-cli` | the native Rust `clap` CLI over the frozen socket |
| `12-scripting` | Luau via `mlua`, the declarative binding layer, the generated `.luau` defs, the session guard |
| `13-testing-and-verification` | unit + e2e + the four standing gates + the parity rig + the self-test-removal ledger |
| `14-migration-and-cutover` | the binary-boundary flip + parity sign-off + the C++-tree retirement |
