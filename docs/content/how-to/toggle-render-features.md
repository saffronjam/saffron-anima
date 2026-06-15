+++
title = 'Toggle render features'
weight = 7
math = false
+++

# Toggle render features

Switch renderer features on and off at runtime with the `sa set-*` commands.

## Steps

Each toggle takes `0|1` (or `off`/`on`); a few take a mode string instead. With the editor running:

```sh
sa set-aa msaa4              # off | fxaa | taa | msaa2 | msaa4 | msaa8
sa set-clustered 1          # clustered light culling vs brute-force loop
sa set-depth-prepass 1      # vertex-only depth pre-pass before the scene pass
```

The other toggles share the same shape:

```sh
sa set-shadows 1            # directional shadow map
sa set-ibl 1                # image-based ambient vs flat ambient
sa set-ssao 1               # screen-space ambient occlusion (GTAO)
sa set-exposure 1.5         # tonemap exposure in EV stops (engine raises 2^EV)
sa set-gi ddgi              # off | ddgi (probe global illumination)
```

`set-clustered 0` falls back to a brute-force per-pixel light loop (pixel-identical to the clustered path).

## Verify

- Read the live flags: `sa render-stats` reports `aa`, `clustered`, `depthPrepass`, `shadows`, `ibl`, `ssao`, `exposureEv`, and more.
- Capture before and after for a visual diff:
  ```sh
  sa set-aa off   && sa screenshot viewport /tmp/aa-off.png
  sa set-aa msaa4 && sa screenshot viewport /tmp/aa-msaa4.png
  ```

## In the code

| What | File | Symbols |
|---|---|---|
| AA / clustered / depth-prepass / exposure | `control_commands_render.cpp` | `set-aa`, `set-clustered`, `set-depth-prepass`, `set-exposure` |
| Shadows / IBL / SSAO / GI | `control_commands_render.cpp` | `set-shadows`, `set-ibl`, `set-ssao`, `set-gi` |
| Live flag readout | `control_commands_render.cpp` | `render-stats` (`aaMode`, `clusteredEnabled`, …) |

## Related

- [MSAA](../../explanations/anti-aliasing/msaa/) and [FXAA](../../explanations/anti-aliasing/fxaa/)
- [Clustered forward lighting](../../explanations/lighting-and-brdf/clustered-forward/)
- [Tonemapping and exposure](../../explanations/screen-space-and-post/tonemap-and-exposure/)
