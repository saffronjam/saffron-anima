module;

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>

#include <string>

module Saffron.Editor;

import Saffron.Core;
import Saffron.Signal;
import Saffron.Scene;

namespace se
{
    void setSelection(EditorContext& ctx, Entity entity)
    {
        ctx.selected = entity;
        ctx.selectionVersion += 1;
        ctx.onSelectionChanged.publish(entity);
    }

    auto gizmoOpName(GizmoOp op) -> const char*
    {
        switch (op)
        {
            case GizmoOp::Rotate: return "rotate";
            case GizmoOp::Scale: return "scale";
            case GizmoOp::Translate: break;
        }
        return "translate";
    }

    auto gizmoOpFromName(const std::string& name) -> GizmoOp
    {
        if (name == "rotate") { return GizmoOp::Rotate; }
        if (name == "scale") { return GizmoOp::Scale; }
        return GizmoOp::Translate;
    }

    auto gizmoSpaceName(GizmoSpace space) -> const char*
    {
        return space == GizmoSpace::Local ? "local" : "world";
    }

    auto gizmoSpaceFromName(const std::string& name) -> GizmoSpace
    {
        return name == "local" ? GizmoSpace::Local : GizmoSpace::World;
    }

    auto newEditorContext() -> EditorContext*
    {
        EditorContext* ctx = new EditorContext();
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

    void destroyEditorContext(EditorContext* ctx)
    {
        delete ctx;
    }
}
