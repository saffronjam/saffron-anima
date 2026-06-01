# Phase 1: PBR BRDF + HDR Offscreen

**Status:** COMPLETED
<!-- Flip to COMPLETED when the "Done when" checklist passes, validation-clean. Delete this file only after COMPLETED + merged. -->

<!--
Done 2026-05-31, validation-clean (headless under a headless `weston` Wayland display
in the toolbox; the host session has no displays). Implementation notes:
- OffscreenColorFormat -> eR16G16B16A16Sfloat (renderer_types.cppm). MSAA + FXAA scratch
  reuse it. tonemap.slang + fxaa.slang storage-image format -> rgba16f. PNG capture
  (writeBufferToPng) unpacks half floats; formatPixelBytes() sizes the readback buffer.
- BRDF: Cook-Torrance GGX (distributionGGX + height-correlated visibilitySmithGGX +
  fresnelSchlick) in mesh.slang, shared by the directional + clustered punctual lights.
  roughness clamped to [0.045,1]. Flat scalar ambient kept as the indirect placeholder
  (phase 2 IBL replaces it); emissive added on top. Unlit permutation = albedo+emissive.
- Eye position: added to LightUbo (set 1) + LightGlobals; renderScene passes the
  inverse-view translation through setSceneLighting.
- MaterialComponent gained metallic/roughness/emissive/emissiveStrength (serialized +
  inspector sliders + glTF import of metallic/roughness/emissive factors). Threaded into
  InstanceData (pbr + emissive float4s; std430 16-byte aligned) via DrawItem.
- Tonemap is now MANDATORY (added in beginFrameGraph after scene+AA, before ui), with an
  exposure push constant (exp2(exposureEv)). Removed the opt-in set-postprocess path.
- se: set-exposure {ev}; set-material extended (metallic/roughness/emissive/emissiveStrength);
  render-stats reports hdr + exposureEv. Removed set-postprocess.
- Verified: 10-cube metallic/roughness grid renders with roughness-dependent highlights;
  metal row tints highlights by base color; exposure sweep visibly darkens/brightens;
  clustered on-vs-off screenshots BYTE-IDENTICAL (A/B contract preserved); batching intact
  (10 instances -> 1 draw call). Sphere primitive deferred (cube grid was sufficient to
  validate the BRDF; not load-bearing for the phase).
-->


## Goal

Replace the flat Lambert `N·L` + scalar-ambient shading with a Cook-Torrance
metallic/roughness BRDF, applied identically to the directional light and the
clustered punctual lights, and switch the offscreen target to HDR float so radiance
survives to the tonemap pass. This is the **load-bearing prerequisite** for every
later lighting tier — shadows, IBL, and all GI feed radiance *into* this BRDF, and the
current scalar ambient *is* the only "GI" today. It is also the biggest perceptual
jump per unit effort (flat-lit cubes become real materials) and runs at full speed on
llvmpipe (pure ALU).

**Depends on:** nothing. Start here.

## Current state (verified)

The fragment shader `editor/assets/shaders/mesh.slang:149-187` is pure diffuse:

- Directional: `lit = color * intensity * max(dot(n,-lightDir),0) + ambient`
  (`mesh.slang:161-164`) — no view vector, no specular, no Fresnel.
- Punctual `punctual()` (`mesh.slang:107-128`): `color * intensity * N·L *
  attenuation * cone` — UE4-style inverse-square falloff, diffuse only.
- Final: `color = tex.rgb * baseColor.rgb * lit` (`mesh.slang:185`).
- Ambient is one scalar `globals.directionAmbient.w` (default `0.15`) added flat to
  RGB — the only indirect term.

Material today (`engine/source/saffron/scene/scene.cppm:53`):

```cpp
struct MaterialComponent {
    glm::vec4 baseColor{ 1.0f };
    Uuid albedoTexture;
    bool unlit = false;
};
```

The offscreen is LDR: `OffscreenColorFormat = vk::Format::eR8G8B8A8Unorm`
(`renderer_types.cppm:33`). The PSO bakes this format
(`renderer.cppm:849` `renderingInfo.setColorAttachmentFormats(OffscreenColorFormat)`).
The mesh PSO push constant is only `viewProj` (mat4), vertex stage
(`renderer.cppm:852-855`) — there is **no eye position in the fragment shader**, which
a specular BRDF needs.

## Step 1 — HDR float offscreen

Change `OffscreenColorFormat` to `vk::Format::eR16G16B16A16Sfloat`
(`renderer_types.cppm:33`). Verify it works as both a color attachment and a storage
image (the tonemap/FXAA compute passes write the offscreen) — `R16G16B16A16_SFLOAT`
storage is widely supported and supported on llvmpipe; if a target ever lacks it, fall
back per-feature, but assume yes here.

Consequences, all already handled by existing patterns:

- The mesh PSO picks up the new format automatically (`renderer.cppm:849`).
- The existing Reinhard tonemap compute pass (`addTonemapPass`, `renderer.cppm:619`,
  `shaders/tonemap.slang`) already terminates HDR→LDR. **Make tonemap mandatory** when
  HDR is on (today it is opt-in via `usePostProcess`): an HDR offscreen sampled
  directly by ImGui without tonemapping will look wrong. Either force the tonemap pass
  in `beginFrameGraph`/`endFrame`, or have ImGui sample through it.
- Rebuild the PSO cache after the format change. Mirror `setAa`
  (`renderer.cppm:1378-1407`): `waitGpuIdle` → recreate targets → `pipelines.cache.clear()`
  → rebuild `depthPrepass`. Recreating the offscreen `Image` already bumps
  `targets.generation` (`renderer_types.cppm:577`) which refreshes the ImGui descriptor.

## Step 2 — eye position into the fragment shader

A specular BRDF needs `V = normalize(eyePos - worldPos)`. Two options; prefer (a):

- **(a)** Add `glm::vec4 eyePosition` to the per-frame light UBO (`LightUbo`, written
  in `setSceneLighting`, `renderer.cppm:1319`). `renderScene` already has the camera
  view (`assets.cppm:413`); pass `cameraWorldPosition` through `setSceneLighting`
  (extend its signature, or add a small `setCameraPosition`). The fragment reads it
  from set 1.
- **(b)** Reconstruct from `clusterParams.view` (`mesh.slang:65-73`): eye =
  `inverse(view)[3].xyz`. Avoids touching the UBO but adds a matrix inverse per
  fragment — don't.

## Step 3 — extend the material model

Extend `MaterialComponent` (`scene.cppm:53`):

```cpp
struct MaterialComponent {
    glm::vec4 baseColor{ 1.0f };
    Uuid albedoTexture;
    f32 metallic = 0.0f;
    f32 roughness = 1.0f;
    Uuid metallicRoughnessTexture;  // optional; glTF packs MR in one texture (G=rough, B=metal)
    Uuid normalTexture;             // optional; reserved — needs tangents (see Notes)
    bool unlit = false;
};
```

- Register the new fields for JSON + the inspector. `MaterialComponent` is registered
  at `scene.cppm:576`-ish via `registerComponent` — extend its serialize/inspect
  closures (the struct-of-closures itable; see the `ecs-architecture` memory).
- Carry metallic/roughness + the bindless MR/normal slot indices into `InstanceData`
  (`renderer_types.cppm:640`). Today it holds `model`, `normalMatrix`, `baseColor`,
  `uvec4 texture` (`.x` = albedo bindless index). Add a second `uvec4` for
  `mrIndex`/`normalIndex` and pack `metallic`/`roughness` into spare lanes (keep
  std430 16-byte alignment — the struct comment at `:639` is explicit about this).
- `submitDrawList` (`renderer.cppm` builds `InstanceData` from `DrawItem`) and the
  `DrawItem` build in `renderScene` (`assets.cppm:474-495`) thread the new fields.
- Import: extend the cgltf path (`engine/source/saffron/geometry/geometry.cppm`) to
  read `pbrMetallicRoughness` base-color-factor, metallic-factor, roughness-factor and
  the MR texture; default metallic 0 / roughness 1 when absent (so existing assets
  look matte, not black).

## Step 4 — the BRDF in `mesh.slang`

Rewrite the lit branch (`mesh.slang:161-185`) and `punctual()` (`:107-128`) to
Cook-Torrance metallic/roughness. Keep the cluster loop, instance SSBO, bindless
albedo, and spot/attenuation math unchanged — only the per-light *term* changes from
"diffuse" to "BRDF × radiance".

```hlsl
static const float PI = 3.14159265;

float3 fresnelSchlick(float cosT, float3 F0) { return F0 + (1 - F0) * pow(1 - cosT, 5); }

float distributionGGX(float ndoth, float a) {        // a = roughness^2
    float a2 = a * a;
    float d = ndoth * ndoth * (a2 - 1) + 1;
    return a2 / max(PI * d * d, 1e-5);
}

float visibilitySmithGGX(float ndotv, float ndotl, float a) {  // height-correlated Smith
    float a2 = a * a;
    float v = ndotl * sqrt(ndotv * ndotv * (1 - a2) + a2);
    float l = ndotv * sqrt(ndotl * ndotl * (1 - a2) + a2);
    return 0.5 / max(v + l, 1e-5);
}

// radiance = light color * intensity * attenuation * cone (the existing punctual scalar)
float3 brdf(float3 n, float3 v, float3 l, float3 albedo, float metallic, float roughness, float3 radiance) {
    float3 h = normalize(v + l);
    float ndotl = max(dot(n, l), 0);
    float ndotv = max(dot(n, v), 1e-4);
    float ndoth = max(dot(n, h), 0);
    float a = roughness * roughness;
    float3 F0 = lerp(float3(0.04), albedo, metallic);
    float3 F = fresnelSchlick(max(dot(h, v), 0), F0);
    float D = distributionGGX(ndoth, a);
    float Vis = visibilitySmithGGX(ndotv, ndotl, a);
    float3 spec = D * Vis * F;
    float3 kd = (1 - F) * (1 - metallic);
    float3 diff = kd * albedo / PI;
    return (diff + spec) * radiance * ndotl;
}
```

- Directional: `radiance = color * intensity`, `l = -normalize(lightDir)`.
- Punctual: change `punctual()` to return `brdf(...)` with
  `radiance = lt.colorIntensity.rgb * lt.colorIntensity.a * attenuation * cone` and
  `l = normalize(toLight)` — the attenuation/cone/range cutoff math is unchanged.
- Ambient: keep `albedo * ambient` for now as a flat placeholder; **phase 2 (IBL)
  replaces it** with `irradiance(n)·albedo + specularIBL`. Note in the shader that the
  scalar ambient is temporary.
- `albedo` is `tex.rgb * baseColor.rgb` (already computed). Read `metallic`/`roughness`
  from the instance buffer (sample the MR texture if `mrIndex != 0`).

## Step 5 — PSO variant + control command

- The übershader stays one Slang file. PBR is the *default* lit path; `kUnlit`
  (`mesh.slang:147`, spec constant id 0) is unchanged. No new permutation is strictly
  required for v1 (PBR replaces the old lit math in place). If you want a runtime
  A/B against the old Lambert path, add a second spec constant `kLegacyLambert`
  (id 1) and key it in `requestMeshPipeline` (`renderer.cppm:897-917`, which currently
  keys on `shader` + `"|unlit"`).
- Add `se set-material` fields for `metallic`/`roughness` (extend the existing
  command, `control.cppm:489`). Optionally `se set-pbr {0|1}` if you keep the A/B
  spec constant. Follow the `set-clustered` registration template
  (`control.cppm:231`).

## Done when

- [ ] `OffscreenColorFormat` is `eR16G16B16A16Sfloat`; tonemap runs so the Viewport
      looks correct (not blown out); PSO cache rebuilt cleanly.
- [ ] `MaterialComponent` has metallic/roughness (+ optional MR texture), serialized,
      inspectable, importable from glTF, threaded through `InstanceData`.
- [ ] `mesh.slang` evaluates Cook-Torrance GGX for the directional **and** clustered
      punctual lights; spot cone + range cutoff + attenuation still correct.
- [ ] A dielectric sphere shows a roughness-dependent specular highlight; a metal
      sphere tints its highlight by base color and has near-zero diffuse.
- [ ] Validation-clean in the toolbox; `SAFFRON_CAPTURE` PNG verified; `se` material
      fields work; the clustered A/B (`set-clustered 0`) still matches.

## Notes / risks

- **Energy compensation**: rough metals look too dark without a multiscatter term.
  Defer it as polish — keep phase 1 contained.
- **Normal mapping** needs per-vertex tangents, which the `Vertex`
  (`geometry.cppm`) does not carry yet, and the mesh `.smesh` format is versioned —
  bumping it is its own change. Reserve `normalTexture` but implement normal mapping as
  a follow-up (it pairs naturally with phase 2/4 G-buffer work).
- **Back-face culling** is currently off (`renderer.cppm:825`, "enable Back once
  winding is verified"); leaving it off is fine for PBR but enabling it later helps
  shadow acne in phase 3.
- Coordinate the **RGB ambient** change with `plans/skybox/phase-3-lighting-integration.md`
  — both want to replace the scalar ambient; do it once.
