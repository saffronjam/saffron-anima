module;

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>

module Saffron.Editor;

import Saffron.Core;
import Saffron.Signal;
import Saffron.Scene;

namespace se
{
    void setSelection(EditorContext& ctx, Entity entity)
    {
        ctx.selected = entity;
        ctx.onSelectionChanged.publish(entity);
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
