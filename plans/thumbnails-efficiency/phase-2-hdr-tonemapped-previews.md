# Phase 2 — tonemapped HDR previews

**Status:** COMPLETED

After phase 1. `convertToRgb` (`renderer_detail.cppm:1209-1248`) clamps RGBA16F values to
[0,1] before the ×255 encode. That is correct for the screenshot/capture path — the
rendered offscreen is already tonemapped to display range — but wrong for a thumbnail of
an HDR *asset*, whose radiance values run far past 1.0: the sky preview comes out mostly
blown-out white.

## The work

- Give the PNG conversion an explicit transfer mode instead of the implicit clamp: a small
  enum (`Clamp` for captures, `Tonemap` for HDR asset thumbnails) threaded through
  `encodeBufferToPng` / `convertToRgb`. Capture/screenshot callers keep `Clamp`;
  `thumbnailResult`'s texture branch selects `Tonemap` when the catalog entry is `hdr`
  (`assets.cppm:1815`).
- Tonemap on the CPU in the conversion loop — post-phase-1 the image is ≤ size×size, so
  this is ~16k pixels, not 8.4M. Reinhard (`c / (1 + c)`) plus the usual gamma encode is
  enough for a recognizable preview; pick exposure from a quick luminance pass over the
  small image if plain Reinhard reads too dark.
- Mesh/material thumbnails are unaffected (their offscreen render is display-range
  already).

## Verification

- e2e: thumbnail of an HDR fixture decodes to a PNG that is not uniformly white — assert
  some pixel variance / mean below a threshold.
- Manual: the qwantani sky tile shows sky-and-horizon detail instead of a white square.
- Milestone gate: `make engine` + `make prepare-for-commit`.
- Docs: note the HDR preview tonemap in
  `docs/content/explanations/ui-and-editor/assets-panel-and-thumbnails.md`.
