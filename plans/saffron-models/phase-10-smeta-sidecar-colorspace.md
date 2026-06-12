# Phase 10 — `.smeta` sidecar + colorspace provenance

**Status:** NOT STARTED
**Depends on:** 09

## Goal

Add the `.smeta` sidecar (the Unity `.meta` / Godot `.import` analog) for **foreign/headerless** files — a
raw `.png` dropped into `assets/`, or any standalone file whose bytes can't carry identity — holding
`{ id, type, colorspace, folder, name, importOptions? }`. Make colorspace **recoverable** from the
container chunk flags (embedded textures) or the `.smeta` (standalone), rather than only the volatile
`AssetEntry.linear`. The scan detects/repairs a missing or drifted sidecar. Defers: the cache (11),
extraction (12).

## Why

Identity for engine-written files lives in the bytes (the locked decision), but a raw image has no place to
store its `Uuid` or whether it is sRGB color vs linear data — and **a wrong colorspace silently
mis-renders** (a normal map decoded as sRGB). The `.smeta` is the fallback the user specified, used **only**
for foreign formats; engine containers/extracted files don't need one. This closes the one piece of
identity/colorspace a folder scan (phase 09) can't otherwise recover.

## The `.smeta` schema (JSON sidecar, e.g. `textures/<uuid>.png.smeta`)

```jsonc
{
  "version": 1,
  "id": "<decimal-uuid>",        // stable identity for the sibling file
  "type": "texture",             // mesh | texture | material | animation
  "colorspace": "linear",        // srgb | linear | hdr | auto
  "folder": "Characters/Sponza", // optional UI folder override (else derived from path)
  "name": "sponza_normal",       // optional display name (else derived from filename)
  "importOptions": { }           // optional; for re-deriving a standalone import
}
```

Colorspace becomes first-class via `AssetEntry.colorspace` (reserved in phase 03):
- **Embedded textures:** the `STEX` chunk's `flags` carry colorspace (set at bake from
  `ImportOptions::colorspaceFor(role)`, phase 04); the scan reads it from the prefix metadata — no sidecar.
- **Standalone textures:** colorspace comes from the `.smeta`; `.hdr` extension implies `hdr`; a missing
  sidecar defaults to `auto` and the scan **mints and writes** a `.smeta` (guessing sRGB for color-ish,
  with a warning that the guess may be wrong for data maps).

`AssetEntry.linear` is kept for back-compat but derived from `colorspace` (`linear = colorspace != Srgb`);
new code reads `colorspace`.

## Scan integration

`scanAssets` (phase 09) for a standalone file: read its `.smeta` if present (identity + colorspace + folder
+ name); if absent, mint a `Uuid`, infer type from extension, infer colorspace (extension/heuristic), write
the `.smeta`, and warn. A `.smeta` whose `id` collides with another entry, or whose sibling file is gone, is
reported (broken — phase 14/15), not silently dropped.

## Files to touch

- `engine/source/saffron/assets/assets.cppm` — `.smeta` read/write (`readSmeta`/`writeSmeta`); integrate
  into `scanAssets`; derive `AssetEntry.colorspace`/`linear`; route texture upload through `colorspace`
  (the `uploadTexture` sRGB-vs-Unorm choice) instead of the call-site `srgb` bool.
- `engine/source/saffron/scene/scene.cppm` — finalize `Colorspace` usage on `AssetEntry`.

## Steps

1. Define the `.smeta` schema + `readSmeta`/`writeSmeta`.
2. Add colorspace to the texture-upload path: `uploadTexture` picks `eR8G8B8A8Srgb` vs `Unorm` vs the
   float path from `AssetEntry.colorspace`, replacing the scattered `registerTextureBytes(srgb=…)` bools.
3. Integrate `.smeta` into `scanAssets`: present → use it; absent → mint+infer+write+warn.
4. Self-test + e2e: drop a raw `foo.png` into `assets/textures/`, `scan-assets` → a `.smeta` appears with a
   stable id, the texture is in the catalog with a colorspace, and a reload preserves the same id; flip the
   `.smeta` colorspace and assert the upload format follows.

## Gate / done

- `make engine` clean; the dropped-`.png` e2e proves stable identity + colorspace from `.smeta`; embedded
  textures get colorspace from chunk flags (no sidecar); `make e2e` + contract test pass;
  `make prepare-for-commit` clean.

## Risks

- **Wrong-colorspace silent mis-render:** the dangerous default. For an ambiguous standalone texture, the
  guess can be wrong (data map as sRGB). Warn loudly on a minted-by-guess `.smeta`, and prefer the
  container's authoritative flag whenever the texture is embedded (the common path).
- **Sidecar drift / loss:** a `.smeta` that loses sync with its file (edited externally, moved without the
  sidecar) must be detected, not trusted blindly — re-derive on mismatch and warn.
- **Engine files don't get `.smeta`:** keep the sidecar strictly for foreign/headerless files; writing
  `.smeta` for `.smodel`/extracted files would create the two-sources-of-truth problem the plan avoids.
- **`linear` field debt:** leaving `AssetEntry.linear` as a derived shadow risks code reading the stale
  field; grep callers and route them to `colorspace`.
