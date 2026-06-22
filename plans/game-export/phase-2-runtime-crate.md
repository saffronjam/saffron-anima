# Phase 2 — extract `saffron-runtime`

**Status:** IN PROGRESS (implemented; e2e gate pending)

Move the play-mode simulation spine out of `HostLayer` into a new shared crate so both the editor
host and the standalone player run the world through one code path. Per **NO LEGACY**, the host's
inline spine is removed and rebuilt on the new crate in this same change.

## The crate

- New `engine/crates/runtime` → `saffron-runtime`, added to the workspace (`engine/Cargo.toml`).
- DAG (simulation only, never rendering/window):
  ```
  saffron-runtime → {saffron-core, saffron-scene, saffron-assets, saffron-animation,
                     saffron-script, saffron-physics, saffron-geometry}
  ```

## What moves (the renderer-independent play spine)

From `HostLayer::update_session` (`engine/crates/host/src/layer.rs`) and the play path it drives,
extract into a `RuntimeSession`-style type:

- `tick_animation` (frame-rate-independent animation advance).
- pose snapshot for ragdolls / pose-override blending.
- `tick_play` — the fixed-step physics loop (the `sim_tick` closure run once per fixed step).
- the script tick and the script-sink draining (logs + errors).
- the script input snapshot (`ScriptInputState` / `derive_script_input_edges`).

Reuse the existing subsystem types **verbatim** (do not reimplement): `AssetServer`,
`AnimationRuntime`, `ScriptHost` (`engine/crates/script/src/runtime.rs`), `SharedPhysics`,
component registry. Proposed shape:

```rust
pub struct RuntimeSession { scene, assets, animation, script, script_registry,
                            physics, script_input, /* sinks */ … }
// lifecycle: load(...) / start_scripts(...) / advance(dt) / stop()
```

`advance(dt)` performs: tick animation → pose snapshot → fixed-step physics → script tick → drain
sinks. It must be free of any renderer, window, control-socket, shm, or edit-mode reference.

## What stays in `saffron-host`

Edit-only concerns remain in the host: the gizmo + selection, the fly camera, undo/redo, the
Edit↔Play state machine, the control socket, shm publishing, the overlay. The host's **play**
state is refactored to drive a `RuntimeSession` (the host owns one and calls `advance` during
play), and the old inline spine is **deleted**.

## Watch-outs

- `update_session` interleaves play-tick logic with edit concerns (play-state edges, fly camera,
  edit smoothing, parent-death watch). Separate "advance the world" cleanly from "edit session"
  bookkeeping; the latter stays in the host.
- Keep determinism: the fixed-step accumulator and tick ordering must not change — the e2e physics
  flows assert behavior.
- `render_scene` does **not** move (it lives in `saffron-assets`); the runtime crate has no draw
  responsibility.

## Gate

Done (all exit 0): workspace `cargo build`; `cargo clippy -p saffron-runtime -p saffron-sceneedit
-p saffron-host -- -D warnings`; unit tests for runtime + sceneedit + host — including the
real-Jolt falling-box + teardown-ordering tests, which execute in the toolbox. The refactored host
boots clean headless (`just run-engine-headless`): renderer up, control socket listening, frames
run, validation-clean exit.

Pending: a green `just e2e` for the play/physics/script flows over the wire. The focused subset
currently fails only on the harness's 5 s `beforeEach` socket-boot timeout under heavy contention
(a concurrent `just run` editor + llvmpipe), not on any assertion — the host demonstrably creates
its socket and runs. Re-run in an uncontended environment (close the editor) to close this.
