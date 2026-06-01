# Vulkan 1.4 target bump

Move the engine's Vulkan API floor from **1.3** to **1.4**.

## Why

Vulkan 1.4 is the current core release (shipped 2024-12-03; the 1.4.340 patch landed 2026-01).
The engine already builds against Vulkan-Hpp **headers 1.4.341** and runs on a 1.4-conformant
dev device (Mesa/llvmpipe), so the bump is small and low-risk here. It is a deliberate
baseline move, not a fix — nothing the engine does today *needs* 1.4. The payoff is alignment
with the current core plus a handful of promoted extensions and raised limits that simplify
future work (see phase 3).

## The nuance (read before editing docs)

Most "1.3" references in the codebase and docs describe **feature provenance**: dynamic
rendering and synchronization2 were promoted to core in Vulkan **1.3**, and that stays true at
1.4. Only the statements about what the engine **targets / requires** change. Do not blanket
replace "1.3" with "1.4" — keep "dynamic rendering and sync2 are 1.3 core", change "the engine
targets 1.3".

## Status convention

Each phase file carries a `**Status:**` line (`NOT STARTED` / `IN PROGRESS` / `COMPLETED`).
Mark a phase `COMPLETED` when its work is done and validation-clean; delete a phase file only
*after* it is `COMPLETED` and merged. Delete this folder once all phases are done.

## Phases

| # | Phase | File | Depends on |
|---|-------|------|-----------|
| 1 | Pre-flight: confirm 1.4 device support + go/no-go | `phase-1-preflight-device-support.md` | — |
| 2 | Bump the target (code) + sync docs/AGENTS | `phase-2-bump-target.md` | 1 |
| 3 | *(optional)* Adopt 1.4-core features that simplify code | `phase-3-adopt-1-4-core-features.md` | 2 |

Phases 1–2 are the whole migration. Phase 3 is opportunistic and can stay `NOT STARTED`
indefinitely; pick a feature off it only when something actually wants it.

## The one real risk

Requiring 1.4 rejects any device/driver that reports only 1.3. AMD, Intel, NVIDIA, and Mesa
all ship 1.4-conformant drivers, and the dev box (llvmpipe) is 1.4 — but phase 1 exists to
confirm the *actual* target GPU before committing.

## Touch points (grounded)

- Code (all in `engine/source/saffron/rendering/renderer.cppm`):
  `require_api_version(1, 3, 0)` (:59), `VkPhysicalDeviceVulkan13Features` chain (:81/84/90),
  `.set_minimum_version(1, 3)` (:99), `allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_3` (:193).
- Metadata: `AGENTS.md` tech-stack table (:51 "target **1.3**") and the status bullet (:178).
- Docs (target claims only): `docs/content/explanations/vulkan-foundation/_index.md`,
  `device-and-swapchain.md`, `dynamic-rendering.md`, `synchronization2-and-barriers.md`;
  `frame-and-render-graph/render-graph-overview.md`;
  `global-illumination-and-raytracing/raytracing-device-gating.md`;
  `ui-and-editor/imgui-integration.md`.
