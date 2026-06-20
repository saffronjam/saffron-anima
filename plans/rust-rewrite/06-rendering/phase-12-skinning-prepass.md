# Phase 12 — GPU skinning prepass + skinned-BLAS refit

**Status:** COMPLETED

**Depends on:** 06-rendering:phase-6-instancing-and-scene-pass, 06-rendering:phase-10-aa-and-temporal

## Goal

Port the compute skinning prepass: deform each skinned mesh-instance once into a per-frame deformed-vertex
buffer that every geometry pass (shadow, depth-prepass, gbuffer, scene) then reads as an ordinary static
vertex stream — the deform-once win. It also deforms the *previous* pose into a prev-deformed buffer for
the motion pass (TAA). The joint palettes live in the `Instancing` sub-state; the cross-frame motion
caches key on entity uuid. The skinned-BLAS refit (for RT, phase 13) rides here conceptually but the AS
build lands in phase 13.

## Why this shape (NO LEGACY)

- **One `skin` compute pass writes both the current and previous deformed buffers; every consumer reads
  it via `VertexInputRead`, and the graph derives the compute-write→vertex-input barrier**
  (`renderer.cppm:1215`–`:1276`). The shadow/depth/gbuffer/scene/motion passes each push a
  `VertexInputRead` (or `AccelStructBuildRead` for the skinned-BLAS) access on the deformed buffer when
  `do_skin` — the conditional accesses stubbed in phases 7/9/10 become live here.
- **The deformed buffers are per-frame-in-flight, grow-only, in `Skinning`** (`renderer_types.cppm:1206`):
  base 32-byte `Vertex` layout, STORAGE|VERTEX usage, sized in `Vertex` elements, never shrunk. The
  prev-deformed buffer is laid out identically (same per-instance offset) so the motion pass reads it as
  the prev-position stream.
- **The cross-frame motion caches are `HashMap<u64, Vec<Mat4>>` (prev palette) + `HashMap<u64, Mat4>`
  (prev model), keyed by entity uuid** (`renderer_types.cppm:1221`). A new entity reads back current ==
  previous so its first frame emits zero motion (no velocity flash). These are owned by `Skinning` and
  mutated through its methods — single-threaded host state, no `Arc<Mutex>`.
- **The skin descriptor pool is reset and re-allocated each frame** (`Skinning.pools[frame]`,
  `SkinMaxSetsPerFrame = 64`); instances past the cap are skipped + logged. The per-instance
  `SkinDispatch` (set + counts + offsets) is built in `submit_draw_list` (phase 6) and consumed in the
  `skin` pass body — `submit_draw_list` gains the joints argument here.
- **The joint palette + prev-joint palette are per-frame SSBOs in `Instancing`** (`renderer_types.cppm:
  1192`), the slots reserved in phase 6 now filled. Skinned vertices come out in world space (the palette
  is `worldBone * inverseBind`, the kernel omits the model matrix), so skinned RT instances use an
  identity transform (phase 13).

## Grounding (real files/symbols)

- `engine-old/source/saffron/rendering/renderer.cppm` — the `skin` pass in `beginFrameGraph` (`:1215`–
  `:1276`), the `do_skin` conditional `VertexInputRead` accesses on every geometry pass, `setSkinning`
  (`:3076`).
- `engine-old/source/saffron/rendering/renderer_types.cppm` — `Skinning` (`:1206`, deformed/prev-deformed
  buffers + the motion caches), `Instancing` joint/prev-joint palettes (`:1192`–`:1198`), `SkinDispatch`
  (`:620`), `SceneDrawList.skinDispatches`/`prevSkinDispatches`/`skinnedRtInstances` (`:648`–`:656`),
  `SkinMaxSetsPerFrame` (`:78`).
- `engine-old/source/saffron/rendering/renderer_drawlist.cpp` — `submitDrawList` (the joints form) building
  the `SkinDispatch` list + the palette upload.
- Shader: `skin`. README §6; the base `Vertex` (32B) layout from `saffron-geometry`.

## Acceptance gate

- `cargo build -p saffron-rendering` and the workspace build are green.
- `cargo test -p saffron-rendering` passes named tests:
  - the `skin` pass appears only when skinned dispatches exist and the deformed buffers are allocated.
  - a new entity's first frame reads back prev == current (zero motion); a moved entity's prev palette
    reflects last frame.
  - the deformed buffer grows to fit peak vertices and is not shrunk.
  - dispatches past `SkinMaxSetsPerFrame` are skipped (and logged), not an error.
- **Golden-image** test: a skinned mesh at a known pose deforms identically to a committed golden, and a
  consumer pass (shadow / scene) reads the deformed buffer correctly. Validation log clean (the
  compute-write→vertex-input barriers are real-GPU-valid).
