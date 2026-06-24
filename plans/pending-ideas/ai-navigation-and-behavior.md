# AI: navigation & behavior

**Status:** PENDING IDEA

> Inspiration backlog — not yet implementable as written. Needs a codebase pass (the recastnavigation
> cxx seam, a behavior-tree interpreter + React Flow authoring, and wiring agents to `CharacterVirtual`).

The **sweet spot is perception + behavior trees first** — they ride entirely on primitives that already
exist (raycast, signals, React Flow, Luau) for very high value per effort. Navmesh is the one gating
investment, and it vendors `recastnavigation` via the exact cxx pattern Jolt already proved.

## What it is

NPC navigation and decision-making: navmesh pathfinding, behavior trees, perception (sight/hearing), and
query-based spatial reasoning.

- **UE5:** Navigation (Recast) + Behavior Trees + Blackboard + Environment Query System (EQS) + AI
  Perception; Mass for large crowds.
- **Unity:** the built-in NavMesh + agents (ships neither EQS nor first-party perception).

## Core technique

**Navmesh:** voxelize world geometry, build walkable regions, and triangulate a polygon mesh
(`recastnavigation`); at runtime, `findPath` over the polys + **funnel** (string-pulling) for a smooth
path; drive the existing **`CharacterVirtual`** controller along it. **Behavior trees** are an
interpreter over a tree of composite (sequence/selector) + decorator + leaf-task nodes sharing a
**blackboard**. **Perception** is a sight cone + line-of-sight raycast (+ hearing events), emitting
signal/slot stimuli. **EQS** generates candidate points/actors, scores them with tests (LOS, path cost,
distance), and returns the best — useful for cover/flanking.

## Build size

- **L** navmesh generation (vendor `recastnavigation` via cxx — the Jolt template).
- **M** runtime pathfinding + agent (Detour `findPath` + funnel → `CharacterVirtual`).
- **M** area costs + dynamic obstacles.
- **M** behavior trees + blackboard (interpreter is S–M; React Flow authoring + JSON asset is the bulk).
- **M** Environment Query System.
- **S–M** AI perception (sight cone + LOS raycast + hearing) — **cheapest high-value win.**
- **L–XL** crowds/RVO (via `dtCrowd`) — defer until GPU culling gives representation LOD.

## Dependencies (do these first)

- **cxx-FFI vendoring pattern** (proven by Jolt) — reused wholesale for `recastnavigation`.
- **Perception + behavior trees need nothing new** — start here while navmesh is built in parallel.
- Crowds want GPU-driven culling (a known gap) for agent LOD — defer.

## What we reuse / what's missing

**Reuse (across all of it):** Jolt raycast + `CharacterVirtual`, React Flow + the asset/override model
(BT/EQS authoring), the signal/slot system (perception stimuli), Luau (custom tasks), the gizmo overlay
(`submit_overlay`) for navmesh/EQS debug draw, and the control plane.

**Missing:** the recastnavigation cxx seam, a BT interpreter + asset type, and the EQS generators/tests.

## Notes & references

- Mononen, `recastnavigation` (Recast + Detour) — the de-facto open navmesh library.
- UE5 Behavior Tree + EQS + AI Perception docs — the node taxonomy and EQS test model.
- "Behavior trees in robotics and AI" (Colledanchise & Ögren) for the formalism.
