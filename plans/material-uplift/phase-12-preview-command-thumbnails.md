# Phase 12 — Preview command + thumbnails

**Status:** NOT STARTED
**Depends on:** 11

## Goal

Expose the preview to the editor: a `preview.render` control command that arms the phase-11 pass and
returns a **PNG/base64 blob**, a `preview.configure` for preview-scene options (lighting preset,
auto-rotate, primitive), and **cached material thumbnails** for the asset browser (the same render path,
regenerated when a material or any bound texture changes).

## Why

The editor presents preview images over the control plane (decision 5 — no second Wayland surface). The
asset browser already shows mesh thumbnails via `getThumbnailUrl`→`getThumbnail`→base64 PNG; material
thumbnails reuse that exact pipeline. Live editing (phase 13) re-requests `preview.render` on each change.

## Commands

- **`preview.render {material, size?, primitive?}` → `{ png: base64 }`** — arm the material on the preview
  sphere (or `cube`/`plane`), render in the next frame, read back, encode PNG (`encodeTextureThumbnailPng`
  tail from `renderer_thumbnail.cpp`), return base64. Because the handler runs before `beginFrame`, this is
  request-now / arm / fulfil-after-the-frame — return the result once the readback for the armed request
  is ready (a small wait or a request id the editor polls; prefer a synchronous one-frame wait for simplicity).
- **`preview.configure {lighting?, rotate?, primitive?}` → `{}`** — preview-scene presets (which studio
  IBL/key-light, auto-rotate angle, default primitive). Editor-side state.
- **`get-thumbnail {asset, size}`** (existing, for meshes) — extend to handle `AssetType::Material`: render
  via the preview path instead of the flat mesh thumbnail. Cache + invalidate on material version bump (phase 10).

## Files to touch

- `engine/source/saffron/control/control_dto.cppm` — `PreviewRender/ConfigureParams` + results.
- `engine/source/saffron/control/control_commands_render.cpp` — register `preview.render`/`preview.configure`;
  arm the phase-11 pass; encode the readback to PNG; return base64.
- `engine/source/saffron/host/host.cppm` / rendering — the arm→render→readback→fulfil handshake across the
  frame boundary; thumbnail cache keyed on `(materialId, version, size)`.
- `tools/gen-control-dto/gen.ts` — add the commands; `preview.render` returns a base64 blob the contract
  test must tolerate (a fixture with a tiny size, or a skip if non-deterministic). Regenerate.
- `tools/se/source/main.cpp` — `se preview render <material> --out file.png` (write the decoded blob to disk).

## Steps

1. Add the DTOs + handlers; wire the arm/fulfil handshake (one-frame latency is fine).
2. Extend `get-thumbnail` to render materials via the preview path; cache + version-invalidate.
3. `gen.ts` fixtures/skips; regenerate; `check-control-schema` green.
4. `se preview render` writes a PNG; e2e asserts a non-empty PNG of expected dimensions.

## Gate / done

- `make engine` + `make schema` clean; `se preview render <id> --out x.png` produces a correctly-lit sphere PNG.
- Material asset tiles in the browser show a PBR-ball thumbnail (phase 13 consumes it).
- `make prepare-for-commit` clean. Docs: preview/thumbnail commands.

## Risks

- **Cross-frame fulfilment**: the cleanest contract is a synchronous "render one frame, then respond"; if the
  present loop makes that awkward, return a request id + a `preview.poll`. Pick one and keep it simple.
- **Thumbnail thrash**: invalidate only on the material's version bump (phase 10), not every frame, or you
  re-render constantly. Cache by `(id, version, size)`.
- **base64 size**: cap preview size (e.g. ≤512²) so the control message stays small.
