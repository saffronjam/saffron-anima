# Phase 7 — wire all geometry passes to the deformed buffer

**Status:** COMPLETED

## Goal

Now that the compute pre-pass (Phase 6) produces a deformed `Vertex` buffer, point the **depth prepass**,
the **shadow passes**, and the **SSAO G-buffer prepass** at it and delete their `if (batch.skinned)
continue;` guards. The result: animated characters get early-Z, self-shadow and cast shadows correctly,
and receive SSAO/AO — the three defects that come from skinned geometry only existing in the main scene
pass. (Motion vectors are Phase 8; the BLAS is Phase 9.)

## What exists to build on

- The skinned-skip guards, each commented "skinned draws render in the scene pass only (v1)":
  - depth prepass — `renderer_drawlist.cpp:567`
  - directional + spot shadow depth — `:594`, `:761`
  - SSAO G-buffer prepass — `:625`
- These passes record with the **non-skinned** vertex pipeline already (they draw static geometry on
  binding 0) — so once they bind the deformed buffer for skinned items, they need **no skinned shader
  variant** (the whole point of Phase 6).
- Scene-pass attachments (`renderer.cppm:2621-2641`): color `storeOp = eStore` (unless MSAA), **depth
  `storeOp = eDontCare`** (`:2640`). The depth prepass result is loaded by the scene pass only when
  `doDepthPrepass` is true (`:2637`).

## Work

### 1. Bind the deformed buffer in each pass's record loop

For depth prepass, shadow (directional + spot), and SSAO G-buffer record loops: where they currently
`continue` on `batch.skinned`, instead bind the deformed `Vertex` buffer (at the item's
`deformedVertexOffset`, from Phase 6) on binding 0 and draw exactly like a static batch. Remove the four
guards. These passes use the existing non-skinned pipelines unchanged.

### 2. Make skinned depth survive for the prepass→scene reuse

The depth prepass now writes skinned depth, but the scene pass's depth `storeOp = eDontCare`
(`renderer.cppm:2640`) and the prepass is a separate pass — confirm the prepass attachment **stores**
depth and the scene pass **loads** it for skinned items (it already loads when `doDepthPrepass`). If the
prepass depth was being discarded for skinned geometry (because skinned items were skipped), it now needs
to persist: ensure the depth attachment the prepass writes is `eStore` and the scene pass `loadOp = eLoad`
for that image. Adjust only what's needed so skinned early-Z and contact shadows work; do not change
non-skinned behavior.

### 3. Compute-pass ordering

The Phase-6 `skin` compute pass must run **before all of these** passes (depth/shadow/SSAO all now read
the deformed buffer). Confirm graph ordering puts the compute dispatch first and that each consuming pass
declares `StorageReadCompute`/vertex-input read on the deformed buffer so the graph derives the
compute→geometry barrier once and reuses the buffer across passes (the deform-once win).

## Validation (done criteria)

- `make engine` green, validation-clean log (no missing-barrier errors from the now-shared deformed buffer
  feeding multiple passes).
- Visual: the skinned fixture now **casts and receives shadows** and shows **AO** on the animated mesh
  (compare a posed frame before/after via the PNG screenshot path). Self-shadowing of a bent limb is the
  clearest check.
- `make prepare-for-commit` clean.
- `docs/`: extend `compute-skinning.md` with the "every pass reads the deformed buffer" section and note
  the removed `v1: scene-pass-only` limitation.

## Notes / gotchas

- **Shadow cascades sample the buffer multiple times** — that's fine; it's a read, the deform already
  happened once in compute. The cost saved vs per-pass VS skinning grows with cascade count.
- Don't forget the **spot** shadow guard at `:761` in addition to the directional one at `:594` — the
  verification flagged both.
- If a missing-barrier validation error appears, it's almost always a consuming pass that reads the
  deformed buffer without declaring the access on its `RgPass` — fix the declaration, never hand-insert a
  barrier.
- This phase changes a shared depth store-op; re-run the full smoke + a non-skinned scene to confirm no
  regression to static-mesh depth/shadows.
