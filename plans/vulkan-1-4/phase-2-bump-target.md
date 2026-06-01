# Phase 2 — Bump the target + sync docs

**Status:** COMPLETED
**Depends on:** phase 1 (go decision)

**Result (2026-06-01):** four edits in `renderer.cppm` applied (`require_api_version(1,4,0)`,
`set_minimum_version(1,4)`, VMA `VK_API_VERSION_1_4`, empty `features14` wired via
`.set_required_features_14`). Incremental `-j1` rebuild clean. llvmpipe selects under the raised
1.4 floor (no "no suitable GPU"); zero validation **errors** (the 5 perf-hint warnings about
unconsumed vertex attributes are pre-existing and present in the pre-bump baseline too). Capture
byte-identical to the 1.3 baseline (`cmp -s`, both 22722 bytes). Docs/`AGENTS.md` target-claims
synced to 1.4 with 1.3 provenance kept; `hugo --gc` exit 0.

The load-bearing change. Four edits in `renderer.cppm`, a rebuild, a validation + pixel-identity
check, and the doc/metadata sync in the same change (per AGENTS.md "Keep `docs/` current").

## Code (`engine/source/saffron/rendering/renderer.cppm`)

1. `:59` — `.require_api_version(1, 3, 0)` → `.require_api_version(1, 4, 0)`.
2. `:99` — `.set_minimum_version(1, 3)` → `.set_minimum_version(1, 4)`.
3. `:193` — `allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_3` → `VK_API_VERSION_1_4`.
4. Feature chain (`:81`–`:90`): add a default-initialized
   `VkPhysicalDeviceVulkan14Features features14{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES };`
   and link it into the `pNext` chain alongside 11/12/13. Leave its bits at default — wiring it
   now means phase 3 can flip a 1.4 feature on with a one-liner. No existing 1.1/1.2/1.3 bits
   change; they stay valid under a 1.4 instance.

That is the entire functional change. The engine uses no 1.4-only feature yet, so behaviour is
identical — this only raises the floor.

## Verify

In the `saffron-build` toolbox:

```sh
cmake --build build/debug -j1
SAFFRON_EXIT_AFTER_FRAMES=5 SAFFRON_CAPTURE=/tmp/vk14.png ./build/debug/bin/SaffronEditor
```

- **Validation-clean:** no validation-layer errors on startup or the 5 frames (llvmpipe + layers).
- **Device selects:** startup device-info log shows the device chosen at API ≥ 1.4; no
  "no suitable device" failure from the raised floor.
- **Pixel-identical:** the captured frame matches a pre-bump capture (the cube-fallback pixel
  gate). Nothing should move — same features, same passes.

## Sync docs + metadata (same change)

Change only the **target / requires** statements; keep **feature-provenance** statements
("dynamic rendering and sync2 are Vulkan 1.3 core") as-is.

- `AGENTS.md`: tech-stack table (:51) `target **1.3**` → `target **1.4**`; status bullet (:178)
  "Vulkan 1.3 via Vulkan-Hpp" → "Vulkan 1.4 …".
- `docs/content/explanations/vulkan-foundation/_index.md` (:8) — "targeting Vulkan 1.3" → 1.4.
- `docs/content/explanations/vulkan-foundation/device-and-swapchain.md` — the InstanceBuilder
  "requires API version 1.3" (:12), the "bare 1.3 device" / selector lines (:16, :42, :43); keep
  the "1.3 feature bits: dynamicRendering, synchronization2" list (:18) unchanged.
- `docs/content/explanations/vulkan-foundation/dynamic-rendering.md` (:8),
  `synchronization2-and-barriers.md` (:52) — "targets Vulkan 1.3" → 1.4, but keep "both are
  Vulkan 1.3 core".
- `docs/content/explanations/frame-and-render-graph/render-graph-overview.md` (:95) — "targets
  Vulkan 1.3" → 1.4.
- `docs/content/explanations/global-illumination-and-raytracing/raytracing-device-gating.md`
  (:9) — "targets Vulkan 1.3" → 1.4.
- `docs/content/explanations/ui-and-editor/imgui-integration.md` (:8) — "Vulkan 1.3 dynamic
  rendering" is provenance; fine to leave, or soften to "Vulkan dynamic rendering".
- Rebuild docs to confirm: `cd docs && ~/.local/bin/hugo --gc` (exit 0).
- (`ui-and-editor/mesh-thumbnails.md` has a `1.3f` framing factor — not a Vulkan version, leave it.)

## Done when

- The four code edits are in, build is validation-clean, the frame is pixel-identical to pre-bump,
  the device selects at ≥ 1.4, and every target-claim doc + the AGENTS.md table read 1.4 (with
  provenance claims intact). Mark COMPLETED.
