# Phase 3 — Keep the bun e2e suite as the cross-engine parity harness

**Status:** IN PROGRESS — BLOCKED on a Rust renderer crash (see "Blocker" below)

## Blocker — the Rust host crashes in swapchain creation under llvmpipe headless

The harness is verified engine-agnostic and the C++ parity baseline is green, but the Rust
half of the acceptance gate cannot pass: the Rust host segfaults during boot, before the
control socket appears, so every e2e test fails at `Engine.boot`.

- **Symptom:** `target/debug/saffron-host` exits with SIGSEGV (139) on startup. `harness.ts`
  reports `engine exited before the control socket appeared`. The last log lines are the
  rendering bring-up (`software rasterizer detected`, `ray tracing available`); the crash is in
  the very next step.
- **Crash site (coredump backtrace):** NULL function-pointer jump inside `libvulkan_lvp.so`
  (Mesa llvmpipe WSI), reached via
  `saffron_app::run` → `bring_up` → `saffron_rendering::renderer::Renderer::new` →
  `saffron_rendering::swapchain::Swapchain::new` → `ash …khr::swapchain::create_swapchain`.
  `Renderer::new` (`crates/rendering/src/renderer.rs:456`) unconditionally builds a
  `Swapchain` (`swapchain` is a non-`Option` field), so even the headless editor-viewport host
  (`SAFFRON_EDITOR_NATIVE_VIEWPORT=1`) builds a real `VK_SWAPCHAIN` on the
  `VK_EXT_headless_surface` — and lavapipe's headless WSI crashes on that `vkCreateSwapchainKHR`.
  The codebase already knows this: `crates/rendering/src/render_settings.rs:167` notes "a
  headless `Renderer::new` crashes lavapipe's WSI" and the rendering unit tests deliberately
  avoid the full-renderer path for that reason.
- **Not environmental — the C++ engine boots and serves under the identical environment.** With
  the same headless `weston` + the same llvmpipe ICD (the only Vulkan device this toolbox
  enumerates), the C++ `build/debug/bin/SaffronAnima`, driven through the *unchanged* `harness.ts`,
  boots, creates the control socket, and answers `ping`/`help` (154 commands)/`create-entity`/
  `list-entities`/`inspect` with `validationErrors()` empty. The real e2e files
  `control.test.ts` + `scene.test.ts` (+ the `animation-control.test.ts` bun's glob also picks up)
  pass 18/18 against the C++ binary. The C++ `buildSwapchain` (`engine-old/.../renderer_detail.cppm:109`)
  uses vk-bootstrap's `SwapchainBuilder` and survives lavapipe's headless WSI where the Rust
  hand-rolled `Swapchain::new` does not.
- **This belongs to `06-rendering`, not here.** The fix is in the renderer's swapchain creation
  (make it survive — or skip — on the `VK_EXT_headless_surface` the editor host uses, matching the
  C++ path), not in the e2e harness, which is correct and unchanged. This phase's harness glue is a
  no-op: `harness.ts` already boots whatever `SAFFRON_ANIMA_BIN` points at; the only thing missing
  is a Rust host that boots/serves.

### What is verified now

- `harness.ts` boots a binary unchanged via `SAFFRON_ANIMA_BIN` and reads the
  `[saffron:vulkan] error: [validation]` log form — confirmed against the C++ binary.
- The Rust validation-error log form matches the harness filter:
  `crates/rendering/src/device.rs:610` emits `[validation] …` through `saffron-core`'s `log`, which
  prints `[saffron:vulkan] error: [validation] …` at error severity (`crates/core/src/log.rs:40`).
- The Rust Cargo workspace compiles green (`cargo build --workspace`).
- The slice-enable ladder is documented below (the binding map of which e2e file turns on in which
  feature phase's gate); no e2e file is deleted or duplicated.

The smoke slice flips from blocked to green for the Rust binary once `06-rendering` lands a host
boot that creates the control socket under the headless surface.

---


**Depends on:** 09-control-plane:phase-1-socket-server-and-dispatch, 13-testing-and-verification:phase-1-test-conventions-and-coverage-map

## Goal

Keep the existing `tests/e2e` bun suite exactly where it is and re-purpose it as the cross-engine parity
harness: the same 81 `*.test.ts` files, the same `harness.ts`, driving either the C++ binary or the Rust
binary through `SAFFRON_ANIMA_BIN`. Because the suite speaks only the frozen JSON-over-unix-socket wire
(never C++), it is already engine-language-agnostic — it is the single best proof that the Rust engine
behaves identically to the C++ engine over the wire the editor actually uses.

This phase does **not** rewrite the suite. It establishes the suite as the Rust engine's wire-behavior
oracle and lands the minimal glue so it boots the Rust binary, then turns on the slices as the matching
Rust subsystems land.

Concretely:

- **Confirm `harness.ts` boots the Rust binary unchanged.** `Engine.boot` already honors
  `SAFFRON_ANIMA_BIN` (`harness.ts:17`) and reads stdout/stderr into `.log` for `validationErrors()`
  (`:54`). The only requirement on the Rust engine is that it (a) creates the control socket at
  `SAFFRON_CONTROL_SOCK`, (b) answers the same envelope shape, and (c) prints validation errors in the
  exact `[saffron:vulkan] error: [validation]` form the filter matches (`:57`) — all already required
  by `08-host-and-viewport` and `06-rendering`. This phase asserts those contracts are met by running a
  smoke slice.
- **The slice-enable ladder.** The 81 files map onto the area build order; a slice flips from
  skipped-against-Rust to required as its subsystem lands. The map: control/scene/camera/picking/play/
  toggles/hierarchy after `09`; animation/foot-ik after `04`+`09`; skinning/skinned-*/skeleton-overlay
  after `06`+`04`; physics-* after `05`+`09`; material*/asset*/thumbnail* after `07`; the `*_render`
  pixel tests after the relevant render phase. Each feature phase's gate names the e2e files it turns
  on (the coverage map, phase 1).
- **No `@saffron/protocol` change.** The suite imports the generated protocol types
  (`engine.call<RenderStats>(...)`); those are regenerated from the Rust DTOs by `10-protocol-codegen`,
  so the types stay valid with zero suite edits — a schema change that breaks an assertion shows up at
  `tsc`, exactly as today.
- **The `make e2e` target** is carried into the justfile (`01-build-and-toolchain` phase 5) verbatim,
  with `SAFFRON_ANIMA_BIN` defaulting to the Rust binary once cutover-ready and overridable to the C++
  binary for parity runs (phase 7).

## Why this shape (NO LEGACY)

- **The suite is the frozen-wire contract made executable; rewriting it would discard validated
  coverage.** The feasibility study names "the headless e2e suite" as one of the three "first-class,
  continuously-green deliverables … the only automated detectors for the entire silent-failure class."
  It is already language-agnostic by construction (TypeScript driving JSON), so the correct move is
  *keep it*, not port it to Rust. A Rust mirror exists too (phase 4) but does not replace it — they
  test the same wire from two sides, and the bun suite is the one that also proves the *editor's* client
  path works (it uses the same `@saffron/protocol`).
- **It stays in `tests/e2e/` (not relocated).** Moving it would break the editor's and the justfile's
  paths and the `tools/ci/check.sh` reference for no benefit — the directory is engine-agnostic already.
  Relocation is rejected; the only change is the binary it points at.
- **Slices enable, never fork.** A subsystem not yet ported leaves its slice skipped against the Rust
  binary (it still runs against C++ for parity); it is not deleted, copied, or stubbed. When the
  subsystem lands, the slice flips to required for the Rust binary in that phase's gate. One suite, two
  binaries, no second copy.

## Grounding (real files/symbols)

- `tests/e2e/harness.ts` — `Engine.boot` (`:60`) reading `SAFFRON_ANIMA_BIN` (`:17`) and
  `SAFFRON_CONTROL_SOCK` (`:79`); `call` (`:97`) and the envelope `{ok, result, error}` shape;
  `validationErrors` (`:54`) and its `[saffron:vulkan] error: [validation]` filter (`:57`);
  `importEntity`/`rig`/`getThumbnail`/`shutdown` helpers.
- `tests/e2e/AGENTS.md` — the two-tier model (behavioral vs pixel), the `validationErrors()` assertion
  discipline, and the `@saffron/protocol` typing convention.
- `tests/e2e/*.test.ts` — the 81-file suite the slice ladder enables; `imggen.ts` and the `*_render`
  files (e.g. `material_codegen_render.test.ts`) for the pixel tier; `perf.test.ts` for the telemetry
  smoke.
- `tools/ci/check.sh` — the `make e2e`-adjacent invocation and the `SAFFRON_ANIMA_BIN` default.

## Acceptance gate

- The bun e2e suite boots the **Rust** binary via `SAFFRON_ANIMA_BIN` and a smoke slice passes:
  `control.test.ts` (ping/help/quit) + `scene.test.ts` (create/inspect/list) green against the Rust
  engine, with `validationErrors()` empty.
- The same smoke slice still passes against the C++ binary (parity baseline intact) — the suite is not
  C++-specific.
- The slice-enable ladder is documented as the binding map (which file turns on in which feature
  phase's gate), with no e2e file deleted or duplicated.
- `bun test` in `tests/e2e` runs under headless weston in the toolbox; the Cargo workspace compiles and
  `cargo test --workspace` stays green (this phase adds no Rust unit code, only the wire contract the
  engine must meet).
