# Phase 4 — CPU↔GPU correlation

**Status:** NOT STARTED

The single highest-value upgrade in the plan: map resolved GPU timestamp spans onto the CPU
`steady_clock` so a capture is **one merged CPU+GPU timeline** instead of two disconnected axes. Without
this, the editor can only draw a GPU lane and a CPU lane on independent zero-points and the user cannot
see *which CPU phase submitted the work a GPU pass is executing*. With it, Phase 2's CPU spans and
Phase 3's nested GPU spans share one clock — the prerequisite for the two-lane timeline in Phase 7 and
for a correct Chrome-Trace / Perfetto export in Phases 5/9.

## The mechanism: `VK_EXT_calibrated_timestamps`

GPU timestamps count device ticks from an unknown epoch; CPU `steady_clock` counts host nanoseconds from
another. `VK_EXT_calibrated_timestamps` samples *both clocks at the same instant*, giving the offset to
project one onto the other.

- **Enable the extension** at device creation, next to the existing `VK_EXT_memory_budget` enable
  (`renderer.cppm` device-create block near `renderer.cppm:254-257`). It is a device extension; request
  it through the vk-bootstrap selection path and degrade gracefully if unavailable (see fallback below).
- **Enumerate calibrateable domains** (`vkGetPhysicalDeviceCalibrateableTimeDomainsEXT`) and pick the
  host domain that matches `steady_clock` — on Linux that is `CLOCK_MONOTONIC_RAW` (or
  `CLOCK_MONOTONIC`); confirm `std::chrono::steady_clock`'s backing clock and pick the matching domain so
  no unit conversion fudge is needed.
- **Sample both clocks** via `vkGetCalibratedTimestampsEXT` (device + host in one call) and store the
  resulting `deviceToHostNsOffset` on `GpuProfiler` (`renderer_types.cppm:601-614`) alongside the
  existing `timestampPeriod`/`timestampMask`. The call also returns a max-deviation; record it so the
  capture metadata can flag low-confidence correlation.

## Applying the offset

In `readbackGpuTimings` (`renderer.cppm:787-840`), after the existing `timestampMask` + `timestampPeriod`
conversion, add the stored offset so each GPU span's `startNs/endNs` land on the *same axis* as Phase 2's
CPU spans. Keep the mask/period math exactly as-is — the offset is a final additive step, not a
replacement.

**Drift:** the two clocks drift over a long capture, so re-calibrate periodically — every N frames (e.g.
once a second) is enough for the bounded captures this plan targets. Store the offset per recalibration
and apply the nearest one to each frame's spans.

## Graceful fallback

`VK_EXT_calibrated_timestamps` is **not** guaranteed — some llvmpipe/lavapipe configs lack it. When the
extension or a matching host domain is absent:

- Keep GPU spans on their **own** axis (zeroed to the first GPU timestamp of the frame) and set a
  `correlated = false` flag in the capture metadata.
- The editor (Phase 7) then renders a GPU-only lane and a separate CPU lane rather than faking a merged
  timeline — honest, not wrong. This composes with the existing `softwareGpu` honesty: a llvmpipe capture
  may be both software-GPU *and* uncorrelated, and both flags propagate into the export `args`.

## Files touched

| What | File | Symbols |
|---|---|---|
| Extension enable + domain pick | `engine/source/saffron/rendering/renderer.cppm` (device-create) | calibrated-timestamps ext, domain enumeration |
| Offset + deviation storage | `engine/source/saffron/rendering/renderer_types.cppm` | `GpuProfiler` (offset, maxDeviation, correlated) |
| Apply offset at read-back | `engine/source/saffron/rendering/renderer.cppm` | `readbackGpuTimings` |
| Periodic recalibration | `engine/source/saffron/rendering/renderer.cppm` | `beginFrame` / `endFrame` calibration tick |

## Validation

- `make engine` + `make prepare-for-commit` clean.
- Headless with timestamps mode: a GPU pass span's `[startNs, endNs]` falls *within* the CPU
  `executeRenderGraph` phase span that recorded it (sanity: the GPU executes after the CPU records, but
  on the merged axis the windows overlap plausibly — assert the GPU span starts no earlier than the CPU
  submit and the magnitudes are within an order of magnitude when not software-GPU).
- On a device without the extension, the `correlated = false` path is taken, the capture still completes,
  and no crash/validation warning is emitted.
- The engine now *has* a merged internal timeline even before any command exposes it — wire surface
  lands in Phase 5.
