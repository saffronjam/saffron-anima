# GPU particle / VFX system

**Status:** PENDING IDEA

> Inspiration backlog — not yet implementable as written. Needs a codebase pass (render-graph buffer
> resources + indirect-draw args, a GPU sort kernel, and how the node editor's Slang codegen extends to
> a context-stack model).

**The keystone enabler.** Smoke/fire, weather, sparks, destruction dust, and water splashes all hang off
this. Saffron has unique wins here that mainstream engines lack out of the box, and building it also lays
down the **persistent-buffer + indirect-draw-args** primitives that the deferred **GPU-driven culling**
gap needs — build it once, retire two gaps.

## What it is

A general particle/VFX system: emitters spawn particles, compute updates them entirely on the GPU (no
CPU readback), and renderers draw them as sprites, meshes, ribbons/trails, or lights. Authoring is a
node graph.

- **UE5:** Niagara (Emitter/System with a Spawn/Update/Render module stack; data interfaces sample
  meshes, skeletons, physics; GPU or CPU sim).
- **Unity:** VFX Graph (GPU, node-authored) and the older Shuriken (CPU).

## Core technique

Structure-of-arrays particle buffers live in GPU memory. Each frame: **spawn** (append via atomics) →
**update** (integrate forces, curl-noise turbulence, per-property curves, collision) → **sort** (bitonic,
for correct alpha blending) → **render** via indirect draw with GPU-written draw args. Sprite renderers
build camera- or velocity-aligned quads; mesh renderers instance; ribbons stitch a strip along the
particle history. **Collision** comes from either scene-depth reconstruction or `traceRayEXT` against the
existing TLAS. **Data interfaces** let an emitter sample a mesh surface or a skinned vertex buffer for
spawn positions.

## Saffron-specific wins (why ours can be better than stock)

- **Ray-traced particle collision** — the TLAS and RT pipeline already exist, so particles can collide
  with the *real* scene, not just the depth buffer.
- **Light-emitting particles** — forward+ clustered lighting already indexes many lights, so sparks/embers
  that actually illuminate the scene are cheap.
- **Skinned-mesh emission** — the compute-skinning buffers already exist, so particles can spawn from an
  animated character's surface (blood, sweat, magic-from-hands) for free.
- **Node-graph authoring** — the material editor's React Flow → Slang codegen generalizes to a
  Spawn/Update/Output context stack; one codegen pipeline, not a second authoring tool.

## Build size

- **L** for the core runtime (buffers, spawn/update/sort, indirect render).
- **S** mesh renderer, **M** sprite + light renderers, **L** ribbon/trail.
- **M** the module library (forces, curl-noise, curves, color-over-life).
- **L** node-graph authoring.
- **M** RT/depth collision; **M** skeletal/mesh sampling data interfaces.

## Dependencies (do these first)

- **Persistent / transient render-graph buffer resources + indirect-draw-args support** — this is the
  shared primitive with the GPU-driven-culling gap; build it here.
- A **GPU sort kernel** (bitonic) for back-to-front transparency.
- Node authoring rides on the existing **React Flow → Slang** material-editor pipeline.

## What we reuse / what's missing

**Reuse:** compute, the render graph, bindless + instancing, forward+ clustering (light particles), the
TLAS + RT pipeline (RT collision — a real differentiator), compute-skinning buffers (skinned emission),
React Flow → Slang codegen, and the contact-event ring (collision → gameplay events).

**Missing:** persistent/transient render-graph buffers + indirect-draw args (the shared prereq), and a
GPU sort kernel.

## Unlocks (downstream pending ideas)

`smoke-fire-and-fluids` (FLIP liquid spatial binning), `surface-detail-and-screen-fx` (weather
precipitation), `destruction-and-fracture` (dust/debris VFX), `water-and-ocean` (splashes/spray). Build
this before any of them.

## Notes & references

- UE5 Niagara module/data-interface docs (dev.epicgames.com) — the Spawn/Update/Render stack model.
- Unity VFX Graph docs — the GPU-only authoring model.
- "GPU Gems 3" particle chapters + bitonic-sort references for the transparency sort.
