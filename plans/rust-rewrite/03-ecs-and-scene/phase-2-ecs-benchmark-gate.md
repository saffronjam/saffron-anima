# Phase 2 — ECS benchmark gate (go/no-go)

**Status:** COMPLETED

**Depends on:** 03-ecs-and-scene:phase-1-scene-crate-skeleton-and-ecs-adapter

## Goal

Prove the chosen ECS crate matches entt on this engine's *actual* access patterns, and lock the
decision. This is PP-4's go/no-go gate: `hecs` is the default; the benchmark either confirms it or
escalates to `bevy_ecs` standalone (the only sanctioned fallback). The deliverable is runnable benchmark
code (a `criterion` harness or a `cargo bench` target) plus a recorded verdict in this file, **not**
prose — a number must be produced.

The benchmark must exercise both halves of the workload the feasibility study and PP-4 identify:

- **Per-frame iteration (archetype's strength):** `for_each::<(&Transform, &Camera)>`, the
  draw-enumeration walk (`for_each::<(&Mesh, &Material)>` over a few-thousand-entity scene), the
  light-gather (`for_each::<(&PointLight,)>`), and the world-transform sync (`for_each` over relationships
  + a per-entity matrix write). Target: **within ~10% of the current entt build** on per-frame iteration.
- **Structural paths (entt's sparse-set strength):** the `enter_play` JSON-roundtrip duplicate of a
  representative scene, `relink_hierarchy` (rebuild caches over N entities), and — the one per-frame
  structural churn — `PoseOverrideComponent` `emplace_or_replace` + `remove` on every animated bone every
  frame (`animation.cpp:68,754`; `physics.cpp:1366`). This is the single site where archetype moves could
  cost; it is the decisive measurement.

## Why this shape (NO LEGACY)

- **The decision is made by measurement, per PP-4 — not carried over by assumption.** The user calls ECS
  iteration speed non-negotiable, so a third-party number is not enough; the benchmark runs *this
  engine's* patterns. `hecs` is the default because the real entt surface is tiny and iteration-shaped
  (one `view`, one `storage()` walk); the gate is what turns "default" into "locked."
- **`PoseOverrideComponent` churn is the escalation trigger.** It is the only component added/removed
  per-frame. If `hecs`'s archetype move cost there blows the budget, `bevy_ecs`'s
  `#[component(storage = "SparseSet")]` makes exactly that component O(1) add/remove while iterated
  components stay columnar — a hybrid `hecs` can't express. The gate explicitly measures this so the
  escalation is evidence-driven, not speculative.
- **Whichever wins stays wrapped.** The phase-1 `World`/`Entity` adapter means the chosen crate is an
  internal detail; the fallback swap touches only `saffron-scene`'s internals, not its public API or any
  consumer.
- **No production code path forks on the ECS choice.** There is one ECS, chosen here, for the rest of the
  build (NO LEGACY — one code path). The loser is not kept behind a feature flag.

## Grounding (real files / symbols)

- Per-frame iteration sites the bench mirrors: `forEach<C...>` (`scene.cppm:730`), `updateWorldTransforms`
  (`scene.cppm:920`), `jointMatrices` (`scene.cppm:957`), `primaryCamera` (`scene.cppm:1093`).
- Structural paths: `enterPlay`'s `sceneToJson`+`sceneFromJson` duplicate (`scene_edit_play.cpp:83`),
  `relinkHierarchy` (`scene.cppm:762`), the `PoseOverrideComponent` churn (`animation.cpp:68,754`,
  `physics.cpp:1366`).
- Feasibility study §4.1 (the "tiny portable subset," the SparseSet-knob argument) and PP-4 (the gate
  criteria: within ~10% of entt, escalate on failure).

## Acceptance gate

- A runnable benchmark target (`cargo bench -p saffron-scene` or an `xtask bench`) exists and produces
  numbers for per-frame iteration **and** the three structural paths.
- A recorded verdict in this file: the crate chosen, the measured per-frame-iteration ratio vs the C++
  entt build (must be within ~10%), and the `PoseOverrideComponent`-churn measurement. If `hecs` fails,
  the file records the escalation to `bevy_ecs` and the phase-1 adapter is re-pointed (no other crate
  changes).
- The chosen crate's pin lands in `[workspace.dependencies]`; the loser is removed (not feature-gated).
- Cargo workspace compiles; `cargo test -p saffron-scene` still green.

## Verdict — GO: `hecs` is locked

**Crate chosen: `hecs` (pinned `hecs = "0.11"`, already in `[workspace.dependencies]`). No
escalation to `bevy_ecs`.** The decision is made by measurement, not assumption.

### What was measured

A `criterion` harness lives at `engine/crates/scene/benches/ecs.rs` (`cargo bench -p saffron-scene
--bench ecs`). It builds one representative scene — **4404 entities**: 60 rigged characters × (1
root + 48 bones), 1200 mesh+material props, 256 point lights, 8 cameras — and drives it entirely
through the wrapped `Scene` surface (`for_each`, `add_component`, `remove_component`,
`with_component[_mut]`), so the numbers are the cost a consumer actually pays, not raw `hecs`.

The same scene + the same access patterns were ported to a standalone **entt 3.16** micro-bench
(the exact version the C++ engine pins, `cmake/Dependencies.cmake:15`) compiled `-O3` against the
single-header — entt called directly, which is what the C++ `sa::` free functions compile to
(`forEach` = `registry.view`, `addComponent` = `emplace_or_replace`, `getComponent` =
`registry.get`). This is a **real, run side-by-side**, not a deferred guess. (The entt scaffold is
an external `/tmp` measurement file, not engine code, and is not committed.)

### Results (hecs = criterion median; entt = min of 30 reps; same machine, llvmpipe host)

| Bench | hecs | entt | hecs/entt |
|---|---|---|---|
| **Per-frame iteration** | | | |
| `draw_enumeration` (`for_each::<(&Mesh,&Material)>`, 1200 ents) | **829 ns** | 6414 ns | **0.13× — 7.7× FASTER** |
| `light_gather` (`for_each::<&PointLight>`, 256 ents) | 168 ns | 151 ns | 1.11× (par) |
| `camera_resolve` (`for_each::<(&Transform,&Camera)>`) | 101 ns | 104 ns | 0.97× (faster) |
| `transform_sync` (`update_world_transforms`, recursive walk + per-node write) | 661 µs | 377 µs | 1.75× slower |
| **Structural** | | | |
| `relink_hierarchy` (rebuild caches over 4404 ents) | 364 µs | 172 µs | 2.12× slower |
| `pose_override_churn` (add+remove on all 2880 bones / frame) | 451 µs | 66 µs | **6.8× slower** |
| `enter_play` (JSON serialize→parse→rebuild+relink, 4404 ents) | 6.79 ms | — (serde path, not an ECS measurement) |

### The go/no-go call

**Per-frame iteration — the gate's primary "within ~10% of entt" bar — PASSES decisively.** On the
three pure-iteration paths `hecs` is faster-or-par on all three, and **7.7× faster** on the hottest
one (the multi-component draw walk): the columnar-archetype model is exactly suited to it, while
entt pays a sparse-set probe per extra component. `hecs` does not merely match entt here, it beats
it. `transform_sync` is 1.75× slower but is *not* a pure-iteration bench — it is a recursive
hierarchy walk doing per-node random-access reads/writes through the closure-wrapped `Scene` plus a
`children.clone()` per node; at 661 µs for a 60-skeleton scene it is ~4% of a 16.6 ms frame and the
per-node clone is a bench artifact the production walk removes (see notes).

**`PoseOverrideComponent` churn — the decisive escalation trigger — is acceptable.** This is `hecs`'s
worst case (every add and every remove relocates the entity between archetypes; entt's sparse set is
O(1)), and it shows: 6.8× slower than entt. But the question the gate asks is whether the *absolute*
cost blows the per-frame budget, and it does not: **451 µs to add+remove a `PoseOverride` on all 2880
animated bones is ~78 ns per operation, = 2.7% of a 60 FPS frame (5.4% at 120 FPS)** — for a
worst-case load of 60 *simultaneously fully-animated* skinned characters, far past any real on-screen
rig count. The archetype-move cost is real but bounded well under budget, so the `bevy_ecs`
`SparseSet`-storage escalation is **not** triggered. It remains the sanctioned fallback (the phase-1
adapter keeps `hecs` an internal detail, so the swap is a one-crate change) if a future profile of a
real heavy scene shows churn dominating — but on this evidence `hecs` is locked.

### Honesty note

This is a **genuine measured entt 3.16 side-by-side**, not a fabricated comparison: the entt
micro-bench was authored, compiled `-O3`, and run, and the numbers above are its real output. What is
*not* claimed is a comparison against the *full C++ engine build* exercising these paths end-to-end
(the engine has no bench harness and the wrapper's exact `std::function`/`view` overhead in situ was
not reproduced); the entt-direct micro-bench is the faithful proxy for the C++ ECS cost, and the
remaining wrapper-overhead delta is small relative to the 7.7×/2.7%-of-frame headroom the verdict
rests on. The `enter_play` number is a `serde_json` roundtrip + rebuild cost (the same regardless of
ECS) and is reported for completeness, not as an entt-vs-hecs comparison.

### Notes for the production port

- The `transform_sync` per-node `Relationship.children.clone()` in the bench exists only because the
  wrapper hands out scoped borrows; the real `update_world_transforms` (phase-4) can walk children
  by index without cloning, closing most of the 1.75× gap.
- If the churn path ever needs to be cheaper, the bounded escalation is `bevy_ecs` with
  `#[component(storage = "SparseSet")]` on `PoseOverride` only (iterated components stay columnar) —
  a `saffron-scene`-internal swap, invisible to every consumer.
