# Phase 4: HDR Textures

**Status:** COMPLETED

<!--
COMPLETED 2026-06-01, validation-clean. .hdr (Radiance) panoramas now decode + upload as true
linear half-float textures. Implemented exactly per the Implementation Steps:
- geometry.cppm: DecodedImageFloat + decodeImageHdr / decodeImageFromMemoryHdr (stbi_loadf /
  stbi_loadf_from_memory, STBI_rgb_alpha, 4*w*h floats).
- renderer_textures.cpp: uploadTextureFloat (near-copy of uploadTexture) pinning
  eR16G16B16A16Sfloat, with a static floatToHalf IEEE binary16 narrow (round-to-nearest-even,
  subnormal flush, >65504 -> inf, nan preserved) into a std::vector<u16> staging buffer; bindless
  write + GpuTexture fill reused verbatim. Declared beside uploadTexture in renderer_types.cppm.
- scene.cppm: AssetEntry.hdr (bool, default false); NO ProjectVersion/SceneVersion bump
  (read with jsonBoolOr default false, so old projects round-trip).
- assets.cppm: catalogToJson/FromJson serialize hdr; registerHdrTextureBytes; importTexture
  routes a case-insensitive .hdr; loadTextureAsset branches on entry->hdr. Sky wiring + sky.slang
  unchanged; no new shader, no reconfigure.
Verified headless (weston): se import-texture <hdr> returns an id with no decode error; the catalog
entry persists hdr=true in project.json (LDR entries hdr=false); validation-clean; clean exit (no
VMA leak). Commit: see git log (skybox phase 4).
-->


## Goal

Decode and upload `.hdr` (Radiance RGBE) panoramas as true linear float textures, so a
`SkyMode::Texture` sky (and the phase-5 user-IBL source) carries real HDR radiance instead
of the clamped LDR sRGB RGBA8 the current import path forces. This is a self-contained import
+ upload change: it adds a float decode path, a float upload path, a per-asset color-space
flag in the catalog, and routes `.hdr` assets through them. The visible-sky shader, the sky
pass, the tonemap pass, and the bindless descriptor layout are all **unchanged** — the only
thing that changes is the pixel format and bit depth of one texture slot.

## Post-lighting reality / Current Engine Fit

The texture pipeline is 8-bit-only end to end, but every other piece HDR needs is already in
place. Grounded anchors (verified against the current tree):

- **Decode is hardcoded to 8-bit RGBA.** `decodeImage` (`geometry.cppm:568-584`) and
  `decodeImageFromMemory` (`geometry.cppm:586-603`) call `stbi_load` / `stbi_load_from_memory`
  with `STBI_rgb_alpha`, and `DecodedImage` (`geometry.cppm:71-76`) holds only
  `std::vector<u8> rgba`. `stb_image.h` is already included (`geometry.cppm:7`), so
  `stbi_loadf` is available with no new dependency.
- **Upload assumes 4 bytes/pixel and picks only 8-bit formats.** `uploadTexture`
  (`renderer_textures.cpp:87`, declared at `renderer_types.cppm:1036`) computes
  `bytes = width*height*4` (`:93`) and selects `srgb ? eR8G8B8A8Srgb : eR8G8B8A8Unorm`
  (`:111`). Everything downstream of the format pick — image create, staging copy, layout
  transitions (`:148-159`), view create (`:170-175`), bindless write (`:182-185`) — is
  format-agnostic.
- **The bindless array is already format-agnostic.** Set 0 binding 0 is a runtime-sized
  `eCombinedImageSampler` array (`renderer_detail.cppm:2356-2362`, capacity
  `MaxBindlessTextures = 1024` from `renderer_types.cppm:44`) with `ePartiallyBound |
  eUpdateAfterBind`. `writeBindlessTexture` (`renderer_detail.cppm:2182-2192`) writes a
  `DescriptorImageInfo` with the texture's own view + the shared `linearSampler` at
  `dstArrayElement = index` (`:2188`); it never inspects the format. A `eR16G16B16A16Sfloat`
  view drops into the same array beside the `eR8G8B8A8Srgb` albedo views with **no layout,
  pool, or descriptor-type change** — confirmed: the descriptor type is the only pinned
  property, and the binding's `stageFlags` is `eFragment` (`:2360`), which is exactly where
  both the mesh shader and `sky.slang` sample it.
- **`GpuTexture.format` is already per-texture** (`renderer_types.cppm:281`), stored from
  whatever `uploadTexture` chose, used only for view creation / bookkeeping. No shader
  binding reads it. Slots are allocated linearly via `Descriptors::nextBindlessIndex`
  (`renderer_types.cppm:582`, bumped at `renderer_textures.cpp:184`) and never reclaimed —
  fine for HDR.
- **The sky shader already samples linear and the offscreen is already HDR.** `sky.slang`
  Texture mode (`editor/assets/shaders/sky.slang:64-70`) does
  `albedoTextures[NonUniformResourceIndex(index)].SampleLevel(uv, 0).rgb * intensity` and
  returns linear HDR; `OffscreenColorFormat` is `eR16G16B16A16Sfloat` and the mandatory
  tonemap pass maps to display (per phase 2). With an LDR sRGB slot the sampler hardware
  linearizes sRGB→linear and clamps at 1.0; with an rgba16f slot the same sample returns the
  real >1.0 radiance. **No shader edit, no second clear, no second tonemap.**
- **Catalog has no color-space metadata.** `AssetEntry` (`scene.cppm:118-124`) is
  `{ id, name, type, path }`; `assetTypeFromName` knows only `mesh|texture|other`
  (`assets.cppm:56-61`); `catalogFromJson` (`assets.cppm:74-98`) reads each field with a
  default. So on reload there is no way to tell an `.hdr` slot from a `.png` slot except the
  file extension — we add an explicit flag rather than relying on the extension at every site.
- **`loadTextureAsset` hardcodes sRGB.** `loadTextureAsset` (`assets.cppm:269-298`) decodes
  with `decodeImage` (`:281`) and uploads with `uploadTexture(..., /*srgb=*/true)` (`:284`).
  `registerTextureBytes` (`assets.cppm:205-239`) does the same on import (`:214`).
  `importTexture` (`assets.cppm:242-264`) infers only the file extension for the on-disk copy.
- **Import is reachable from the CLI + editor today.** `se import-texture {path}`
  (`control_commands_asset.cpp:47-61`) calls `importTexture`; the Environment panel's
  `drawAssetPicker(ctx.scene, AssetType::Texture, "Sky Texture", ...)`
  (`editor_panels.cpp:209-212`) shows any `Texture` catalog entry. Both work for `.hdr` once
  import routes it correctly — no new command or picker is required.

Net: phase 4 is a decode + upload + one-catalog-field change. The renderer-side machinery
(bindless array, HDR offscreen, tonemap, sky shader) is all reused as-is.

## Data Model

### Decoded float image (`geometry.cppm`)

Add beside `DecodedImage` (`geometry.cppm:71-76`):

```cpp
// Decoded linear float RGBA, tightly packed (width*height*4 floats). From .hdr/.exr-class
// sources; values are real radiance (may exceed 1.0), never sRGB-encoded.
struct DecodedImageFloat
{
    std::vector<f32> rgba;
    u32 width = 0;
    u32 height = 0;
};
```

Declare beside the existing decode decls (`geometry.cppm:83-84`):

```cpp
auto decodeImageHdr(const std::string& path) -> Result<DecodedImageFloat>;
auto decodeImageFromMemoryHdr(const std::vector<u8>& encoded) -> Result<DecodedImageFloat>;
```

### Catalog color-space flag (`scene.cppm`)

Add one optional field to `AssetEntry` (`scene.cppm:118-124`). A bool keeps the on-disk
format minimal and is sufficient for phase 4 (HDR vs sRGB is the only distinction); a string
class is overkill until normal/data maps arrive.

```cpp
struct AssetEntry
{
    Uuid id;
    std::string name;
    AssetType type = AssetType::Mesh;
    std::string path;     // relative to the asset root
    bool hdr = false;     // texture: decode as linear float (.hdr); else sRGB RGBA8
};
```

`hdr` is meaningful only for `AssetType::Texture` entries; ignore it for meshes.

> **No `ProjectVersion` bump.** `loadProject` rejects any version `!= ProjectVersion`
> (`assets.cppm:148`, check at `:190-194`), so a bump would *break* existing projects. The
> field is purely additive: `catalogToJson` writes it, `catalogFromJson` defaults it to
> `false` for any old file via the same `jsonU64Or`/`jsonStringOr`-with-default pattern at
> `assets.cppm:88-92`. An existing v1 project with only LDR textures round-trips unchanged.
> `SceneVersion` (`scene.cppm:424`) is likewise untouched — the catalog lives in the project
> wrapper (`assets.cppm:156`), not the scene block.

## Renderer API

Add a float overload of `uploadTexture` rather than threading a format parameter through the
existing 8-bit signature — the stride math, the channel assumptions, and the call sites differ
enough that an overload is clearer and leaves every current caller untouched.

Declare beside the existing `uploadTexture` (`renderer_types.cppm:1036`):

```cpp
// Uploads tightly-packed linear float RGBA (width*height*4 floats) as a half-float
// (eR16G16B16A16Sfloat) sampled texture in the bindless array. For HDR panoramas /
// environment sources; no sRGB encoding. The driver narrows f32 -> f16 on copy via the
// half-float image format.
auto uploadTextureFloat(Renderer& renderer, const f32* rgba, u32 width, u32 height)
    -> Result<Ref<GpuTexture>>;
```

Implement it in `renderer_textures.cpp` (reuse the existing TU — no `CMakeLists` edit) as a
near-copy of `uploadTexture` (`:87-197`) with these deltas:

- `bytes = width*height*4*sizeof(f32)` for the staging buffer (16 B/pixel).
- `const vk::Format format = vk::Format::eR16G16B16A16Sfloat;` replacing the line-111 ternary.
- **Format choice: `eR16G16B16A16Sfloat`.** Half the bandwidth/footprint of `eR32...Sfloat`
  and ample dynamic range for sky radiance; this is the standard real-time HDR environment
  format. The staging buffer stays full `f32` (what `stbi_loadf` produces); the
  GPU image format is `f16`, so `vkCmdCopyBufferToImage` cannot reinterpret 32→16 bit. Two
  options, pick one in the implementation step:
  - **Preferred:** narrow on the CPU into a `std::vector<u16>` half-float buffer before the
    staging copy (a small `f32`→half helper local to the TU), so the staging buffer matches
    the `f16` image and `bytes = width*height*4*sizeof(u16)`. Mirrors how the existing path
    keeps staging and image formats identical.
  - Or stage as `eR32G32B32A32Sfloat` and blit/copy to an `f16` image (extra image + barrier);
    avoid — more code for no benefit at sky resolutions.
- The rest (image create with `eTransferDst | eSampled`, the two `transitionImage` calls, the
  view with `viewType = e2D` + `format`, the `writeBindlessTexture(renderer, *view, index)`
  slot claim, the `GpuTexture` fill with `format = eR16G16B16A16Sfloat`) is identical to the
  8-bit path and is **reused verbatim** — the bindless write does not care about the format.

> No descriptor-layout work. `writeBindlessTexture` (`renderer_detail.cppm:2182`) writes the
> rgba16f view into the same set 0 array as every albedo texture; the layout, pool, and the
> `eFragment` stage flag (`renderer_detail.cppm:2360`) already cover the sky's fragment-stage
> sample. This is the verified answer to "second float array or shared array": **shared array,
> single descriptor write, no new binding.**

## Asset Loading

Wire the float path through the asset layer in `assets.cppm` (all in the existing module TU):

- **`registerTextureBytes` → add `registerHdrTextureBytes`** beside it (`assets.cppm:205-239`).
  Decode with `decodeImageFromMemoryHdr`, upload with `uploadTextureFloat`, write the encoded
  bytes to `textures/<uuid>.hdr`, and add the catalog entry with `hdr = true`. Reuse the
  uniqueName/`putAsset`/`textureRefByUuid` tail unchanged.
- **`importTexture` → detect `.hdr`** (`assets.cppm:242-264`). The extension is already
  extracted at `:257-262`; when it equals `"hdr"` (case-insensitive), call
  `registerHdrTextureBytes`, else the existing `registerTextureBytes`. This makes both
  `se import-texture some.hdr` and the editor import route correctly with no UI change.
- **`loadTextureAsset` → branch on the catalog flag** (`assets.cppm:269-298`). After resolving
  the `AssetEntry` (`:276`), if `entry->hdr`, decode with `decodeImageHdr` + upload with
  `uploadTextureFloat`; else the current `decodeImage` + `uploadTexture(..., true)` path. The
  negative-cache + cache-store logic (`:287-297`) is unchanged. This is the cross-process
  reload site that needs the persisted `hdr` flag — extension sniffing would also work but the
  flag is the source of truth and keeps `.hdr`-by-another-name correct.
- **Catalog serialization** (`assets.cppm:63-98`). In `catalogToJson` (`:63`) add
  `{ "hdr", entry.hdr }` to the per-entry object (`:68-69`); in `catalogFromJson` (`:74-98`)
  read `parsed.hdr = jsonU64Or(entry, "hdr", 0) != 0;` (or a bool reader) after the `path`
  line (`:92`). Default `false` preserves old projects.

The sky wiring needs **no change**: `renderScene` (`assets.cppm:646-657`) already calls
`loadTextureAsset(assets, renderer, env.skyTexture)` and passes `panorama->bindlessIndex` into
`SkyRenderSettings::textureIndex`. With the catalog flag set, that same call now returns an
rgba16f texture whose slot the sky shader samples as real HDR. `SceneEnvironment.skyTexture`
in `SkyMode::Texture` (`scene.cppm:211-223`) is unchanged.

## Shader Approach

None. `sky.slang` (`editor/assets/shaders/sky.slang`) is unchanged — Texture mode samples the
bindless slot (`:64-70`) and the existing tonemap handles display. No new `.slang` file, so no
CMake reconfigure for shaders. The only narrowing happens in `uploadTextureFloat`'s CPU
half-float conversion; the GPU sees a normal sampled `f16` texture.

## Render Graph Placement

None. HDR import touches no pass. The sky pass (phase 2) and the mandatory tonemap pass run
exactly as today; the only difference is the radiance the sky pass writes into the
`eR16G16B16A16Sfloat` scene color target is now real >1.0 HDR for Texture-mode skies, which
the tonemap maps for free.

## Implementation Steps

Prerequisites: phases 1-3 (shipped). No other dependency.

1. `geometry.cppm`: add `DecodedImageFloat` (beside `:71-76`) and the two `decodeImageHdr` /
   `decodeImageFromMemoryHdr` declarations (beside `:83-84`). Implement them with `stbi_loadf`
   / `stbi_loadf_from_memory` + `STBI_rgb_alpha` (forces 4 channels) just before the existing
   `decodeImage` bodies (`:568`, `:586`); free with `stbi_image_free`, fill width/height, copy
   `4*w*h` floats.
2. `renderer_textures.cpp`: add `uploadTextureFloat` (declared at `renderer_types.cppm:1036`
   beside `uploadTexture`). Copy the 8-bit body, swap the stride to half-float bytes, pin
   `format = eR16G16B16A16Sfloat`, add the f32→half narrowing before the staging copy, keep the
   bindless write + `GpuTexture` fill identical.
3. `scene.cppm`: add `bool hdr = false;` to `AssetEntry` (`:118-124`).
4. `assets.cppm`: serialize `hdr` in `catalogToJson` (`:63-69`) and read it in `catalogFromJson`
   (`:88-92`). No `ProjectVersion` bump.
5. `assets.cppm`: add `registerHdrTextureBytes` beside `registerTextureBytes` (`:205-239`);
   route `.hdr` in `importTexture` (`:242-264`); branch on `entry->hdr` in `loadTextureAsset`
   (`:269-298`).
6. Build + verify (below). No CMake reconfigure needed (no new `.slang`, reuse existing TUs).
7. Update `docs/` (the rendering/textures explanation page + its hub row) and confirm the
   `se import-texture` help still describes `.hdr` acceptance — treat both as part of "done".

## Verification

Build only in the toolbox, `-j1`:

```sh
toolbox run -c saffron-build bash -lc '
  cd /var/home/saffronjam/repos/SaffronEngine
  cmake --build build/debug -j1
'
```

Drive the running editor headless and capture, then diff:

- Place a small `.hdr` panorama under `editor/assets/` (or any path). Import it and assign it
  as the Texture-mode sky, then screenshot the viewport:
  ```sh
  toolbox run -c saffron-build bash -lc '
    cd /var/home/saffronjam/repos/SaffronEngine
    VAL=0 SAFFRON_EXIT_AFTER_FRAMES=0 ./build/debug/bin/SaffronEditor &
    sleep 2
    ID=$(./build/debug/bin/se import-texture editor/assets/test.hdr | sed "s/.*: //")
    ./build/debug/bin/se set-environment --json "{\"skyMode\":\"texture\",\"skyTexture\":$ID,\"visible\":true,\"skyIntensity\":1.0}"
    ./build/debug/bin/se screenshot viewport /tmp/sky_hdr.png
    ./build/debug/bin/se quit
  '
  ```
  Confirm `se import-texture` returns a texture id (no decode error) and `se list-assets` shows
  the new entry; confirm the saved `project.json` catalog entry carries `"hdr": true`.
- **HDR range survives.** Compare against the same image imported as LDR (a PNG export of the
  panorama, which clamps to 1.0). On a bright region (sun/sky), the rgba16f sample feeds the
  tonemap with >1.0 radiance, so the tonemapped pixels differ from the clamped LDR import.
  Diff the two viewport captures with numpy/PIL:
  ```sh
  python3 -c "
  from PIL import Image; import numpy as np
  a=np.asarray(Image.open('/tmp/sky_hdr.png').convert('RGB')).astype(int)
  b=np.asarray(Image.open('/tmp/sky_ldr.png').convert('RGB')).astype(int)
  print('max abs diff', np.abs(a-b).max(), 'changed px %', (np.abs(a-b).sum(2)>4).mean()*100)
  "
  ```
  Expect a non-trivial difference in the bright sky region (HDR is not clamped before tonemap),
  not pixel-identical.
- **No regression for LDR.** Import a PNG panorama the same way (`hdr=false` path), screenshot,
  and confirm it renders as before (matches a pre-change capture, max abs diff ~0).
- **Cross-process reload.** `se save-project /tmp/p.json`, relaunch, `se load-project /tmp/p.json`,
  re-screenshot the HDR sky; it must match the in-session HDR capture (the persisted `hdr` flag
  drives `loadTextureAsset` down the float path, not the sRGB path).
- **Validation-clean.** Run all of the above with `VAL=0` unset (validation on) and confirm no
  Vulkan validation errors — in particular no format/descriptor warnings from writing an
  rgba16f view into the set 0 array, and clean teardown (exit code 0, no VMA leak assertion).

Out of scope (later phases): equirect→cubemap re-bake of a user HDR into the IBL chain and
routing it into DDGI (phase 5); reflection probes (phase 6); procedural atmosphere LUTs
(phase 7); clouds + time of day (phase 8).
