# Skybox And Environment Plan

This folder tracks a thorough implementation plan for skyboxes and scene environment rendering in Saffron Engine.

## Recommendation

The sky should be modeled as scene environment state, not as a normal mesh entity.

Use entity components for things that have a meaningful transform, multiplicity, selection, picking, and local behavior: meshes, cameras, point lights, spot lights, and directional lights. A skybox is global frame state: it controls the background, ambient lighting input, future reflection probes, and eventually atmosphere. Treating the default sky as a giant unlit mesh would make the first image easy, but it would fight depth prepass, picking, batching stats, editor hierarchy semantics, lighting, and future image-based lighting.

The recommended shape is:

```cpp
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
    glm::vec3 ambientColor{ 0.15f };
    f32 ambientIntensity = 1.0f;
};

struct Scene
{
    entt::registry registry;
    SceneEnvironment environment;
    const AssetCatalog* catalog = nullptr;
};
```

Add specialized components later only where they represent actual placed objects or volumes, for example `SkyAtmosphereComponent`, `CloudLayerComponent`, or `ReflectionProbeComponent`.

## Current Engine Fit

Relevant current boundaries:

- `engine/source/saffron/scene/scene.cppm` owns components, scene serialization, and `Scene`.
- `engine/source/saffron/assets/assets.cppm::renderScene` resolves scene data into renderer inputs each frame.
- `engine/source/saffron/rendering/renderer.cppm::beginFrameGraph` builds the clustered light pass, optional depth prepass, scene pass, FXAA, app-authored post-process, and UI pass.
- `engine/source/saffron/rendering/renderer_types.cppm` owns renderer frame state and public renderer APIs.
- `editor/source/main.cpp` calls `renderScene` from the editor viewport path.

The integration point should be:

```text
Scene.environment
  -> renderScene(...)
  -> submitSky(...) / setSceneEnvironment(...)
  -> render graph sky pass + lighting constants
```

## External References

Unreal separates sky appearance, ambient/reflection lighting, and atmospheric simulation:

- Sky Light captures distant scene/sky or uses a cubemap for lighting/reflections:
  https://dev.epicgames.com/documentation/it-it/unreal-engine/sky-lights-in-unreal-engine
- Sky Atmosphere is a physically based atmosphere system:
  https://dev.epicgames.com/documentation/en-us/unreal-engine/sky-atmosphere?application_version=4.27
- HDRI Backdrop combines a visible backdrop, cubemap lighting, and projection workflow:
  https://dev.epicgames.com/documentation/unreal-engine/hdri-backdrop-visualization-tool-in-unreal-engine

Frostbite treats sky, atmosphere, and clouds as physically based lighting systems integrated with PBR and time of day:

- https://www.ea.com/news/physically-based-sky-atmosphere-and-cloud-rendering
- https://www.ea.com/news/moving-frostbite-to-pb

## Document Map

- `phase-0-research-and-architecture.md`: final architecture choices and rejected alternatives.
- `phase-1-scene-environment.md`: scene data model, serialization, and editor-facing settings.
- `phase-2-visible-skybox.md`: renderer API, shaders, pipeline, and render graph integration.
- `phase-3-lighting-integration.md`: ambient color, sky-driven lighting, and first IBL steps.
- `phase-4-atmosphere-and-ibl-roadmap.md`: longer-term physically based atmosphere, reflections, clouds, and probes.

