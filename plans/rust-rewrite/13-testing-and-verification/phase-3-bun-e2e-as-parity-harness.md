# Phase 3 — Keep the bun e2e suite as the cross-engine parity harness

**Status:** COMPLETED — the **full** 81-file bun e2e suite is GREEN against the live Rust host
(`target/debug/saffron-host`) under headless `weston`/llvmpipe: **301 pass / 0 fail**, every
`validationErrors()` empty. `cargo test --workspace` stays green (1258), `cargo clippy
--workspace --all-targets -- -D warnings` and `cargo fmt --check` clean.

## Final close-out — the last three e2e gaps closed

The play-edge runtime, set-4 binding, texture-import, and thumbnail-async were already landed by
their owning areas; three focused gaps remained, closed here:

- **Per-frame telemetry was never driven (06-rendering seam).** The renderer's
  `frame_history.record` / `alarms.tick` / capture-`tick` methods existed but the run loop only
  drove the wall-clock + CPU EMA. Added `Renderer::finalize_frame_telemetry(busy_ms, wait_ms,
  dt_sec)` (the C++ `endFrame` perf tail: CPU EMA → history ring push → alarm tick → capture
  advance over the just-rendered slot), wired it as the `FrameHost::finalize_frame_telemetry`
  hook the loop calls once per non-minimized frame (replacing `record_cpu_frame_timing`, NO
  LEGACY), and armed the CPU span recorder (`execute-render-graph` outer span + per-pass nesting,
  gated on `profiler.mode != Off`) so a capture carries both lanes. Closed `profiler.test.ts`,
  `frame_history.test.ts`, `alarms.test.ts`, `perf.test.ts`.
- **Codegen preview shader path mis-resolved (07-assets).** `material_artifact_path` returned a
  project-relative path; the renderer's shader loaders treat a relative path as relative to the
  *shader* dir (`resolve_shader_dir()`), so the preview `.spv` resolved to
  `shaders/appdata/userdata/…`. Absolutized `material_artifact_path` (`std::path::absolute`) so
  all three codegen artifacts (`.spv`, `_preview.spv`, `_mesh.spv`) load through the loaders'
  `is_absolute` branch.
- **Codegen preview pipeline destroyed mid-recording (06-rendering UAF).** `render_material_preview`
  moved the per-call `Arc<Pipeline>` into the `FnOnce` draw closure, so the pipeline was destroyed
  the instant `draw()` returned — before `cmd_end_rendering` — invalidating the command buffer and
  segfaulting (the cached studio pipeline survived only because `self` held a clone). Held the
  `Arc` in the outer scope and dropped it after the submit-and-wait. Closed
  `material_codegen_render.test.ts` + the codegen preview cases.

## Earlier state — the Rust host boots and the smoke slice is green

The boot SIGSEGV (lavapipe headless-WSI swapchain crash) is fixed; the Rust host now boots
headless, creates the control socket, and serves the wire. The phase's acceptance smoke slice
(`control.test.ts` + `scene.test.ts`, plus the `animation-control.test.ts` the glob picks up)
passes **18/18** against `target/debug/saffron-host` under headless `weston`/llvmpipe, with
`validationErrors()` empty. The decimal-string-u64 contract test (`tools/check-control-schema`)
also passes **158/158** against the live Rust host.

Wire-contract gaps the e2e exposed past boot — closed here, faithful to `engine-old`:

- **Positional `args` fold.** Every C++ DTO field has a positional index and reads from
  `params.args[i]` when its named key is absent (`requiredField`/`optionalField`,
  `allowPositional = true`, uniform across all 260 fields). The Rust typed `register<P, R>` fed raw
  params straight to serde, ignoring `args`, so `sa <cmd> <value>` and the e2e's `{ args: [...] }`
  silently dropped — `create-entity` failed `missing field name`, `set-aa nonsense` was silently
  accepted. Fixed: `CommandRegistry::register` folds `args[i]` onto DTO `P`'s declaration-ordered
  field names before deserializing (`saffron_protocol::positional_field_order::<P>()` reads the
  `schemars` `properties` order, cached per type).
- **Bool coercion (the C++ `readBool`).** A wire bool field accepts a JSON bool, a number
  (`!= 0`), or a string (anything but `"0"`/`"false"`/`"off"`); the `sa` CLI and editor send `1`/`0`
  and `"on"`/`"off"`. Serde's derive rejected those. Fixed: a `coerce::{boolean,opt_boolean}`
  `deserialize_with` applied to every *params* bool field in `dto.rs` (toggles now round-trip).
- **`EnvironmentDto` envelope.** The `set-environment`/`get-environment`/`set-atmosphere` reply is
  the bare environment object (its OpenRPC schema is `$ref Environment`), but the Rust DTO
  serialized `{ value: {...} }`. Fixed with `#[serde(transparent)]`.
- **Animation edit-preview playhead.** `play-animation` set `preview_in_edit`, but the host's clip
  loader opened a *fresh, empty* `AssetServer` per call (catalog empty ⇒ clip never resolved ⇒
  negative-cached permanently), so `tick_animation` saw no clip and never advanced. Fixed to match
  the C++ `clipLoader` that captured the live `assets&`: `tick_animation` now takes a per-call
  `ClipLoader` (`&mut dyn FnMut`) the host backs with the **live** catalog (`assets.load_anim_clip`),
  removing the stored `'static` loader (one path). The playhead advances in Edit preview.

Entity picking precision and the view-mode behavioral round-trips were already correct
(`picking.test.ts`, `view-mode.test.ts` behavioral assertions all pass); they needed no change.

## Remaining — full-suite green is gated on cross-area engine feature gaps (NOT test infra)

The ~60 still-red assertions are all engine feature gaps owned by other areas' host integration,
each bigger than a focused control/test fix:

- **Play-edge lifecycle is stubbed (host; areas 05 + 12).** `HostLayer::install_play_state_hooks`
  subscribes no-op closures and never sets `sim_tick` — so Edit→Playing builds **no** Jolt world
  and **no** script VM. Every `physics-*`, `script.test.ts`, `ragdoll*`, and play-discard test
  fails (`physics-state` stays `active:false`, the box never falls, scripts never run). The seam
  (`on_play_state_changed`, `sim_tick`, `tick_play`) exists; wiring the world-build-from-components,
  write-back, contact dispatch, script scheduler, kinematic bones, and ragdoll blend is the
  05-physics / 12-scripting host integration.
- **Renderer telemetry counters (area 06).** `render-stats` reports `descriptorBinds`/
  `commandBuffers`/`queueSubmits`/`cpuFrameMs = 0` and `pass-timings` is empty (timestamps report 0
  on llvmpipe). Fails `perf.test.ts`, `profiler.test.ts`, `frame_history.test.ts`, `alarms.test.ts`.
- **Renderer descriptor set-4 unbound (area 06).** Toggling SSGI/DDGI/SSAO trips
  `VUID-vkCmdDrawIndexed-None-08600: set 4 ... not bound` — the validation-clean assertions in
  `toggles.test.ts` / `view-mode.test.ts` fail (the round-trips themselves pass).
- **Texture import → shaded pixels (areas 06/07).** The material/normal `*_render` and
  `material_*` texture tests need a glTF texture import to perturb the shaded result; the pixel
  diff is unchanged.
- **Thumbnail async worker (area 07).** `thumbnail_async.test.ts` (cold-pending → resolve).

These are tracked against their owning areas; this phase's harness glue is complete and the smoke
slice is green. Full-suite green follows as those areas land their host integration.

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
