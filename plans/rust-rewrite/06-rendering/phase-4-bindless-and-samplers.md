# Phase 4 — bindless descriptor table, samplers, and slot allocation

**Status:** COMPLETED

**Depends on:** 06-rendering:phase-3-gpu-resources

## Goal

Port the descriptor infrastructure built once at startup: the descriptor-set layouts (bindless image
array = set 0, light UBO = set 1, instance SSBO = set 2, plus the post-process compute layouts), the
descriptor pools (one `eFreeDescriptorSet`, one `eUpdateAfterBindPool` for bindless), the single global
bindless set, and the samplers (linear, shadow-compare, nearest). This is the `Descriptors` sub-state;
every texture upload claims a stable bindless slot here, and every draw binds this one set 0.

## Why this shape (NO LEGACY)

- **One global bindless combined-image-sampler array, bound once, indexed per-instance.** This is the
  bindless-at-scale pattern wgpu cannot express and the reason ash is mandatory. `upload_texture` writes
  a stable slot (update-after-bind) and returns its index; the default white is slot 0
  (`renderer_types.cppm:1121`). The cap is `MaxBindlessTextures = 1024` (`:74`).
- **Slot allocation + the free-list under `bindlessMutex()` (the explicit `Arc<Mutex>` site, README §5).**
  Claiming a slot pops the free-list (returned by dropped `GpuTexture`s, phase 3) before growing
  `next_bindless_index`, so a churny scene stays bounded. The `vkUpdateDescriptorSets` write to the
  shared set and the free-list push/pop both take the lock, because the thumbnail worker can also upload
  (`renderer_textures.cpp:132`). The free-list is the same `Arc<Mutex<Vec<u32>>>` phase 3 wired into
  `GpuTexture`.
- **Layouts/samplers are device-shared and immutable after init → they live on `Device` or the
  `Descriptors` sub-state and are borrowed `&`.** The per-view *sets* that bind per-view images do **not**
  live here — they live in `ViewTargets` (phase 9/10/11) so a view switch never leaves a set pointing at
  another view's images (`renderer_types.cppm:1130`). This phase owns only the device-global layouts +
  the single bindless set.
- **`Descriptors` is a sub-state struct with its own Drop** freeing pools/layouts/samplers in order
  (sets are freed implicitly with the pool). The free-list is held as the shared `Arc<Mutex>` so it
  outlives both the descriptors and any texture whose Drop pushes to it (matching the C++ `Ref`).
- **No second "bindless v2" path.** There is exactly one bindless set and one slot allocator; the negative
  / reuse cache is the free-list, not a parallel structure.
- **The default white is uploaded *and* written at init — one path, no fixture workaround.** `Renderer::new`
  uploads a 1×1 opaque-white RGBA8 `GpuTexture` (claiming slot 0) and seeds it into *every* bindless slot
  via `Uploader::upload_default_white` → `Descriptors::seed_all_textures`, so a partially-bound array is
  never sampled unbound (lavapipe faults sampling an unwritten slot even on indices a shader skips; UB on
  real hardware). The white is held on the renderer (`default_white`) for its lifetime. The thumbnail /
  material-preview tests run this same production primitive in their fixture rather than hand-writing a
  white into slot 0 — the C++ default-white upload + fill in `initRenderer` (`renderer.cppm:532`).

## Grounding (real files/symbols)

- `engine-old/source/saffron/rendering/renderer_types.cppm` — `Descriptors` (`:1113`): the layouts
  (`bindlessSetLayout`, `lightSetLayout`, `instanceSetLayout`, `tonemapSetLayout`, `fxaaSetLayout`,
  `taaSetLayout`, `clusterSetLayout`), the pools (`descriptorPool` eFreeDescriptorSet, `bindlessPool`
  eUpdateAfterBindPool), `bindlessSet`, `nextBindlessIndex`, `bindlessFreeList` (`:1129`), the samplers
  (`linearSampler`, `shadowSampler`); `MaxBindlessTextures` (`:74`); `bindlessMutex` (`:42`).
- `engine-old/source/saffron/rendering/renderer_textures.cpp` — the slot claim/write/return path and the
  `bindlessMutex` lock (`:132`); `bindlessTextureCount`/`bindlessFreeCount` inspectors.
- `engine-old/source/saffron/rendering/renderer.cppm` — descriptor-resource init inside `newRenderer`
  (the `initDescriptorResources`-style setup), the default-white texture (`defaultWhiteTexture`,
  `:1766`).
- README §5 (the two `Arc<Mutex>` sites).

## Acceptance gate

- `cargo build -p saffron-rendering` and the workspace build are green.
- `cargo test -p saffron-rendering` passes named tests:
  - the bindless set is created with `update-after-bind`; slot 0 is the default white after init.
  - claim N slots, drop those textures, claim N more → the free-list is reused and `next_bindless_index`
    does not grow past N (the bounded-pool invariant).
  - two threads claiming slots concurrently never alias a slot (the `Arc<Mutex>` discipline holds).
- A validation-clean GPU smoke: bind set 0 in a trivial pass and sample slot 0 with zero validation
  messages (the descriptor wiring is real-GPU-valid, incl. the update-after-bind flag).
- An untextured material renders on lavapipe through the production default-white path (no fixture
  workaround): `untextured_material_samples_seeded_default_white_slot` runs `Uploader::upload_default_white`
  exactly as `Renderer::new` does, then previews a texture-less material — the result reads bright
  (white × factor passes through) and validation-clean, where an unwritten slot 0 would fault / sample
  zero. This proves the init-time upload-and-seed is the one live path.
