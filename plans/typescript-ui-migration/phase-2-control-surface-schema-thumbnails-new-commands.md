# Phase 2: Control surface hardening — new editor commands + thumbnail PNG readback + schema-first DTO catalog

**Status:** NOT STARTED

<!-- Flip to COMPLETED when the "Done when" checklist passes, validation-clean. Delete this file only after COMPLETED + merged. -->

## Goal

Close the control-command gaps that block a faithful TypeScript editor port, and stand up the
schema-first shared-type pipeline that the TS client (phase-3) consumes. Concretely:

1. Add the missing editor-shell / viewport / asset commands (`add-entity`, `copy-entity`,
   `deselect`, `get-selection`, `get-camera`/`set-camera`, `get-gizmo`/`set-gizmo`,
   `set-component-field`, `view-asset`, `get-thumbnail`, `dump-schema`).
2. Add a `selectionVersion` + `sceneVersion` so the phase-3 reconcile poll can diff cheaply
   instead of re-fetching the whole hierarchy + inspector every tick.
3. Add GPU→CPU thumbnail PNG readback (mesh + texture) so the TS asset browser can show real
   thumbnails over the socket (base64 PNG), modelled on the existing `captureViewport` readback.
4. Reconcile the gizmo into ONE state + ONE command family and add local-space support to the
   ImGuizmo path (it is hardcoded `ImGuizmo::WORLD` today).
5. Author hand-written JSON Schemas (draft 2020-12) for the whole protocol under
   `schemas/control/`, bootstrapped once from captured `se` output with quicktype, then
   hand-corrected; wire a `tools/check-control-schema` contract test validating live `se`
   responses against them; expose `dump-schema` as the forward seam for codegen.

This is engine + schema work only. It does **not** touch the React UI or the Rust bridge — those
land in phase-3+ and consume what this phase produces.

**Depends on:** phase-1 (the engine viewport bridge is forward-ported and SaffronEditor builds as
`editor-old/`). Nothing in this phase requires the native gizmo *render* (that is phase-4); the
gizmo *state* + `set-gizmo`/`get-gizmo` *commands* land here, with the ImGuizmo path as the
visible consumer in `editor-old`.

## Current state (verified)

### Control plane shape (the wire contract that the schemas must capture)
- Request/reply is newline-delimited JSON over a unix socket. `dispatch`
  (`control_server.cpp:213-236`) builds `{ id, ok, result|error }`; `ok:false` carries a string
  `error`. The envelope is fixed — every command result is the `result` field.
- Commands are registered via `registerCommand(reg, name, help, lambda)`
  (`control_server.cpp:32-38`); the lambda returns `Result<json>` (`command.cppm:41`). Three
  registrars run in render→scene→asset order (`control_server.cpp:140-145`).
- Params are loose: `positionalOr(params, name, idx)` reads `params[name]` else
  `params["args"][idx]` (`control_server.cpp:50-61`). The TS client always sends NAMED params.
- `resolveEntity` (`control_server.cpp:72-132`) accepts a uuid (number or numeric string) OR a
  name. `entityRef(scene, entity)` returns `{ id, name }` (`control_server.cpp:134-138`) — note
  `id` is the raw `u64` `IdComponent.id.value`.
- **Every id is a `u64`** (`core.cppm:51-53`, `struct Uuid { u64 value = 0; }`) emitted as a raw
  unsigned JSON number that can exceed `Number.MAX_SAFE_INTEGER`. The schema MUST type every
  id-bearing field as `string` and the TS side must string-preserve-parse (never `JSON.parse`
  into a JS `number`). This is the single biggest correctness hazard.
- There are **no named C++ DTO structs**. Every response is an ad-hoc `nlohmann::json` literal
  (e.g. `render-stats` at `control_commands_render.cpp:38-61` is a 21-field flat object built
  inline; `inspect` at `control_commands_scene.cpp:326-345` builds `{id,name,components:{...}}`).
  Components are type-erased `std::function` closures in the registry
  (`scene.cppm:435-437`: `serialize`/`deserialize`/`drawInspector`), authored in
  `editor_components.cpp:86-278`. So the schema is the source of truth; C++ is a *validated
  consumer* (the contract test), and `dump-schema` exposes the live shapes as a codegen seam.

### Existing commands (38) and what they already return
- Scene (`control_commands_scene.cpp:21-398`): `list-entities`, `list-components`,
  `create-entity`, `destroy-entity`, `add-component`, `remove-component`, `set-component`,
  `set-transform`, `set-material`, `set-light`, `select`, `pick`, `inspect`, `focus`,
  `get-environment`, `set-environment`.
- Render (`control_commands_render.cpp:16-296`): `ping`, `help`, `render-stats`, `set-aa`,
  `set-clustered`, `set-ibl`, `set-ssao`, `set-contact-shadows`, `set-ssgi`, `set-rt-shadows`,
  `set-restir`, `set-gi`, `set-shadows`, `set-exposure`, `set-depth-prepass`.
- Asset (`control_commands_asset.cpp:21-242`): `import-model`, `import-texture`, `list-assets`,
  `rename-asset`, `assign-asset`, `save-scene`, `load-scene`, `save-project`, `load-project`,
  `screenshot`, `quit`.
- Phase-1 added the 3 viewport commands (`viewport-native-info`, `attach-native-viewport`,
  `resize-native-viewport`).

### Command gaps (verified against the C++ editor surface)
- **No `add-entity`/spawn-preset.** The Create menu + Hierarchy "Add +" presets live only in
  ImGui (`editor_panels.cpp:23-62`): Empty (`createEntity`), Model (`ctx.onCreateCube`, wired in
  `editor_app.cppm:208-218` to `importModel(models/cube.gltf)` + `spawnModel`), Point/Spot/
  Directional Light, Camera. `create-entity` only makes an Empty.
- **No `copy-entity`.** Deep-copy lives only in `editor_panels.cpp:85-98` (for each registry row
  that `has` the source, `addDefault` on a fresh entity then `deserialize(serialize(src))`).
- **No `get-selection`.** Selection is `EditorContext.selected` + a `SubscriberList<Entity>
  onSelectionChanged` (`editor_context.cppm:43-44`); `setSelection` publishes it
  (`editor_context.cpp:16-20`). `select`/`pick` *set* it but nothing reads it back, and there is
  no version stamp — so a UI poll cannot tell whether the in-engine selection changed.
- **No `sceneVersion`.** `list-entities` returns the raw list every call; nothing tells a poll
  the scene changed.
- **No `deselect`.** `setSelection(ctx, Entity{ entt::null })` is the in-process call but no
  command exposes it.
- **No `get-camera`/`set-camera`.** `EditorCamera` (`editor_context.cppm:24-35`:
  `position`, `yaw`, `pitch`, `fov`, `nearPlane`, `farPlane`, `moveSpeed`, `lookSpeed`,
  `controlling`) is only mutated by `updateEditorCamera` + `focus`.
- **Gizmo is split + WORLD-only.** The ImGuizmo path uses `EditorContext.gizmoOp`
  (`editor_context.cppm:59`) and is **hardcoded `ImGuizmo::WORLD`** at `editor_gizmo.cpp:49`. The
  worktree's faf704d added a *separate* `NativeGizmoState{ mode, space, ... }`
  (`wt:editor_context.cppm:62-74`) plus demo commands `set-gizmo-mode` / `set-gizmo-space`
  (`wt:control_commands_scene.cpp:362-401`) that ONLY touch `nativeGizmo`. There must be ONE
  gizmo state and ONE command family — `set-gizmo`/`get-gizmo` — driving both the ImGuizmo and the
  native paths.
- **`click-viewport` is a demo, not real.** `wt:control_commands_scene.cpp:326-360` ray-picks then
  *toggles the hit's material baseColor* (yellow ↔ white). It must NOT be forward-ported as-is.
  `pick` (`control_commands_scene.cpp:304-324`) already does the real ray-pick + select; material
  edits go through `set-material`. So `click-viewport` is simply dropped.
- **No `set-component-field`.** `assign-asset` (`control_commands_asset.cpp:96-140`) only handles
  `slot ∈ {mesh, albedo}`. There is no generic "set field X of component Y on entity to asset Z",
  so drag-drop onto the Environment `skyTexture` picker or any other Uuid field has no command.
- **No `get-thumbnail`.** `renderMeshThumbnail` (`renderer_thumbnail.cpp:141-272`, declared
  `renderer_types.cppm:1045`) returns a `Ref<GpuTexture>` (a live GPU image for ImGui), NOT bytes.
  There is no PNG readback for an *asset* (only the offscreen-viewport `captureViewport`).

### The readback model to copy for `get-thumbnail`
- `captureViewport` (`renderer_capture.cpp:38-110`) is the canonical GPU→CPU→PNG path: `waitIdle`,
  `newHostCaptureBuffer` (`renderer_detail.cppm:925-940`, a `TRANSFER_DST` host-mapped VMA buffer),
  a one-off command buffer that calls `captureImageToBuffer` (`renderer_detail.cppm:944-964`:
  fromLayout→TransferSrc barrier, `copyImageToBuffer`, TransferSrc→toLayout barrier), submit +
  `waitIdle`, `vmaInvalidateAllocation`, then `writeBufferToPng`
  (`renderer_detail.cppm:978-1023`, BGRA/RGBA + RGBA16F→sRGB-clamped, writes via
  `stbi_write_png`), then `vmaDestroyBuffer`. `formatPixelBytes` (`renderer_detail.cppm:967-974`).
- `renderMeshThumbnail` already renders a framed mesh into an offscreen `size×size` color image
  in `renderer.swapchain.format` (8-bit BGRA/RGBA), leaving it in `eShaderReadOnlyOptimal`
  (`renderer_thumbnail.cpp:244-246`). For `get-thumbnail` we render the SAME image but read it
  back to a host buffer and PNG-encode to **memory** instead of taking ownership as a texture.
- Asset → GPU resolution already exists: `loadMeshAsset(assets, renderer, Uuid)`
  (`assets.cppm:346-...`) returns `Ref<GpuMesh>` (catalog-resolved + cached);
  `loadTextureAsset(assets, renderer, Uuid)` (`assets.cppm:269-298`) returns `Ref<GpuTexture>`;
  `findAsset(catalog, uuid)` resolves the `AssetEntry` (`type ∈ mesh|texture|other`).

### Component DTOs (the discriminated union keyed on component name)
From `editor_components.cpp` serialize/deserialize + `scene.cppm:28-103`:
- `Name` `{ name:string }` — **not removable** (`editor_components.cpp:89-100`).
- `Transform` `{ translation:Vec3, scale:Vec3, rotation:Vec3 }` — **not removable**;
  rotation is **Euler XYZ radians** on the wire (`editor_components.cpp:114-127`,
  `scene.cppm:42`; the UI converts to/from degrees at `editor_components.cpp:107-111`).
- `Mesh` `{ mesh:Uuid }` (`editor_components.cpp:135-138`).
- `Camera` `{ fov, near, far, primary:bool }` (`editor_components.cpp:152-164`) — note JSON keys
  `near`/`far`, not `nearPlane`/`farPlane`.
- `Material` `{ baseColor:Vec4, albedoTexture:Uuid, metallic, roughness, emissive:Vec3,
  emissiveStrength, unlit:bool }` (`editor_components.cpp:179-199`).
- `DirectionalLight` `{ direction:Vec3, color:Vec3, intensity, ambient }`
  (`editor_components.cpp:211-224`).
- `PointLight` `{ color:Vec3, intensity, range }` (`editor_components.cpp:235-246`).
- `SpotLight` `{ direction:Vec3, color:Vec3, intensity, range, innerAngle, outerAngle }`
  (`editor_components.cpp:260-276`) — angles in **degrees**.
- `Vec3` = `{x,y,z}`, `Vec4` = `{x,y,z,w}` (`scene.cppm:339-359`), floats.
- `Environment` (`environmentToJson`, `scene.cppm:381-395`):
  `{ skyMode:"color"|"texture"|"procedural", clearColor:Vec3, skyTexture:Uuid, skyIntensity,
  skyRotation, exposure, visible:bool, useSkyForAmbient:bool, ambientColor:Vec3,
  ambientIntensity }`.
- `RenderStats` (`control_commands_render.cpp:38-61`): `drawCalls/batches/instances/blasCount/
  pipelines` (u32), `clustered/depthPrepass/shadows/ibl/ssao/contactShadows/ssgi/ddgi/
  rtSupported/rtShadows/restir/hdr` (bool), `exposureEv` (f32), `aa` (enum string).
- `AssetEntry` (`control_commands_asset.cpp:69-70` / `assets.cppm:68-69`):
  `{ id:Uuid, name, type:"mesh"|"texture"|"other", path }`.

### Build / docs conventions to fold into "done"
- Build in the toolbox only, `-j1`: `toolbox run -c saffron-build bash -lc 'cd <repo> && cmake
  --build build/debug -j1'`. Headless verify with `SAFFRON_EXIT_AFTER_FRAMES=N`.
- Per AGENTS.md the `se`-current rule: every new command needs a `printResult` branch in
  `tools/se/source/main.cpp:112-177` (or it falls through to the JSON default — acceptable for
  most, but `get-thumbnail`'s base64 must NOT be dumped raw to a text terminal) AND a docs row.
- Docs are Hugo (hugo-book). The command table lives in
  `docs/content/reference/control-commands.md`; the per-file explanation pages are
  `docs/content/explanations/tooling-and-control/{scene,render,asset}-commands.md`. The schema
  pipeline gets a new explanation page.
- `schemas/` does not exist yet; `tools/` holds only `se`. Both are created here.

## Implementation

Order: engine commands first (they make `se` produce the payloads), then capture those payloads
into schemas + the contract test, then `se`/docs. Build `-j1` after each engine TU group.

### Step 1 — Scene: selection round-trip + scene/selection versions (`control_commands_scene.cpp`, `editor_context.{cppm,cpp}`)

Add two monotonically-increasing counters to `EditorContext` (`editor_context.cppm:39-60`,
insert after `EditorCamera camera;`):

```cpp
u64 sceneVersion = 0;       // bumped on entity create / destroy / copy
u64 selectionVersion = 0;   // bumped whenever the selection changes
```

- Bump `selectionVersion` in `setSelection` (`editor_context.cpp:16-20`):
  ```cpp
  void setSelection(EditorContext& ctx, Entity entity)
  {
      ctx.selected = entity;
      ctx.selectionVersion += 1;
      ctx.onSelectionChanged.publish(entity);
  }
  ```
  Keep this the ONLY selection mutation site (it already is — `pick`/`select`/`destroy-entity`/
  `load-*` all route through it).
- `sceneVersion` is bumped by the new `add-entity`/`copy-entity`/`destroy-entity` commands (step 2)
  rather than inside `createEntity`/`destroyEntity` (those are engine-internal and also used by the
  ImGui editor + serialization; bumping in the *command* keeps the version a control-plane concept
  and avoids touching `Saffron.Scene`). Also bump it in `load-scene`/`load-project` (the whole
  scene changed) — add `ctx.editor.sceneVersion += 1;` to those handlers
  (`control_commands_asset.cpp:159-203`).

Add the commands (in `registerSceneCommands`):

```cpp
registerCommand(reg, "get-selection",
    "get-selection — the current editor selection + version stamps",
    [](EngineContext& ctx, const json&) -> Result<json>
    {
        json out;
        out["selectionVersion"] = ctx.editor.selectionVersion;
        out["sceneVersion"] = ctx.editor.sceneVersion;
        const Entity sel = ctx.editor.selected;
        if (sel.handle != entt::null && valid(ctx.editor.scene, sel))
        {
            out["entity"] = entityRef(ctx.editor.scene, sel);
        }
        else
        {
            out["entity"] = nullptr;   // schema: nullable
        }
        return out;
    });

registerCommand(reg, "deselect", "deselect — clear the editor selection",
    [](EngineContext& ctx, const json&) -> Result<json>
    {
        setSelection(ctx.editor, Entity{ entt::null });
        return json{ { "selectionVersion", ctx.editor.selectionVersion } };
    });
```

`get-selection` is the cheap per-tick poll: the UI reads `selectionVersion`/`sceneVersion` every
tick and only re-fetches `inspect` (on selection/scene change) and `list-entities` (on scene
change). Both versions are surfaced here; `render-stats` is the other per-tick read (already cheap).

### Step 2 — Scene: `add-entity` (presets) + `copy-entity` (`control_commands_scene.cpp`)

`add-entity` replaces the Create menu / "Add +" / `onCreateCube`. The Model/Cube preset needs the
AssetServer (to import the bundled cube), which `EngineContext` has (`command.cppm:32`). Mirror the
ImGui presets (`editor_panels.cpp:23-62`) exactly so behaviour matches.

```cpp
registerCommand(reg, "add-entity",
    "add-entity {preset=empty|cube|model|point-light|spot-light|directional-light|camera}",
    [](EngineContext& ctx, const json& params) -> Result<json>
    {
        const std::string preset = asString(positionalOr(params, "preset", 0), "empty");
        Scene& scene = ctx.editor.scene;
        Entity e{ entt::null };
        if (preset == "empty")
        {
            e = createEntity(scene, "Entity");
        }
        else if (preset == "cube" || preset == "model")
        {
            // Reuse the bundled-cube import (same as Create > Cube / onCreateCube).
            auto cube = importModel(ctx.assets, ctx.renderer, assetPath("models/cube.gltf"));
            if (!cube) { return Err(cube.error()); }
            e = spawnModel(scene, "Cube", *cube);
        }
        else if (preset == "point-light")
        {
            e = createEntity(scene, "Point Light");
            addComponent<PointLightComponent>(scene, e);
            getComponent<TransformComponent>(scene, e).translation = glm::vec3(0.0f, 2.0f, 0.0f);
        }
        else if (preset == "spot-light")
        {
            e = createEntity(scene, "Spot Light");
            addComponent<SpotLightComponent>(scene, e);
            getComponent<TransformComponent>(scene, e).translation = glm::vec3(0.0f, 4.0f, 0.0f);
        }
        else if (preset == "directional-light")
        {
            e = createEntity(scene, "Directional Light");
            addComponent<DirectionalLightComponent>(scene, e);
        }
        else if (preset == "camera")
        {
            e = createEntity(scene, "Camera");
            addComponent<CameraComponent>(scene, e);
        }
        else
        {
            return Err(std::format("unknown preset '{}'", preset));
        }
        ctx.editor.sceneVersion += 1;
        setSelection(ctx.editor, e);
        return entityRef(scene, e);
    });
```

> `assetPath`, `importModel`, `spawnModel` are already used in this TU's sibling
> (`editor_app.cppm:208-218`); `assetPath` is exported from `Saffron.Rendering` and `importModel`/
> `spawnModel` from `Saffron.Assets` — both already imported by `control_commands_scene.cpp:14-17`.
> If `assetPath` is not visible from the control TU, route the cube import through the same
> `EngineContext` path the editor uses (confirm at build time; add `import Saffron.Window;` is NOT
> needed). Keep the cube path identical to `editor_app.cppm` so a fresh checkout's cube matches.

`copy-entity` mirrors `editor_panels.cpp:85-98` (deep-copy through the registry):

```cpp
registerCommand(reg, "copy-entity", "copy-entity {entity} — deep-duplicate it (selected)",
    [](EngineContext& ctx, const json& params) -> Result<json>
    {
        auto src = resolveEntity(ctx, params);
        if (!src) { return Err(src.error()); }
        Scene& scene = ctx.editor.scene;
        const std::string copyName =
            getComponent<NameComponent>(scene, *src).name + " (copy)";
        Entity fresh = createEntity(scene, copyName);
        for (const ComponentTraits& t : ctx.editor.registry.rows)
        {
            if (t.has(scene, *src))
            {
                t.addDefault(scene, fresh);
                static_cast<void>(t.deserialize(scene, fresh, t.serialize(scene, *src)));
            }
        }
        ctx.editor.sceneVersion += 1;
        setSelection(ctx.editor, fresh);
        return entityRef(scene, fresh);
    });
```

Also bump `ctx.editor.sceneVersion += 1;` in `destroy-entity`
(`control_commands_scene.cpp:54-69`, after `destroyEntity`).

### Step 3 — Scene: `set-component-field` (`control_commands_scene.cpp`)

A generic merge-one-field-into-a-component command so drag-drop can target ANY field (the sky
texture, or any Uuid/scalar a future field adds), not just `assign-asset`'s `mesh`/`albedo`.
Routes through the same registry serialize → patch → deserialize the existing `set-transform`/
`set-material` commands use (`control_commands_scene.cpp:142-242`), so the wire shape stays
identical to scene files.

```cpp
registerCommand(reg, "set-component-field",
    "set-component-field {entity, component, field, value} — merge one field "
    "(value may be a uuid string, number, bool, or json object)",
    [](EngineContext& ctx, const json& params) -> Result<json>
    {
        auto entity = resolveEntity(ctx, params);
        if (!entity) { return Err(entity.error()); }
        const std::string comp = asString(positionalOr(params, "component", 1), "");
        const std::string field = asString(positionalOr(params, "field", 2), "");
        if (comp.empty() || field.empty())
        {
            return Err(std::string{ "usage: set-component-field {entity, component, field, value}" });
        }
        const ComponentTraits* row = findByName(ctx.editor.registry, comp);
        if (row == nullptr) { return Err(std::format("unknown component '{}'", comp)); }
        if (!row->has(ctx.editor.scene, *entity))
        {
            row->addDefault(ctx.editor.scene, *entity);
        }
        json body = row->serialize(ctx.editor.scene, *entity);
        json value = positionalOr(params, "value", 3);
        // A bare uuid is passed as a string by the CLI; coerce numeric strings to u64
        // so a value<u64> deserialize doesn't abort under JSON_NOEXCEPTION (mirrors
        // set-material's albedoTexture handling, control_commands_scene.cpp:200-211).
        if (value.is_string())
        {
            const std::string s = value.get<std::string>();
            char* end = nullptr;
            const unsigned long long n = std::strtoull(s.c_str(), &end, 10);
            if (end != s.c_str() && *end == '\0') { value = static_cast<u64>(n); }
        }
        body[field] = value;
        auto result = row->deserialize(ctx.editor.scene, *entity, body);
        if (!result) { return Err(result.error()); }
        return json{ { "set", row->name }, { "field", field } };
    });
```

### Step 4 — Editor camera commands (`control_commands_scene.cpp`)

Read/merge-write `EditorContext.camera` (`editor_context.cppm:24-35`). camelCase on the wire.

```cpp
registerCommand(reg, "get-camera", "get-camera — the editor fly-camera state",
    [](EngineContext& ctx, const json&) -> Result<json>
    {
        const EditorCamera& c = ctx.editor.camera;
        return json{ { "position", vec3ToJson(c.position) },
                     { "yaw", c.yaw }, { "pitch", c.pitch }, { "fov", c.fov },
                     { "near", c.nearPlane }, { "far", c.farPlane },
                     { "moveSpeed", c.moveSpeed }, { "lookSpeed", c.lookSpeed } };
    });

registerCommand(reg, "set-camera",
    "set-camera {position?, yaw?, pitch?, fov?, near?, far?, moveSpeed?, lookSpeed?}",
    [](EngineContext& ctx, const json& params) -> Result<json>
    {
        EditorCamera& c = ctx.editor.camera;
        if (params.contains("position")) { c.position = vec3FromJson(params["position"]); }
        if (params.contains("yaw"))       { c.yaw = jsonF32Or(params, "yaw", c.yaw); }
        if (params.contains("pitch"))     { c.pitch = jsonF32Or(params, "pitch", c.pitch); }
        if (params.contains("fov"))       { c.fov = jsonF32Or(params, "fov", c.fov); }
        if (params.contains("near"))      { c.nearPlane = jsonF32Or(params, "near", c.nearPlane); }
        if (params.contains("far"))       { c.farPlane = jsonF32Or(params, "far", c.farPlane); }
        if (params.contains("moveSpeed")) { c.moveSpeed = jsonF32Or(params, "moveSpeed", c.moveSpeed); }
        if (params.contains("lookSpeed")) { c.lookSpeed = jsonF32Or(params, "lookSpeed", c.lookSpeed); }
        const EditorCamera& r = ctx.editor.camera;
        return json{ { "position", vec3ToJson(r.position) }, { "yaw", r.yaw }, { "pitch", r.pitch },
                     { "fov", r.fov }, { "near", r.nearPlane }, { "far", r.farPlane },
                     { "moveSpeed", r.moveSpeed }, { "lookSpeed", r.lookSpeed } };
    });
```

> `vec3ToJson`/`vec3FromJson`/`jsonF32Or` are from `Saffron.Scene` / `Saffron.Json`, already
> imported here. Wire keys `near`/`far` match the `Camera` component for consistency.
> `controlling` (RMB-latched) is engine-internal — do NOT surface it.

### Step 5 — Single gizmo state + `get-gizmo`/`set-gizmo` (`editor_context.{cppm,cpp}`, `editor_gizmo.cpp`, `control_commands_scene.cpp`)

There must be ONE gizmo state. Today the ImGuizmo path uses `EditorContext.gizmoOp`
(`ImGuizmo::OPERATION`) and is hardcoded WORLD; phase-4 adds the native overlay. Introduce a
backend-neutral op + space on `EditorContext` and derive `ImGuizmo::OPERATION`/`MODE` from it.

In `editor_context.cppm` (after the gizmo comment block around `:126-132`, add to the export
namespace; keep entt/glm/imgui-free so it stays consumable by the control TU):

```cpp
enum class GizmoOp { Translate, Rotate, Scale };
enum class GizmoSpace { World, Local };

auto gizmoOpName(GizmoOp op) -> const char*;       // "translate"|"rotate"|"scale"
auto gizmoOpFromName(const std::string&) -> GizmoOp;
auto gizmoSpaceName(GizmoSpace s) -> const char*;  // "world"|"local"
auto gizmoSpaceFromName(const std::string&) -> GizmoSpace;
```

Replace `ImGuizmo::OPERATION gizmoOp = ImGuizmo::TRANSLATE;` on `EditorContext`
(`editor_context.cppm:59`) with:

```cpp
GizmoOp gizmoOp = GizmoOp::Translate;
GizmoSpace gizmoSpace = GizmoSpace::World;
```

> The worktree's `NativeGizmoMode`/`NativeGizmoSpace`/`NativeGizmoState`
> (`wt:editor_context.cppm:37-74`) are forward-ported in phase-4 for the *overlay render + drag
> math*, but their `mode`/`space` must be **derived from** (not parallel to) `EditorContext.gizmoOp`/
> `gizmoSpace`. Phase-4 keeps the per-drag scratch fields (`startMouse`, `startTranslation`, …) on
> `NativeGizmoState` but reads op/space from this one source. Do not introduce `set-gizmo-mode`/
> `set-gizmo-space`.

Implement the name helpers in `editor_context.cpp` (no imgui include needed; this TU already has
`Saffron.Scene`). Update `drawGizmo` (`editor_gizmo.cpp:20-69`):
- W/E/R hotkeys (`:26-28`) now set `ctx.gizmoOp = GizmoOp::Translate|Rotate|Scale`.
- Derive the ImGuizmo operation + mode at the `Manipulate` call (`:48-49`):
  ```cpp
  ImGuizmo::OPERATION op = ctx.gizmoOp == GizmoOp::Rotate ? ImGuizmo::ROTATE
                         : ctx.gizmoOp == GizmoOp::Scale  ? ImGuizmo::SCALE
                                                          : ImGuizmo::TRANSLATE;
  ImGuizmo::MODE mode = ctx.gizmoSpace == GizmoSpace::Local ? ImGuizmo::LOCAL : ImGuizmo::WORLD;
  ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(proj), op, mode, glm::value_ptr(model));
  ```
  This is the **local-space ADD** for the ImGuizmo path. Note ImGuizmo forces WORLD for SCALE
  internally regardless of `mode` — that is acceptable (matches ImGuizmo semantics); the local/world
  distinction is meaningful for translate/rotate. The Euler-delta write-back (`:51-67`) is unchanged.

Commands (in `registerSceneCommands`):

```cpp
registerCommand(reg, "get-gizmo", "get-gizmo — the gizmo op + space",
    [](EngineContext& ctx, const json&) -> Result<json>
    {
        return json{ { "op", gizmoOpName(ctx.editor.gizmoOp) },
                     { "space", gizmoSpaceName(ctx.editor.gizmoSpace) } };
    });

registerCommand(reg, "set-gizmo", "set-gizmo {op?:translate|rotate|scale, space?:world|local}",
    [](EngineContext& ctx, const json& params) -> Result<json>
    {
        if (params.contains("op"))
        {
            ctx.editor.gizmoOp = gizmoOpFromName(asString(params["op"], "translate"));
        }
        if (params.contains("space"))
        {
            ctx.editor.gizmoSpace = gizmoSpaceFromName(asString(params["space"], "world"));
        }
        return json{ { "op", gizmoOpName(ctx.editor.gizmoOp) },
                     { "space", gizmoSpaceName(ctx.editor.gizmoSpace) } };
    });
```

> Adding `gizmoOp`/`gizmoSpace` as plain enums (not `ImGuizmo::OPERATION`) lets the control TU
> read/write them without pulling in ImGuizmo. `editor_gizmo.cpp` is the only place that maps to
> ImGuizmo types, keeping the imgui dependency contained.

### Step 6 — `get-thumbnail` PNG readback (`renderer_thumbnail.cpp`, `renderer_types.cppm`, `control_commands_asset.cpp`)

New renderer entry point that renders/loads an asset to a host buffer and returns **PNG bytes in
memory** (no temp file, no GpuTexture handed back). Declare beside `renderMeshThumbnail`
(`renderer_types.cppm:1045`):

```cpp
// Renders/loads an asset to a `size`x`size` image and reads it back as encoded PNG bytes
// (synchronous, own command buffer + waitIdle; never on the present path). For meshes,
// frames the mesh like renderMeshThumbnail; for textures, downsamples the bindless image.
auto encodeAssetThumbnailPng(Renderer& renderer, const Ref<GpuMesh>& mesh, u32 size)
    -> Result<std::vector<u8>>;
auto encodeTextureThumbnailPng(Renderer& renderer, const Ref<GpuTexture>& texture, u32 size)
    -> Result<std::vector<u8>>;
```

Implement in `renderer_thumbnail.cpp` (existing TU; no CMake edit):
- **Mesh path** — copy `renderMeshThumbnail`'s body (`:141-255`) up to and including the submit +
  `waitIdle`, but BEFORE taking ownership as a `GpuTexture` (`:257-271`), add a readback: the color
  image is already `eShaderReadOnlyOptimal`. Run a second one-off command buffer (or extend the
  first) calling `captureImageToBuffer(cmd, color.image, color.extent, eShaderReadOnlyOptimal,
  eFragmentShader, eShaderSampledRead, eShaderReadOnlyOptimal, eFragmentShader, eShaderSampledRead,
  hostBuffer)` against a `newHostCaptureBuffer(size*size*formatPixelBytes(color.format))`; submit +
  `waitIdle` + `vmaInvalidateAllocation`. Then PNG-encode to memory.
- **Encode-to-memory helper.** `writeBufferToPng` (`renderer_detail.cppm:978-1023`) writes to a
  *file*. Add a sibling in `:Detail` that returns bytes, or refactor `writeBufferToPng` to build the
  3-channel RGB vector then call `stbi_write_png_to_func` with a closure that appends to a
  `std::vector<u8>`. Preferred: add `encodeBufferToPng(pixels, w, h, format) -> Result<std::vector<u8>>`
  (the BGRA/RGBA + RGBA16F→sRGB conversion is identical; only the final `stbi_write_png` →
  `stbi_write_png_to_func` differs) and have `writeBufferToPng` call it then `std::ofstream` the
  bytes, so there is one conversion path.
- **Texture path** — `texture->image` is in the bindless array (`eShaderReadOnlyOptimal`,
  `GpuTexture` at `renderer_types.cppm:272-281` carries `image`/`extent`/`format`). For an MVP,
  read back the texture at its **native** extent (ignore `size`, or document `size` as a hint only)
  via `captureImageToBuffer` + `encodeBufferToPng`; stb downscaling is out of scope. If `size` must
  be honored, blit `texture->image` → a `size×size` `newColorImage` with `vk::Filter::eLinear`
  first, then read that back (mirrors the mesh offscreen). Start with native-extent readback; gate
  honoring `size` behind a follow-up if payloads are too large.
- Free all transient buffers/images; never run on the main present path (caller idles).

`get-thumbnail` command (in `registerAssetCommands`, `control_commands_asset.cpp`):

```cpp
registerCommand(reg, "get-thumbnail", "get-thumbnail {asset:id|name, size=128} — base64 PNG preview",
    [](EngineContext& ctx, const json& params) -> Result<json>
    {
        const std::string selector = asString(positionalOr(params, "asset", 0), "");
        const u64 byId = std::strtoull(selector.c_str(), nullptr, 10);
        const AssetEntry* match = nullptr;
        for (const AssetEntry& e : ctx.assets.catalog.entries)
        {
            if (e.id.value == byId || e.name == selector) { match = &e; }
        }
        if (match == nullptr) { return Err(std::format("no asset '{}'", selector)); }
        u32 size = 128;
        const json sz = positionalOr(params, "size", 1);
        if (sz.is_number()) { size = static_cast<u32>(sz.get<double>()); }

        std::vector<u8> png;
        if (match->type == AssetType::Mesh)
        {
            auto mesh = loadMeshAsset(ctx.assets, ctx.renderer, match->id);
            if (!mesh) { return Err(std::string{ "mesh failed to load" }); }
            auto bytes = encodeAssetThumbnailPng(ctx.renderer, mesh, size);
            if (!bytes) { return Err(bytes.error()); }
            png = std::move(*bytes);
        }
        else if (match->type == AssetType::Texture)
        {
            auto tex = loadTextureAsset(ctx.assets, ctx.renderer, match->id);
            if (!tex) { return Err(std::string{ "texture failed to load" }); }
            auto bytes = encodeTextureThumbnailPng(ctx.renderer, tex, size);
            if (!bytes) { return Err(bytes.error()); }
            png = std::move(*bytes);
        }
        else
        {
            return Err(std::string{ "asset has no thumbnail" });
        }
        return json{ { "id", match->id.value }, { "format", "png" },
                     { "width", size }, { "height", size },
                     { "base64", base64Encode(png) } };
    });
```

> `loadMeshAsset`/`loadTextureAsset`/`findAsset` are from `Saffron.Assets` (already imported in this
> TU). A small `base64Encode(const std::vector<u8>&) -> std::string` helper is needed — add it to
> `Saffron.Core` (`core.cppm`, alongside the other utilities) since `Saffron.Json` has no binary
> support and nlohmann's binary type is not wire-portable. Standard table-based base64; no
> dependency.

`view-asset` is `get-thumbnail` at a larger default size — register it as an alias that calls the
same body with `size` defaulting to 512 (or simply document `se view-asset {asset} --size 512`
mapping to `get-thumbnail`; the TS client (phase-7) maps `viewAsset → getThumbnail({size:512})`).
To keep one engine code path, register `view-asset` whose handler defaults `size=512` and reuses
the resolution + encode logic (factor the body into a static helper `thumbnailResult(ctx, params,
defaultSize)`).

### Step 7 — `dump-schema` (`control_commands_render.cpp` or a new `control_commands_schema.cpp`)

Emit the live shapes the codegen + contract test key off, derived from the running registry +
serializers so it can never drift from the actual wire output:

```cpp
registerCommand(reg, "dump-schema",
    "dump-schema — live component / environment / render-stats shapes for codegen",
    [](EngineContext& ctx, const json&) -> Result<json>
    {
        // Components: name -> a sample serialized body (default-constructed instance).
        json components = json::object();
        for (const ComponentTraits& row : ctx.editor.registry.rows)
        {
            // Serialize a default instance on a scratch entity to capture the field shape.
            Entity scratch = createEntity(ctx.editor.scene, "__schema__");
            row.addDefault(ctx.editor.scene, scratch);
            components[row.name] = json{ { "removable", row.removable },
                                         { "fields", row.serialize(ctx.editor.scene, scratch) } };
            destroyEntity(ctx.editor.scene, scratch);
        }
        json out;
        out["components"] = std::move(components);
        out["environment"] = environmentToJson(ctx.editor.scene.environment);
        // render-stats shape is fixed inline; emit a sample so the schema author/contract test
        // sees every key + JSON type.
        out["renderStats"] = /* call the same builder render-stats uses, factored to a helper */;
        return out;
    });
```

> Refactor the inline `render-stats` body (`control_commands_render.cpp:38-61`) into a free
> `renderStatsJson(Renderer&) -> json` so both `render-stats` and `dump-schema` use it (single
> source). The scratch-entity create/destroy must NOT bump `sceneVersion` (it doesn't —
> `createEntity`/`destroyEntity` don't bump; only the commands do). `dump-schema` is the forward
> seam: when named C++ DTO structs eventually exist, `reflect-cpp`'s `rfl::json::to_schema` can
> regenerate the SAME `schemas/control/*.schema.json` files the TS pipeline already consumes
> (researchDigest); until then the schemas are hand-authored and `dump-schema` feeds the contract
> test.

### Step 8 — Author `schemas/control/` (draft 2020-12)

Create `schemas/control/` with one file per DTO + one per command result. Bootstrap ONCE with
quicktype from captured `se -o json` payloads, then hand-correct (quicktype is lossy on the
discriminated union + nullability + u64-as-string — researchDigest), then commit. Files:

- `envelope.schema.json` — `{ ok:boolean, error?:string, result?:object }`; `oneOf` on `ok`.
- `vec3.schema.json` `{x,y,z:number}`, `vec4.schema.json` `{x,y,z,w:number}`.
- `uuid.schema.json` — `{ "type": "string", "pattern": "^[0-9]+$" }` (u64-as-string; NEVER number).
- `entity-ref.schema.json` `{ id:Uuid, name:string }`.
- Component DTOs (`name`/`transform`/`mesh`/`camera`/`material`/`directional-light`/`point-light`/
  `spot-light`) with the exact keys from "Current state" (Camera uses `near`/`far`; Material the 7
  fields; SpotLight angles documented degrees; Transform.rotation documented Euler XYZ radians).
- `components.schema.json` — the discriminated union: an object whose keys are component names, each
  value matching the matching component DTO (`patternProperties` / per-key `$ref`).
- `inspect-result.schema.json` `{ id:Uuid, name:string, components:Components }`.
- `render-stats.schema.json` — all 21 fields with `aa` as an enum
  `["off","fxaa","taa","msaa2","msaa4","msaa8"]`.
- `environment.schema.json`, `asset-entry.schema.json`.
- `selection.schema.json` `{ entity: EntityRef | null, selectionVersion:integer, sceneVersion:integer }`.
- `gizmo-state.schema.json` `{ op:enum, space:enum }`.
- `editor-camera.schema.json` `{ position:Vec3, yaw, pitch, fov, near, far, moveSpeed, lookSpeed }`.
- `thumbnail.schema.json` `{ id:Uuid, format:"png", width:integer, height:integer, base64:string }`.
- Per-command result schemas (`add-entity` → EntityRef, `copy-entity` → EntityRef, `get-selection`
  → Selection, `set-gizmo`/`get-gizmo` → GizmoState, `set-camera`/`get-camera` → EditorCamera,
  `render-stats` → RenderStats, `list-entities` → `{entities:EntityRef[]}`, `list-assets` →
  `{assets:AssetEntry[]}`, `inspect` → InspectResult, `get-thumbnail`/`view-asset` → Thumbnail,
  `get-environment`/`set-environment` → Environment, `set-component-field` → `{set,field}`, the
  `set-*` render toggles → their `{flag:value}` echoes).

Enforce `additionalProperties: false` on DTOs (catches inline-json drift). Enforce camelCase keys
(matches the wire). The schemas are the durable contract; `dump-schema`/`reflect-cpp` are forward
seams only.

### Step 9 — `tools/check-control-schema/check.ts` (contract test)

A Bun/TS script that launches a bounded headless SaffronEditor, drives `se` for one example of
every command, and validates each `result` against its schema with Ajv (draft 2020-12, e.g.
`ajv` + `ajv-formats`, or `@cfworker/json-schema`). Outline:

```ts
// tools/check-control-schema/check.ts
// 1. spawn build/debug/bin/SaffronEditor with SAFFRON_CONTROL_SOCK=<tmp> SAFFRON_EXIT_AFTER_FRAMES=0
//    (or kill it at the end); wait for the socket; (X11 not needed — headless control only).
// 2. for each [command, params, schemaFile] in a fixture table, run `se <cmd> -o json` against
//    the socket, parse the {ok,result}, assert ok===true, validate result against the schema.
// 3. validate the full envelope of an intentionally-bad command (ok:false + error:string).
// 4. exit non-zero on any failure; print the failing command + Ajv errors.
```

Run it in the toolbox: `toolbox run -c saffron-build bash -lc 'cd <repo> && bun run tools/check-control-schema/check.ts'`
(Bun availability is the phase-3 spike 0a; if Bun is host-only, the test runs on the host against
a toolbox-launched engine — the socket is reachable cross-boundary since `$HOME` is shared). The
fixture table doubles as living `se` usage docs. Add a `package.json` script `check:schema`.

> The contract test is the ONLY drift guard given there are no named C++ DTOs. It must cover the
> u64-as-string assertion explicitly: assert each id field matches `^[0-9]+$` AND that the captured
> raw JSON line did not lose precision (read the raw bytes, not a JS-number round-trip).

### Step 10 — `se` formatters + docs

- `tools/se/source/main.cpp` `printResult` (`:112-177`): add branches for the high-value text
  outputs — `get-selection` (entity name + versions), `get-gizmo`/`set-gizmo` (`op/space`),
  `get-camera`/`set-camera` (pos + yaw/pitch), `add-entity`/`copy-entity` (reuse the entityRef
  shape). For `get-thumbnail`/`view-asset` print a SUMMARY (`png 128x128, NNNN bytes`) and NOT the
  base64 blob in text mode; `-o json` still dumps it. Everything else falls through to the JSON
  default.
- `docs/content/reference/control-commands.md`: add rows for all 12 new commands in the right group
  table (scene: add-entity, copy-entity, deselect, get-selection, set-component-field, get-gizmo,
  set-gizmo, get-camera, set-camera; asset: get-thumbnail, view-asset; render/schema: dump-schema).
- `docs/content/explanations/tooling-and-control/scene-commands.md` + `asset-commands.md`: extend.
- New `docs/content/explanations/tooling-and-control/shared-types.md` (and a hub row in that
  section's `_index.md`): explain the schema-first pipeline — `schemas/control/` is the source of
  truth → `json-schema-to-typescript` (phase-3) → `@saffron/protocol`; C++ is a validated consumer
  via `tools/check-control-schema`; `dump-schema`/`reflect-cpp` are deferred forward seams; the
  u64-as-string rule; camelCase; Transform-radians / SpotLight-degrees units.

### Build / verify after each TU group

```sh
toolbox run -c saffron-build bash -lc '
  cd /var/home/saffronjam/repos/SaffronEngine
  cmake --build build/debug -j1
  cmake --build build/debug --target se
'
```

Smoke-drive (no X11 needed — control plane is headless):

```sh
toolbox run -c saffron-build bash -lc '
  cd /var/home/saffronjam/repos/SaffronEngine
  SAFFRON_CONTROL_SOCK=/tmp/se-p2.sock SAFFRON_EXIT_AFTER_FRAMES=0 ./build/debug/bin/SaffronEditor &
  sleep 2
  export SAFFRON_CONTROL_SOCK=/tmp/se-p2.sock
  ./build/debug/bin/se add-entity --preset cube
  ./build/debug/bin/se add-entity --preset point-light
  ID=$(./build/debug/bin/se list-entities -o json | head)   # pick an id
  ./build/debug/bin/se copy-entity --entity "$ID"
  ./build/debug/bin/se get-selection -o json
  ./build/debug/bin/se set-gizmo --op rotate --space local && ./build/debug/bin/se get-gizmo -o json
  ./build/debug/bin/se get-camera -o json
  ./build/debug/bin/se list-assets -o json    # grab a mesh + texture id
  ./build/debug/bin/se get-thumbnail --asset <meshId> -o json   # base64 png
  ./build/debug/bin/se dump-schema -o json
  ./build/debug/bin/se quit
'
```

## Done when

- [ ] `cmake --build build/debug -j1` succeeds in the toolbox with no new validation errors; a
      `SAFFRON_EXIT_AFTER_FRAMES` bounded run exits clean (no VMA leak abort).
- [ ] `se add-entity --preset cube` spawns a cube + returns an `EntityRef`; every preset
      (empty/cube/model/point-light/spot-light/directional-light/camera) works and selects the
      result; `sceneVersion` increments on each create.
- [ ] `se copy-entity --entity X` produces an identical duplicate (verified: `se inspect` of the
      copy matches the source except `id`/`name`); `sceneVersion` increments.
- [ ] `se get-selection` reflects an in-viewport `se pick`; `selectionVersion` increments on select/
      deselect; `sceneVersion` bumps on create/destroy/copy/load. `se deselect` clears it.
- [ ] `se set-gizmo --op rotate --space local` changes the gizmo and `se get-gizmo` reads
      `{op:"rotate",space:"local"}` back; the ImGuizmo manipulate uses `ImGuizmo::LOCAL` for the
      local case (verified in `editor-old` by a visibly object-aligned rotate gizmo); NO
      `set-gizmo-mode`/`set-gizmo-space` commands exist (one gizmo state only).
- [ ] `se set-component-field --entity X --component Material --field albedoTexture --value <texId>`
      assigns the texture (verified via `se inspect`); a Uuid string is coerced to u64 (no abort).
- [ ] `se get-camera` returns the editor fly-cam state; `se set-camera --yaw 90` moves it and a
      `get-camera` reads the new value.
- [ ] `se get-thumbnail --asset <meshId>` returns `{format:"png", base64, width, height}` whose
      base64 decodes to a valid PNG (verified: `... -o json | jq -r .result.base64 | base64 -d > t.png
      && file t.png` reports a PNG); same for a `<textureId>`. `view-asset` returns a 512 PNG.
- [ ] `se dump-schema` emits the live component / environment / render-stats shapes and matches the
      committed schemas (the contract test consumes it).
- [ ] `schemas/control/` exists with the full DTO catalog + per-command result schemas (draft
      2020-12, ids typed string with `^[0-9]+$`, camelCase, `additionalProperties:false` on DTOs).
- [ ] `tools/check-control-schema/check.ts` runs green in the documented environment: every command
      fixture validates against its schema, the bad-command envelope validates as `ok:false`, and
      the u64-as-string / no-precision-loss assertion passes.
- [ ] All 12 new commands appear in `se help`; `printResult` formats the text-friendly ones and does
      NOT dump base64 in text mode.
- [ ] `docs/content/reference/control-commands.md` rows added; the scene/asset explanation pages
      updated; a new `shared-types.md` explanation page + hub row exists.
- [ ] The `editor-old/` C++ ImGui editor still builds + runs unchanged (the gizmo op/space refactor
      did not regress the ImGuizmo path; W/E/R still cycles; the gizmo still manipulates).

## Risks / seams

- **Gizmo reconciliation.** `EditorContext.gizmoOp` changes type (`ImGuizmo::OPERATION` →
  `GizmoOp`), touching `editor_gizmo.cpp` + the editor-old W/E/R + the phase-4 native path. Keep
  `editor_context.cppm`'s new enums imgui-free so the control TU compiles; map to `ImGuizmo::*` only
  inside `editor_gizmo.cpp`. Phase-4's `NativeGizmoState.mode/space` must DERIVE from
  `gizmoOp`/`gizmoSpace`, not duplicate them. ImGuizmo treats SCALE as WORLD internally — document,
  don't fight it.
- **`get-thumbnail` is real renderer work.** It runs its own command buffer + `waitIdle`; it must
  never run on the main present path (the caller — a control command drained per frame on the main
  thread — already serializes with the frame; `captureViewport` proves the pattern is safe between
  frames). Watch for: leaking the host buffer/transient image, leaving the source image in the
  wrong layout (texture path: restore `eShaderReadOnlyOptimal` so the bindless array stays valid),
  and large base64 payloads bloating the poll (mitigation: client-side cache in phase-7; cap default
  size at 128; honor `size` only via blit if needed).
- **Base64 + PNG-to-memory.** Adding `base64Encode` to `Saffron.Core` and refactoring
  `writeBufferToPng` to share an `encodeBufferToPng` (via `stbi_write_png_to_func`) is the cleanest;
  do NOT introduce a base64 dependency. Verify the half-float (RGBA16F) branch is unreachable for
  thumbnails (they render in `swapchain.format`, 8-bit) but keep the shared converter correct for
  both.
- **Schema authoring + the contract test are the only drift guard.** There are no named C++ DTOs
  (every response is inline `nlohmann::json`), so the schema can silently diverge from the engine.
  The contract test MUST run in CI (phase-10) and exercise every command; treat a schema/engine
  mismatch as a build break. quicktype is bootstrap-only (lossy on the discriminated union); never
  wire it into the steady-state build.
- **u64-as-string everywhere.** Every id field (`EntityRef.id`, `Mesh.mesh`, `Material.albedoTexture`,
  `Environment.skyTexture`, `AssetEntry.id`, `Thumbnail.id`) is a `u64` over `Number.MAX_SAFE_INTEGER`.
  The schema types them `string`/`^[0-9]+$`; the TS client (phase-3) must string-preserve-parse. The
  contract test asserts no precision loss on the raw JSON bytes. This is the most common way the port
  silently corrupts data.
- **`dump-schema` scratch entity.** Creating/destroying a `__schema__` entity to capture default
  field shapes must not bump `sceneVersion` (it won't — only the commands bump) and must run on the
  main thread (it does — control is drained on the main thread). It is a forward seam for
  reflect-cpp, not the source of truth.
- **`add-entity` cube path coupling.** The cube preset imports `models/cube.gltf` via the same
  `assetPath`/`importModel`/`spawnModel` the editor uses (`editor_app.cppm:208-218`). If `assetPath`
  is not reachable from the control TU, route through the editor's existing path; keep it identical
  so a fresh checkout's cube matches across the ImGui editor, `se`, and the future TS Create menu.
