module;

// Bridges Scene + Geometry + Rendering, so (like those) it uses classic includes.
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <expected>
#include <string>
#include <unordered_map>

export module Saffron.Assets;

import Saffron.Core;
import Saffron.Geometry;
import Saffron.Rendering;
import Saffron.Scene;

export namespace se
{
    // Maps a stable asset id to its uploaded GPU mesh handle. The GpuMesh objects
    // themselves are owned by the Renderer; this only holds handles, so multiple
    // entities referencing one id share a single upload.
    struct AssetServer
    {
        std::unordered_map<u64, u32> meshHandleByUuid;
    };

    // Imports a model file, uploads it, registers it under a fresh id, returns the id.
    std::expected<Uuid, std::string> importModel(AssetServer& assets, Renderer& renderer, const std::string& path)
    {
        std::expected<Mesh, std::string> mesh = importModelFile(path);
        if (!mesh)
        {
            return std::unexpected(mesh.error());
        }
        std::expected<u32, std::string> handle = uploadMesh(renderer, *mesh);
        if (!handle)
        {
            return std::unexpected(handle.error());
        }
        const Uuid id = newUuid();
        assets.meshHandleByUuid[id.value] = *handle;
        return id;
    }

    bool resolveMesh(const AssetServer& assets, Uuid id, u32& outHandle)
    {
        auto it = assets.meshHandleByUuid.find(id.value);
        if (it == assets.meshHandleByUuid.end())
        {
            return false;
        }
        outHandle = it->second;
        return true;
    }

    // Creates an entity carrying the given mesh asset.
    Entity spawnMesh(Scene& scene, std::string name, Uuid mesh)
    {
        Entity entity = createEntity(scene, std::move(name));
        addComponent<MeshComponent>(scene, entity).mesh = mesh;
        return entity;
    }

    // Draws every entity with a Transform + Mesh, viewed through the first primary
    // camera. A no-op when there is no camera or no viewport.
    void renderScene(Renderer& renderer, Scene& scene, AssetServer& assets, u32 meshPipeline)
    {
        bool haveCamera = false;
        glm::mat4 view{ 1.0f };
        f32 fov = 45.0f;
        f32 nearPlane = 0.1f;
        f32 farPlane = 100.0f;
        forEach<TransformComponent, CameraComponent>(scene,
            [&](Entity, TransformComponent& transform, CameraComponent& camera)
            {
                if (haveCamera || !camera.primary)
                {
                    return;
                }
                const glm::mat4 cameraModel =
                    glm::translate(glm::mat4(1.0f), transform.translation) * glm::mat4_cast(transform.rotation);
                view = glm::inverse(cameraModel);
                fov = camera.fov;
                nearPlane = camera.nearPlane;
                farPlane = camera.farPlane;
                haveCamera = true;
            });
        if (!haveCamera)
        {
            return;
        }

        const u32 width = viewportWidth(renderer);
        const u32 height = viewportHeight(renderer);
        if (width == 0 || height == 0)
        {
            return;
        }
        const f32 aspect = static_cast<f32>(width) / static_cast<f32>(height);
        glm::mat4 proj = glm::perspective(glm::radians(fov), aspect, nearPlane, farPlane);
        proj[1][1] *= -1.0f;  // flip Y into Vulkan clip space
        const glm::mat4 viewProjection = proj * view;

        forEach<TransformComponent, MeshComponent>(scene,
            [&](Entity, TransformComponent& transform, MeshComponent& mesh)
            {
                u32 handle = 0;
                if (!resolveMesh(assets, mesh.mesh, handle))
                {
                    return;
                }
                drawMesh(renderer, handle, meshPipeline, viewProjection * transformMatrix(transform));
            });
    }
}
