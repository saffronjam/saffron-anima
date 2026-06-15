module;

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>
#include <nlohmann/json.hpp>

#include <string>

module Saffron.SceneEdit;

import Saffron.Core;
import Saffron.Signal;
import Saffron.Scene;
import Saffron.Json;

namespace sa
{
    void setSelection(SceneEditContext& ctx, Entity entity)
    {
        ctx.selected = entity;
        ctx.selectionVersion += 1;
        ctx.onSelectionChanged.publish(entity);
    }

    auto debugOverlaysToJson(const DebugOverlayOptions& opts) -> nlohmann::json
    {
        return nlohmann::json{ { "bounds", opts.bounds },
                               { "sceneAabb", opts.sceneAabb },
                               { "lightVolumes", opts.lightVolumes },
                               { "grid", opts.grid },
                               { "colliders", opts.colliders } };
    }

    void debugOverlaysFromJson(DebugOverlayOptions& opts, const nlohmann::json& j)
    {
        if (!j.is_object())
        {
            return;
        }
        opts.bounds = jsonBoolOr(j, "bounds", opts.bounds);
        opts.sceneAabb = jsonBoolOr(j, "sceneAabb", opts.sceneAabb);
        opts.lightVolumes = jsonBoolOr(j, "lightVolumes", opts.lightVolumes);
        opts.grid = jsonBoolOr(j, "grid", opts.grid);
        opts.colliders = jsonBoolOr(j, "colliders", opts.colliders);
    }

    auto gizmoOpName(GizmoOp op) -> const char*
    {
        switch (op)
        {
        case GizmoOp::Rotate:
            return "rotate";
        case GizmoOp::Scale:
            return "scale";
        case GizmoOp::Translate:
            break;
        }
        return "translate";
    }

    auto gizmoOpFromName(const std::string& name) -> GizmoOp
    {
        if (name == "rotate")
        {
            return GizmoOp::Rotate;
        }
        if (name == "scale")
        {
            return GizmoOp::Scale;
        }
        return GizmoOp::Translate;
    }

    auto gizmoSpaceName(GizmoSpace space) -> const char*
    {
        if (space == GizmoSpace::Local)
        {
            return "local";
        }
        return "world";
    }

    auto gizmoSpaceFromName(const std::string& name) -> GizmoSpace
    {
        if (name == "local")
        {
            return GizmoSpace::Local;
        }
        return GizmoSpace::World;
    }

    auto newSceneEditContext() -> SceneEditContext*
    {
        SceneEditContext* ctx = new SceneEditContext();
        // Components are registered by the client via registerBuiltinComponents(reg,
        // thumbnailFor) once the thumbnail provider exists. Seeding entities below uses
        // entt directly, so it does not need the ComponentRegistry populated yet.

        // Seed a camera looking at the origin so a freshly spawned mesh is visible.
        Entity camera = createEntity(ctx->scene, "Camera");
        addComponent<CameraComponent>(ctx->scene, camera);
        TransformComponent& cameraTransform = getComponent<TransformComponent>(ctx->scene, camera);
        cameraTransform.translation = glm::vec3(3.0f, 2.5f, 4.0f);
        cameraTransform.rotation = glm::eulerAngles(
            glm::quatLookAt(glm::normalize(-cameraTransform.translation), glm::vec3(0.0f, 1.0f, 0.0f)));

        Entity sun = createEntity(ctx->scene, "Sun");
        addComponent<DirectionalLightComponent>(ctx->scene, sun);

        setSelection(*ctx, camera);
        return ctx;
    }

    void destroySceneEditContext(SceneEditContext* ctx)
    {
        delete ctx;
    }
}
