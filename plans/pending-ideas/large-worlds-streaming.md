# Large worlds & streaming

**Status:** PENDING IDEA

> Inspiration backlog — not yet implementable as written. Needs a codebase pass (a job pool + content
> catalog + transfer-queue GPU upload, and partial hecs (de)serialization keyed by stable GUIDs).

Mostly relevant only at large or multi-author scale — **defer the whole family** unless target worlds
demand it. The one broadly-useful early piece is **async asset/level loading**, which unblocks plenty on
its own.

## What it is

Worlds too big to hold in memory or float-precision: streamed cells, data layers, hierarchical LODs, and
large-coordinate handling.

- **UE5:** World Partition + One File Per Actor (OFPA) + Data Layers + HLOD + Large World Coordinates.
- **Unity:** Addressables + scene streaming.

## Core technique

- **Async loading:** a job pool loads content off the render thread; a **transfer-queue** uploads GPU
  resources; **ref-counted handles** track liveness.
- **Spatial partition + cell streaming:** a grid/hash of cells, each a load state machine (unloaded →
  loading → loaded), driven by camera position. The real cost is **partial hecs (de)serialization** +
  **stable cross-load GUIDs** so references survive a reload.
- **Data layers:** named subsets toggled independently (gameplay variants, time-of-day sets).
- **OFPA:** each entity in its own file for conflict-free multi-author editing.
- **HLOD:** distant cells render as merged/simplified proxies.
- **Origin rebasing:** shift the world origin near the camera (floating origin) or move to f64 large
  coordinates; the subtlety is **invalidating TAA history** on the rebase frame.

## Build size

- **L** async asset/level loading (job pool + catalog + transfer-queue upload + handles) — **unblocks
  everything else; pairs with the async-compute gap.**
- **XL** spatial partition + cell streaming (the partial-serialization + GUID work is the cost).
- **M** data layers.
- **L** OFPA-style external storage.
- **M** HLOD (instancing-only) / **XL** merged/simplified proxies (needs mesh decimation + atlas baking).
- **L** floating-origin rebasing / **XL** full f64 large-world coordinates.

## Dependencies (do these first)

- **Stable entity GUIDs + partial registry (de)serialization** — the gating primitive for cell streaming.
- **Scene-graph parenting** (a known gap) — streamed sub-hierarchies.
- **Async-compute / transfer-queue** (a known render-graph gap) — for non-stalling GPU upload.
- HLOD proxies need a **mesh-decimation + atlas baker** (new offline tooling).

## What we reuse / what's missing

**Reuse:** the JSON project format + component registry (serialization), bindless + instancing (HLOD),
the render graph (transfer-queue uploads), and the control plane.

**Missing:** the async job/loader infrastructure, stable GUIDs + partial serialization, scene-graph
parenting, the transfer-queue upload path, and (HLOD) a mesh-decimation/atlas baker.

## Notes & references

- UE5 World Partition + OFPA + HLOD + Large World Coordinates docs.
- Unity Addressables docs (the catalog + async-handle model).
- Floating-origin / camera-relative rendering write-ups (and the TAA-history-invalidation gotcha).
