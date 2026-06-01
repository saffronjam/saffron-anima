# Phase 1: Scene Environment Data

**Status:** NOT STARTED

## Goal

Add durable scene-level environment state without changing rendering behavior yet. This phase should be mostly data model, serialization, and editor control plumbing.

> **Post-lighting note.** This phase is unchanged by the lighting roadmap — `Scene`
> (`scene.cppm`) still holds only `entt::registry registry` + `const AssetCatalog* catalog`,
> `SceneVersion` is still `1`, and `sceneFromJson` **fails** on a version mismatch (so the
> 1→2 bump *must* ship a migration that accepts v1 and fills defaults). `ProjectVersion`
> stays `1` (the project wraps the scene; only the nested scene version bumps). Grounded
> anchors: `Scene` at `scene.cppm:198-202`, `SceneVersion` at `:335`, `sceneToJson`/
> `sceneFromJson` at `:465-532`, glm vec3/vec4 JSON helpers at `:311-331`, the
> `assetTypeName`/`assetTypeFromName` enum-string pattern at `assets.cppm:49-61`. Default
> `ambientColor`/`ambientIntensity` so an existing (IBL-on) project looks unchanged after
> migration. Control commands that edit the environment must mutate `Scene.environment`
> (the per-frame source of truth `renderScene` re-reads), **not** renderer state directly.

## Data Model

Add to `engine/source/saffron/scene/scene.cppm`:

```cpp
enum class SkyMode
{
    Color,
    Texture,
    ProceduralAtmosphere,
};

struct SceneEnvironment
{
    SkyMode skyMode = SkyMode::Color;
    glm::vec3 clearColor{ 0.05f, 0.06f, 0.08f };
    Uuid skyTexture;
    f32 skyIntensity = 1.0f;
    f32 skyRotation = 0.0f;
    f32 exposure = 1.0f;
    bool visible = true;
    bool useSkyForAmbient = true;
    glm::vec3 ambientColor{ 1.0f };
    f32 ambientIntensity = 0.15f;
};
```

Then extend:

```cpp
struct Scene
{
    entt::registry registry;
    SceneEnvironment environment;
    const AssetCatalog* catalog = nullptr;
};
```

## Serialization

Bump `SceneVersion` from 1 to 2.

Current project format stores:

```json
{
  "version": 1,
  "scene": {
    "version": 1,
    "entities": []
  }
}
```

New scene format:

```json
{
  "version": 2,
  "environment": {
    "skyMode": "color",
    "clearColor": { "x": 0.05, "y": 0.06, "z": 0.08 },
    "skyTexture": 0,
    "skyIntensity": 1.0,
    "skyRotation": 0.0,
    "exposure": 1.0,
    "visible": true,
    "useSkyForAmbient": true,
    "ambientColor": { "x": 1.0, "y": 1.0, "z": 1.0 },
    "ambientIntensity": 0.15
  },
  "entities": []
}
```

Migration rule:

- If scene version is 1, keep entities as-is and use default `SceneEnvironment`.
- If `environment` is missing, use defaults.
- Unknown `skyMode` should fall back to `Color` and log a warning.

## Editor UI

Add a scene/environment settings panel rather than a hierarchy entity.

Minimum location options:

- New `Environment` panel.
- Or a section in an existing editor settings panel if one appears before implementation.

Controls:

- Combo: `Sky Mode`.
- Color editor: `Clear Color`.
- Asset picker: `Sky Texture`, visible for texture mode.
- Drag float: `Intensity`.
- Drag float: `Rotation`.
- Drag float: `Exposure`.
- Checkbox: `Visible`.
- Checkbox: `Use Sky For Ambient`.
- Color editor: `Ambient Color`.
- Drag float: `Ambient Intensity`.

## Asset Type

Do not add a new asset type yet unless needed. The existing `AssetType::Texture` can be used for LDR sky panoramas. Add metadata later when HDR/cubemap import arrives.

## Implementation Steps

1. Add `SkyMode` and `SceneEnvironment`.
2. Add JSON helpers for sky mode string conversion.
3. Extend `sceneToJson` and `sceneFromJson`.
4. Add version migration for existing version 1 projects.
5. Add editor UI for scene environment fields.
6. Keep renderer behavior unchanged except optionally mapping `clearColor` to `renderer.clearColor`.

## Tests And Verification

- Load existing `project.json`; it should not fail due to scene version 1.
- Save and reload a project; environment values should round-trip.
- Confirm no sky settings appear as entities in hierarchy.
- Confirm existing render output remains unchanged when environment is default.

