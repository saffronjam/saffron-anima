module;

// entt + glm are header-heavy C++ libraries, so this module uses classic
// includes (no `import std`), like the rendering/ui modules.
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <expected>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

export module Saffron.Scene;

import Saffron.Core;
import Saffron.Json;

export namespace se
{
    struct NameComponent
    {
        std::string name;
    };

    struct IdComponent
    {
        Uuid id;
    };

    struct TransformComponent
    {
        glm::vec3 translation{ 0.0f };
        glm::vec3 scale{ 1.0f };
        glm::vec3 rotation{ 0.0f };  // Euler XYZ radians; the editor edits these directly
    };

    // A node in the scene tree. parent (a Uuid; 0 == root) is the only durable field;
    // parentHandle and children are runtime caches rebuilt by relinkHierarchy after any
    // structural change — never serialized or copied (entt ids are not stable across load).
    struct RelationshipComponent
    {
        Uuid parent;                             // 0 == root
        entt::entity parentHandle = entt::null;  // resolved cache
        std::vector<entt::entity> children;      // derived cache
    };

    // Cached world matrix, overwritten each frame by updateWorldTransforms. Stays
    // unregistered (like IdComponent), so serializeEntity skips it.
    struct WorldTransformComponent
    {
        glm::mat4 matrix{ 1.0f };
    };

    // Tags a skeleton joint (set by the glTF skin import) so the outliner can filter
    // bone rows. Serialized as an empty object; a bone is otherwise an ordinary entity.
    struct BoneComponent
    {
        // entt elides storage for empty types (emplace/get return void there), which the
        // generic addComponent/getComponent reflection cannot bind — keep one byte.
        u8 tag = 0;
    };

    // A skinned renderable: the mesh asset plus the ordered joint list by uuid.
    // bones[i] drives jointMatrices()[i] through inverseBind[i] — glTF joint order,
    // defined solely by the import. boneHandles is a runtime cache rebuilt by
    // relinkHierarchy; like the Relationship caches it is never serialized or copied.
    struct SkinnedMeshComponent
    {
        Uuid mesh;
        Uuid rootBone;
        std::vector<Uuid> bones;
        std::vector<glm::mat4> inverseBind;
        std::vector<entt::entity> boneHandles;  // resolved cache
    };

    // References a mesh asset by stable id; the AssetServer resolves it to a GPU mesh.
    struct MeshComponent
    {
        Uuid mesh;
    };

    // Per-entity material applied to the whole mesh. albedoTexture == 0 means "none"
    // (the renderer binds its default white texture, so baseColor shows directly).
    // metallic/roughness drive the Cook-Torrance BRDF; emissive adds unlit radiance.
    struct MaterialComponent
    {
        glm::vec4 baseColor{ 1.0f };
        Uuid albedoTexture;
        Uuid metallicRoughnessTexture;  // glTF metallic-roughness map (rough=G, metal=B); modulates the factors
        f32 metallic = 0.0f;
        f32 roughness = 1.0f;
        glm::vec3 emissive{ 0.0f };
        f32 emissiveStrength = 1.0f;
        bool unlit = false;  // skip lighting (albedo * base color only) — a distinct PSO
    };

    // One material in a multi-material mesh; the same fields as MaterialComponent.
    struct MaterialSlot
    {
        glm::vec4 baseColor{ 1.0f };
        Uuid albedoTexture;
        Uuid metallicRoughnessTexture;
        f32 metallic = 0.0f;
        f32 roughness = 1.0f;
        glm::vec3 emissive{ 0.0f };
        f32 emissiveStrength = 1.0f;
        bool unlit = false;
    };

    // An ordered material table for a mesh with more than one source material; each
    // Submesh.materialSlot indexes `slots`. Supersedes MaterialComponent when present.
    // Single-material meshes keep using MaterialComponent instead.
    struct MaterialSetComponent
    {
        std::vector<MaterialSlot> slots;
    };

    // One script attached to an entity: a .lua path relative to the project src/
    // plus per-instance field overrides (filled by the editor; empty until then).
    struct ScriptSlot
    {
        std::string scriptPath;
        nlohmann::json overrides = nlohmann::json::object();
    };

    // An entity's scripts, run top-to-bottom each play tick. entt keys components
    // by type, so multiple scripts per entity is this vector, never two components.
    // Data only — the Lua runtime lives entirely in Saffron.Script.
    struct ScriptComponent
    {
        std::vector<ScriptSlot> scripts;
    };

    // A perspective camera; its view comes from the entity's TransformComponent.
    struct CameraComponent
    {
        f32 fov = 45.0f;  // vertical field of view, degrees
        f32 nearPlane = 0.1f;
        f32 farPlane = 100.0f;
        bool primary = true;  // the scene renders through the first primary camera
        bool showModel = true;
        bool showFrustum = true;
        f32 frustumMaxDistance = 25.0f;
    };

    // A directional light; the scene shades through the first one. direction points
    // the way the light travels.
    struct DirectionalLightComponent
    {
        glm::vec3 direction{ -0.5f, -1.0f, -0.3f };
        glm::vec3 color{ 1.0f };
        f32 intensity = 1.0f;
        f32 ambient = 0.15f;
    };

    // An omnidirectional light positioned at the entity's Transform translation, with
    // smooth distance falloff out to range. Culled into clusters by the light system.
    struct PointLightComponent
    {
        glm::vec3 color{ 1.0f };
        f32 intensity = 5.0f;
        f32 range = 10.0f;
    };

    // A cone light at the entity's Transform translation, aimed by direction. Falls
    // off by distance (range) and by angle between innerAngle and outerAngle (degrees).
    struct SpotLightComponent
    {
        glm::vec3 direction{ 0.0f, -1.0f, 0.0f };
        glm::vec3 color{ 1.0f };
        f32 intensity = 5.0f;
        f32 range = 10.0f;
        f32 innerAngle = 20.0f;  // full intensity inside this half-angle
        f32 outerAngle = 30.0f;  // zero past this half-angle
    };

    // A reflection probe at the entity's Transform translation. Captures a local cubemap of
    // the scene, prefilters it like the global IBL, and supplies specular ambient to meshes
    // inside its influence sphere (influenceRadius). Outside every probe, meshes fall back to
    // the global IBL. boxProjection re-projects the prefiltered reflection ray against the
    // influence box for parallax-correct local reflections (off = infinite-distance cube).
    struct ReflectionProbeComponent
    {
        f32 influenceRadius = 10.0f;   // sphere of effect around the probe origin
        f32 intensity = 1.0f;          // probe specular multiplier
        bool boxProjection = false;    // parallax-correct against the influence box
        glm::vec3 boxExtent{ 10.0f };  // half-extents for box projection (used when boxProjection)
        bool dirty = true;             // capture pending; set on add/edit, cleared after capture (runtime only)
    };

    auto transformMatrix(const TransformComponent& transform) -> glm::mat4
    {
        glm::mat4 translation = glm::translate(glm::mat4(1.0f), transform.translation);
        glm::mat4 rotation = glm::mat4_cast(glm::quat(transform.rotation));
        glm::mat4 scale = glm::scale(glm::mat4(1.0f), transform.scale);
        return translation * rotation * scale;
    }

    // A project asset (a model imported + baked to a mesh, or a texture). The catalog
    // maps these by id; each entry carries a human name (UTF-8, renameable) and the
    // relative path to the baked .smesh / copied texture under the asset root.
    enum class AssetType
    {
        Mesh,
        Texture,
        Other
    };

    struct AssetEntry
    {
        Uuid id;
        std::string name;
        AssetType type = AssetType::Mesh;
        std::string path;  // relative to the asset root
        std::string folder;
        bool hdr = false;     // texture: decode as linear float (.hdr); else sRGB RGBA8
        bool linear = false;  // texture: upload as a linear RGBA8 format (metallic-roughness), not sRGB
    };

    struct AssetCatalog
    {
        std::vector<AssetEntry> entries;
        std::vector<std::string> folders;
        std::unordered_map<u64, std::size_t> byId;  // id -> index into entries
    };

    auto findAsset(const AssetCatalog& catalog, Uuid id) -> const AssetEntry*
    {
        auto it = catalog.byId.find(id.value);
        if (it == catalog.byId.end())
        {
            return nullptr;
        }
        return &catalog.entries[it->second];
    }

    void putAsset(AssetCatalog& catalog, AssetEntry entry)
    {
        auto it = catalog.byId.find(entry.id.value);
        if (it != catalog.byId.end())
        {
            catalog.entries[it->second] = std::move(entry);
            return;
        }
        catalog.byId[entry.id.value] = catalog.entries.size();
        catalog.entries.push_back(std::move(entry));
    }

    auto renameAsset(AssetCatalog& catalog, Uuid id, std::string name) -> bool
    {
        auto it = catalog.byId.find(id.value);
        if (it == catalog.byId.end())
        {
            return false;
        }
        catalog.entries[it->second].name = std::move(name);
        return true;
    }

    // A name not already used by another entry (appends " (2)", " (3)", … on collision).
    auto uniqueName(const AssetCatalog& catalog, const std::string& base) -> std::string
    {
        bool taken = false;
        for (const AssetEntry& entry : catalog.entries)
        {
            if (entry.name == base)
            {
                taken = true;
            }
        }
        if (!taken)
        {
            return base;
        }
        for (u32 suffix = 2;; suffix = suffix + 1)
        {
            std::string candidate = base + " (" + std::to_string(suffix) + ")";
            bool clash = false;
            for (const AssetEntry& entry : catalog.entries)
            {
                if (entry.name == candidate)
                {
                    clash = true;
                }
            }
            if (!clash)
            {
                return candidate;
            }
        }
    }

    // How the visible sky background is produced. Color = a flat fill; Texture = an
    // equirectangular panorama asset; Procedural = the renderer's baked procedural-sky
    // environment cube (the same cube that feeds IBL, so background and lighting match).
    enum class SkyMode
    {
        Color,
        Texture,
        Procedural,
    };

    // Physically based atmosphere parameters (Hillaire 2020). When enabled, the
    // atmosphere LUT chain replaces the gradient ibl_skygen as the envCube source, so the
    // visible sky and the IBL convolutions both become the atmosphere. Coefficients are in
    // 1/Mm at sea level; lengths in km.
    struct AtmosphereSettings
    {
        bool enabled = false;
        f32 planetRadius = 6360.0f;
        f32 atmosphereHeight = 100.0f;
        glm::vec3 rayleighScattering{ 5.802f, 13.558f, 33.1f };
        f32 rayleighScaleHeight = 8.0f;
        f32 mieScattering = 3.996f;
        f32 mieScaleHeight = 1.2f;
        f32 mieAnisotropy = 0.8f;
        glm::vec3 ozoneAbsorption{ 0.650f, 1.881f, 0.085f };
        f32 sunDiskAngularRadius = 0.00465f;
        f32 sunDiskIntensity = 20.0f;
    };

    // Scene-wide environment / sky state. Global frame state (no transform, not picked,
    // not in the hierarchy), so it lives on the Scene rather than as an entity component.
    // The renderer resolves it into a SkyRenderSettings each frame (see renderScene).
    struct SceneEnvironment
    {
        SkyMode skyMode = SkyMode::Procedural;
        glm::vec3 clearColor{ 0.05f, 0.06f, 0.08f };  // Color mode + clear fallback
        Uuid skyTexture;                              // Texture mode panorama; 0 = none
        f32 skyIntensity = 1.0f;
        f32 skyRotation = 0.0f;  // yaw radians applied to the sky lookup
        f32 exposure = 1.0f;     // reserved; tonemap exposure is set via the renderer
        bool visible = true;
        bool useSkyForAmbient = true;    // drive fallback ambient from ambientColor below
        glm::vec3 ambientColor{ 1.0f };  // non-IBL fallback ambient tint
        f32 ambientIntensity = 0.15f;
        AtmosphereSettings atmosphere;  // physically based envCube source (off = gradient)
    };

    struct Scene
    {
        entt::registry registry;
        SceneEnvironment environment;
        const AssetCatalog* catalog = nullptr;  // borrowed; set per-frame by the client, not owned or serialized
    };

    // A lightweight, copyable handle — just an entt id. The Scene is always passed
    // explicitly to the free functions (Go-style: pass the world). An Entity is a
    // plain index, so it never dangles against a relocated Scene.
    struct Entity
    {
        entt::entity handle = entt::null;
    };

    auto valid(const Scene& scene, Entity entity) -> bool
    {
        return scene.registry.valid(entity.handle);
    }

    // Component access expressed as free generic functions (Go-style: generic
    // functions over the world + handle, not member templates on a class).
    template <typename C, typename... Args>
    auto addComponent(Scene& scene, Entity entity, Args&&... args) -> C&
    {
        return scene.registry.emplace<C>(entity.handle, std::forward<Args>(args)...);
    }

    template <typename C>
    auto getComponent(Scene& scene, Entity entity) -> C&
    {
        return scene.registry.get<C>(entity.handle);
    }

    template <typename C>
    auto hasComponent(const Scene& scene, Entity entity) -> bool
    {
        return scene.registry.all_of<C>(entity.handle);
    }

    template <typename C>
    void removeComponent(Scene& scene, Entity entity)
    {
        scene.registry.remove<C>(entity.handle);
    }

    auto createEntity(Scene& scene, std::string name) -> Entity
    {
        Entity entity{ scene.registry.create() };
        addComponent<IdComponent>(scene, entity, newUuid());
        addComponent<NameComponent>(scene, entity, std::move(name));
        addComponent<TransformComponent>(scene, entity);
        addComponent<RelationshipComponent>(scene, entity);
        return entity;
    }

    // Destroys the entity and its whole subtree. Descendants are gathered through the
    // children caches before any destroy, since registry.destroy invalidates handles.
    void destroyEntity(Scene& scene, Entity entity)
    {
        std::vector<entt::entity> doomed;
        auto gather = [&scene, &doomed](auto&& self, entt::entity handle) -> void
        {
            doomed.push_back(handle);
            if (scene.registry.all_of<RelationshipComponent>(handle))
            {
                for (entt::entity child : scene.registry.get<RelationshipComponent>(handle).children)
                {
                    self(self, child);
                }
            }
        };
        gather(gather, entity.handle);

        // Detach from the parent's children cache so it holds no dead handle.
        if (scene.registry.all_of<RelationshipComponent>(entity.handle))
        {
            entt::entity parent = scene.registry.get<RelationshipComponent>(entity.handle).parentHandle;
            if (parent != entt::null && scene.registry.all_of<RelationshipComponent>(parent))
            {
                std::erase(scene.registry.get<RelationshipComponent>(parent).children, entity.handle);
            }
        }

        for (entt::entity handle : doomed)
        {
            scene.registry.destroy(handle);
        }
    }

    // Iterate every entity carrying the given components.
    // The callback receives (Entity, C&...).
    template <typename... C, typename Fn>
    void forEach(Scene& scene, Fn&& fn)
    {
        auto view = scene.registry.view<C...>();
        for (entt::entity handle : view)
        {
            fn(Entity{ handle }, view.template get<C>(handle)...);
        }
    }

    // The entity carrying the given uuid, or a null handle. Cross-scene lookups must go
    // by uuid — entt handles can coincide between registries and alias silently.
    auto findEntityByUuid(Scene& scene, u64 uuid) -> Entity
    {
        Entity found{ entt::null };
        forEach<IdComponent>(scene,
                             [&](Entity entity, IdComponent& id)
                             {
                                 if (id.id.value == uuid)
                                 {
                                     found = entity;
                                 }
                             });
        return found;
    }

    // Rebuilds the parentHandle/children caches from the durable parent uuids. Entities
    // missing a RelationshipComponent (e.g. created by the raw loader path) get a default
    // root one, so every entity stays hierarchy-addressable. Dangling parent uuids,
    // self-parents, and parent cycles in the source data reset to root with a warning, so
    // the caches always form a forest. O(N); call after any structural change (load,
    // reparent, copy) before traversing the tree.
    void relinkHierarchy(Scene& scene)
    {
        std::unordered_map<u64, entt::entity> uuidToHandle;
        forEach<IdComponent>(scene,
                             [&](Entity entity, IdComponent& id)
                             {
                                 uuidToHandle.emplace(id.id.value, entity.handle);
                                 if (!hasComponent<RelationshipComponent>(scene, entity))
                                 {
                                     addComponent<RelationshipComponent>(scene, entity);
                                 }
                             });
        forEach<RelationshipComponent>(scene,
                                       [](Entity, RelationshipComponent& rel)
                                       {
                                           rel.parentHandle = entt::null;
                                           rel.children.clear();
                                       });
        forEach<RelationshipComponent>(
            scene,
            [&](Entity entity, RelationshipComponent& rel)
            {
                if (rel.parent.value == 0)
                {
                    return;
                }
                auto it = uuidToHandle.find(rel.parent.value);
                if (it == uuidToHandle.end() || it->second == entity.handle)
                {
                    logWarn(std::format("relationship parent {} {}; treating as root", rel.parent.value,
                                        it == uuidToHandle.end() ? "not found" : "is the entity itself"));
                    rel.parent = Uuid{ 0 };
                    return;
                }
                rel.parentHandle = it->second;
            });
        // Cut any parent cycle the source data carried (a hand-edited file can hold one;
        // setParent refuses to create them). A walk longer than the entity count must be
        // looping; resetting the current entity to root breaks that loop for all members.
        const std::size_t entityCount = uuidToHandle.size();
        forEach<RelationshipComponent>(
            scene,
            [&](Entity, RelationshipComponent& rel)
            {
                std::size_t steps = 0;
                for (entt::entity ancestor = rel.parentHandle; ancestor != entt::null;
                     ancestor = getComponent<RelationshipComponent>(scene, Entity{ ancestor }).parentHandle)
                {
                    steps = steps + 1;
                    if (steps > entityCount)
                    {
                        logWarn(
                            std::format("relationship parent {} forms a cycle; treating as root", rel.parent.value));
                        rel.parent = Uuid{ 0 };
                        rel.parentHandle = entt::null;
                        return;
                    }
                }
            });
        forEach<RelationshipComponent>(scene,
                                       [&](Entity entity, RelationshipComponent& rel)
                                       {
                                           if (rel.parentHandle != entt::null)
                                           {
                                               getComponent<RelationshipComponent>(scene, Entity{ rel.parentHandle })
                                                   .children.push_back(entity.handle);
                                           }
                                       });
        // Resolve skinned-mesh joint uuids to live handles with the same map; an
        // unresolved joint warns once here and deforms by identity in jointMatrices.
        forEach<SkinnedMeshComponent>(
            scene,
            [&](Entity, SkinnedMeshComponent& skin)
            {
                skin.boneHandles.assign(skin.bones.size(), entt::null);
                for (std::size_t i = 0; i < skin.bones.size(); i = i + 1)
                {
                    auto it = uuidToHandle.find(skin.bones[i].value);
                    if (it == uuidToHandle.end())
                    {
                        logWarn(std::format("skinned mesh joint {} not found; deforming with identity",
                                            skin.bones[i].value));
                        continue;
                    }
                    skin.boneHandles[i] = it->second;
                }
            });
    }

    // Exact world matrix composed by walking the parent chain. Used where the cached
    // per-frame matrix may lag a just-edited local transform (reparenting math).
    auto composeWorldMatrix(Scene& scene, Entity entity) -> glm::mat4
    {
        glm::mat4 local{ 1.0f };
        if (hasComponent<TransformComponent>(scene, entity))
        {
            local = transformMatrix(getComponent<TransformComponent>(scene, entity));
        }
        if (hasComponent<RelationshipComponent>(scene, entity))
        {
            entt::entity parent = getComponent<RelationshipComponent>(scene, entity).parentHandle;
            if (parent != entt::null)
            {
                return composeWorldMatrix(scene, Entity{ parent }) * local;
            }
        }
        return local;
    }

    // Cached world transform written by updateWorldTransforms; composes on a cache miss.
    auto worldMatrix(Scene& scene, Entity entity) -> glm::mat4
    {
        if (hasComponent<WorldTransformComponent>(scene, entity))
        {
            return getComponent<WorldTransformComponent>(scene, entity).matrix;
        }
        return composeWorldMatrix(scene, entity);
    }

    auto worldTranslation(Scene& scene, Entity entity) -> glm::vec3
    {
        return glm::vec3(worldMatrix(scene, entity)[3]);
    }

    // World-space rotation with scale divided out (gizmo Local axes, spot/camera aim).
    auto worldRotation(Scene& scene, Entity entity) -> glm::quat
    {
        const glm::mat4 world = worldMatrix(scene, entity);
        glm::vec3 scale{ glm::length(glm::vec3(world[0])), glm::length(glm::vec3(world[1])),
                         glm::length(glm::vec3(world[2])) };
        scale = glm::max(scale, glm::vec3(1e-8f));
        const glm::mat3 rotation{ glm::vec3(world[0]) / scale.x, glm::vec3(world[1]) / scale.y,
                                  glm::vec3(world[2]) / scale.z };
        return glm::quat_cast(rotation);
    }

    // Writes the cached WorldTransformComponent for every transformable entity, roots
    // first then down the children caches (entt views are unordered, so ordering never
    // comes from a view). Full mat4 composition preserves non-uniform parent scale, so
    // normalMatrix = transpose(inverse(mat3(world))) stays correct downstream. Runs once
    // per frame before render; relies on relinkHierarchy-fresh caches.
    void updateWorldTransforms(Scene& scene)
    {
        auto writeSubtree = [&scene](auto&& self, Entity entity, const glm::mat4& parentWorld) -> void
        {
            glm::mat4 world = parentWorld;
            if (hasComponent<TransformComponent>(scene, entity))
            {
                world = parentWorld * transformMatrix(getComponent<TransformComponent>(scene, entity));
                if (!hasComponent<WorldTransformComponent>(scene, entity))
                {
                    addComponent<WorldTransformComponent>(scene, entity);
                }
                getComponent<WorldTransformComponent>(scene, entity).matrix = world;
            }
            if (hasComponent<RelationshipComponent>(scene, entity))
            {
                for (entt::entity child : getComponent<RelationshipComponent>(scene, entity).children)
                {
                    self(self, Entity{ child }, world);
                }
            }
        };
        forEach<RelationshipComponent>(scene,
                                       [&](Entity entity, RelationshipComponent& rel)
                                       {
                                           if (rel.parentHandle == entt::null)
                                           {
                                               writeSubtree(writeSubtree, entity, glm::mat4(1.0f));
                                           }
                                       });
    }

    // Fills `out` with worldMatrix(bones[i]) * inverseBind[i] per joint — the matrices
    // the GPU skinning pass blends. Run after updateWorldTransforms so the cached bone
    // world matrices are current; an unresolved joint (relinkHierarchy already warned)
    // deforms by identity. The skinned node's own transform is deliberately absent:
    // per glTF, joints place the vertices entirely.
    void jointMatrices(Scene& scene, const SkinnedMeshComponent& skin, std::vector<glm::mat4>& out)
    {
        const std::size_t count = skin.bones.size();
        out.assign(count, glm::mat4(1.0f));
        for (std::size_t i = 0; i < count; i = i + 1)
        {
            if (i >= skin.boneHandles.size())
            {
                continue;
            }
            const entt::entity bone = skin.boneHandles[i];
            if (bone == entt::null || !scene.registry.valid(bone))
            {
                continue;
            }
            const glm::mat4 inverseBind = i < skin.inverseBind.size() ? skin.inverseBind[i] : glm::mat4(1.0f);
            out[i] = worldMatrix(scene, Entity{ bone }) * inverseBind;
        }
    }

    // Decomposes `local` into the entity's TransformComponent. TRS-only — under a sheared
    // source matrix the shear is lost (accepted: TransformComponent stores Euler + scale,
    // no shear). Returns false, leaving the transform untouched, when the matrix does not
    // decompose.
    auto setLocalFromMatrix(Scene& scene, Entity entity, const glm::mat4& local) -> bool
    {
        glm::vec3 scale;
        glm::quat orientation;
        glm::vec3 translation;
        glm::vec3 skew;
        glm::vec4 perspective;
        if (!glm::decompose(local, scale, orientation, translation, skew, perspective))
        {
            return false;
        }
        // Euler XYZ via the stable ZYX matrix extraction: glm::quat(eulerXYZ) composes
        // Rz*Ry*Rx, and glm::eulerAngles is numerically unstable at yaw +-90 degrees
        // (its asin/atan2 split poisons pitch/roll there).
        glm::vec3 euler;
        glm::extractEulerAngleZYX(glm::mat4_cast(orientation), euler.z, euler.y, euler.x);
        TransformComponent& transform = getComponent<TransformComponent>(scene, entity);
        transform.translation = translation;
        transform.rotation = euler;
        transform.scale = scale;
        return true;
    }

    // Engine-authoritative reparent. Refuses self-parenting and cycles (walks newParent's
    // ancestry); with keepWorld (the editor convention) the child's local TRS is rebased
    // so its world transform is unchanged. A null-handle newParent detaches to root.
    auto setParent(Scene& scene, Entity child, Entity newParent, bool keepWorld = true) -> Result<void>
    {
        if (!valid(scene, child))
        {
            return Err(std::string{ "invalid child entity" });
        }
        if (newParent.handle != entt::null)
        {
            if (!valid(scene, newParent))
            {
                return Err(std::string{ "invalid parent entity" });
            }
            if (newParent.handle == child.handle)
            {
                return Err(std::string{ "cannot parent an entity to itself" });
            }
            entt::entity ancestor = newParent.handle;
            while (ancestor != entt::null)
            {
                if (ancestor == child.handle)
                {
                    return Err(std::string{ "reparent would create a cycle" });
                }
                if (!scene.registry.all_of<RelationshipComponent>(ancestor))
                {
                    break;
                }
                ancestor = scene.registry.get<RelationshipComponent>(ancestor).parentHandle;
            }
        }
        if (!hasComponent<RelationshipComponent>(scene, child))
        {
            addComponent<RelationshipComponent>(scene, child);
        }

        const glm::mat4 childWorld = keepWorld ? composeWorldMatrix(scene, child) : glm::mat4{ 1.0f };

        getComponent<RelationshipComponent>(scene, child).parent =
            newParent.handle == entt::null ? Uuid{ 0 } : getComponent<IdComponent>(scene, newParent).id;

        if (keepWorld && hasComponent<TransformComponent>(scene, child))
        {
            const glm::mat4 parentWorld =
                newParent.handle == entt::null ? glm::mat4{ 1.0f } : composeWorldMatrix(scene, newParent);
            setLocalFromMatrix(scene, child, glm::inverse(parentWorld) * childWorld);
        }

        relinkHierarchy(scene);
        return {};
    }

    // The resolved primary camera: its view matrix + projection parameters. valid is
    // false when the scene has no primary camera. The projection is left un-flipped;
    // the renderer applies the Vulkan Y-flip where it samples, and the editor gizmo
    // consumes it as-is (one source of truth for both).
    struct CameraView
    {
        glm::mat4 view{ 1.0f };
        f32 fov = 45.0f;
        f32 nearPlane = 0.1f;
        f32 farPlane = 100.0f;
        bool valid = false;
    };

    auto primaryCamera(Scene& scene) -> CameraView
    {
        CameraView result;
        forEach<TransformComponent, CameraComponent>(scene,
                                                     [&](Entity entity, TransformComponent&, CameraComponent& camera)
                                                     {
                                                         if (result.valid || !camera.primary)
                                                         {
                                                             return;
                                                         }
                                                         // The world matrix composes the parent chain, so a parented
                                                         // camera views from its world placement (and inherits parent
                                                         // scale into the view basis).
                                                         result.view = glm::inverse(worldMatrix(scene, entity));
                                                         result.fov = camera.fov;
                                                         result.nearPlane = camera.nearPlane;
                                                         result.farPlane = camera.farPlane;
                                                         result.valid = true;
                                                     });
        return result;
    }

    // Un-flipped perspective projection for the resolved camera (GL clip convention).
    auto cameraProjection(const CameraView& camera, f32 aspect) -> glm::mat4
    {
        return glm::perspective(glm::radians(camera.fov), aspect, camera.nearPlane, camera.farPlane);
    }

    // glm <-> json use named fields; quat storage order is config-dependent, so
    // never serialize positionally.
    auto vec3ToJson(const glm::vec3& v) -> nlohmann::json
    {
        return nlohmann::json{ { "x", v.x }, { "y", v.y }, { "z", v.z } };
    }

    auto vec3FromJson(const nlohmann::json& j) -> glm::vec3
    {
        return glm::vec3{ jsonF32Or(j, "x", 0.0f), jsonF32Or(j, "y", 0.0f), jsonF32Or(j, "z", 0.0f) };
    }

    auto vec4ToJson(const glm::vec4& v) -> nlohmann::json
    {
        return nlohmann::json{ { "x", v.x }, { "y", v.y }, { "z", v.z }, { "w", v.w } };
    }

    auto vec4FromJson(const nlohmann::json& j) -> glm::vec4
    {
        return glm::vec4{ jsonF32Or(j, "x", 1.0f), jsonF32Or(j, "y", 1.0f), jsonF32Or(j, "z", 1.0f),
                          jsonF32Or(j, "w", 1.0f) };
    }

    auto nameComponentToJson(const NameComponent& c) -> nlohmann::json;
    auto nameComponentFromJson(NameComponent& c, const nlohmann::json& j) -> Result<void>;
    auto transformComponentToJson(const TransformComponent& t) -> nlohmann::json;
    auto transformComponentFromJson(TransformComponent& t, const nlohmann::json& j) -> Result<void>;
    auto meshComponentToJson(const MeshComponent& c) -> nlohmann::json;
    auto meshComponentFromJson(MeshComponent& c, const nlohmann::json& j) -> Result<void>;
    auto cameraComponentToJson(const CameraComponent& c) -> nlohmann::json;
    auto cameraComponentFromJson(CameraComponent& c, const nlohmann::json& j) -> Result<void>;
    auto materialComponentToJson(const MaterialComponent& c) -> nlohmann::json;
    auto materialComponentFromJson(MaterialComponent& c, const nlohmann::json& j) -> Result<void>;
    auto materialSetComponentToJson(const MaterialSetComponent& c) -> nlohmann::json;
    auto materialSetComponentFromJson(MaterialSetComponent& c, const nlohmann::json& j) -> Result<void>;
    auto scriptComponentToJson(const ScriptComponent& c) -> nlohmann::json;
    auto scriptComponentFromJson(ScriptComponent& c, const nlohmann::json& j) -> Result<void>;
    auto directionalLightComponentToJson(const DirectionalLightComponent& c) -> nlohmann::json;
    auto directionalLightComponentFromJson(DirectionalLightComponent& c, const nlohmann::json& j) -> Result<void>;
    auto pointLightComponentToJson(const PointLightComponent& c) -> nlohmann::json;
    auto pointLightComponentFromJson(PointLightComponent& c, const nlohmann::json& j) -> Result<void>;
    auto spotLightComponentToJson(const SpotLightComponent& c) -> nlohmann::json;
    auto spotLightComponentFromJson(SpotLightComponent& c, const nlohmann::json& j) -> Result<void>;
    auto reflectionProbeComponentToJson(const ReflectionProbeComponent& c) -> nlohmann::json;
    auto reflectionProbeComponentFromJson(ReflectionProbeComponent& c, const nlohmann::json& j) -> Result<void>;
    auto relationshipComponentToJson(const RelationshipComponent& c) -> nlohmann::json;
    auto relationshipComponentFromJson(RelationshipComponent& c, const nlohmann::json& j) -> Result<void>;
    auto boneComponentToJson(const BoneComponent& c) -> nlohmann::json;
    auto boneComponentFromJson(BoneComponent& c, const nlohmann::json& j) -> Result<void>;
    auto skinnedMeshComponentToJson(const SkinnedMeshComponent& c) -> nlohmann::json;
    auto skinnedMeshComponentFromJson(SkinnedMeshComponent& c, const nlohmann::json& j) -> Result<void>;
    auto environmentToJson(const SceneEnvironment& env) -> nlohmann::json;
    auto environmentFromJson(const nlohmann::json& j) -> SceneEnvironment;

    // ComponentTraits is a struct of std::function fields (a Go-interface itable);
    // every cross-cutting feature dispatches through it instead of a switch.
    //
    // Version history: 1 = entities only; 2 = adds the top-level "environment" block;
    // 3 = adds the per-entity Relationship component (durable parent uuid).
    // sceneFromJson migrates older documents: v1 defaults the environment, and a
    // pre-v3 document has no Relationship, so every entity loads as a root.
    inline constexpr int SceneVersion = 3;

    struct ComponentTraits
    {
        entt::id_type id = 0;  // == entt::type_hash<C>::value(); the storage() join key
        std::string name;      // stable JSON key + UI header, e.g. "Transform"
        bool removable = true;
        std::function<bool(Scene&, Entity)> has;
        std::function<void(Scene&, Entity)> addDefault;
        std::function<void(Scene&, Entity)> remove;
        std::function<void(Scene&, Entity, Scene&, Entity)> copyTo;  // clone src -> dst
        std::function<nlohmann::json(Scene&, Entity)> serialize;
        std::function<Result<void>(Scene&, Entity, const nlohmann::json&)> deserialize;
        std::function<void(Scene&, Entity)>
            drawInspector;  // opaque here; the host registers an empty body — the inspector is the React editor
    };

    struct ComponentRegistry
    {
        std::vector<ComponentTraits> rows;
        std::unordered_map<entt::id_type, std::size_t> byId;
        std::unordered_map<std::string, std::size_t> byName;
    };

    // Register a component ONCE. Synthesizes every closure from the existing
    // generic addComponent/getComponent/hasComponent/removeComponent. Adding a
    // new component type elsewhere = one call to this; zero edits to the rest.
    template <typename C>
    void registerComponent(ComponentRegistry& reg, std::string name, std::function<void(Scene&, Entity)> drawFn,
                           std::function<nlohmann::json(const C&)> toJson,
                           std::function<Result<void>(C&, const nlohmann::json&)> fromJson, bool removable = true)
    {
        ComponentTraits traits;
        traits.id = entt::type_hash<C>::value();
        traits.name = name;
        traits.removable = removable;
        traits.has = [](Scene& s, Entity e) -> auto { return hasComponent<C>(s, e); };
        traits.addDefault = [](Scene& s, Entity e) -> auto { addComponent<C>(s, e); };
        traits.remove = [](Scene& s, Entity e) -> auto { removeComponent<C>(s, e); };
        traits.copyTo = [](Scene& src, Entity from, Scene& dst, Entity to) -> auto
        {
            if (hasComponent<C>(src, from))
            {
                addComponent<C>(dst, to, getComponent<C>(src, from));
            }
        };
        traits.serialize = [toJson](Scene& s, Entity e) -> nlohmann::json { return toJson(getComponent<C>(s, e)); };
        traits.deserialize = [fromJson](Scene& s, Entity e, const nlohmann::json& j) -> Result<void>
        {
            if (!hasComponent<C>(s, e))
            {
                addComponent<C>(s, e);
            }
            return fromJson(getComponent<C>(s, e), j);
        };
        traits.drawInspector = std::move(drawFn);

        const std::size_t index = reg.rows.size();
        reg.byId[traits.id] = index;
        reg.byName[name] = index;
        reg.rows.push_back(std::move(traits));
    }

    auto findById(const ComponentRegistry& reg, entt::id_type id) -> const ComponentTraits*
    {
        auto it = reg.byId.find(id);
        if (it == reg.byId.end())
        {
            return nullptr;
        }
        return &reg.rows[it->second];
    }

    auto findByName(const ComponentRegistry& reg, const std::string& name) -> const ComponentTraits*
    {
        auto it = reg.byName.find(name);
        if (it == reg.byName.end())
        {
            return nullptr;
        }
        return &reg.rows[it->second];
    }

    // Scene& is non-const because entt views/storage iteration require it; these
    // functions do not logically mutate the scene.
    auto serializeEntity(ComponentRegistry& reg, Scene& scene, Entity entity) -> nlohmann::json
    {
        nlohmann::json components = nlohmann::json::object();
        for (auto&& [id, set] : scene.registry.storage())
        {
            if (!set.contains(entity.handle))
            {
                continue;
            }
            const ComponentTraits* traits = findById(reg, id);
            if (traits == nullptr)
            {
                continue;  // unregistered/internal storage (e.g. IdComponent) — skipped
            }
            components[traits->name] = traits->serialize(scene, entity);
        }
        return components;
    }

    auto deserializeEntity(ComponentRegistry& reg, Scene& scene, Entity entity, const nlohmann::json& components)
        -> Result<void>
    {
        for (auto it = components.begin(); it != components.end(); ++it)
        {
            const ComponentTraits* traits = findByName(reg, it.key());
            if (traits == nullptr)
            {
                logWarn(std::format("unknown component '{}', skipping", it.key()));
                continue;
            }
            auto result = traits->deserialize(scene, entity, it.value());
            if (!result)
            {
                return Err(std::format("{}: {}", it.key(), result.error()));
            }
        }
        return {};
    }

    // Serializes the scene to a `{version, entities:[{id,components}]}` document (no file
    // IO), so it can be embedded in a larger project document.
    auto sceneToJson(ComponentRegistry& reg, Scene& scene) -> nlohmann::json
    {
        nlohmann::json doc;
        doc["version"] = SceneVersion;
        doc["environment"] = environmentToJson(scene.environment);
        doc["entities"] = nlohmann::json::array();
        forEach<IdComponent>(scene,
                             [&](Entity entity, IdComponent& id)
                             {
                                 nlohmann::json entry;
                                 entry["id"] = uuidToJson(id.id.value);
                                 entry["components"] = serializeEntity(reg, scene, entity);
                                 doc["entities"].push_back(std::move(entry));
                             });
        return doc;
    }

    // Replaces the scene's entities from a `sceneToJson` document.
    auto sceneFromJson(ComponentRegistry& reg, Scene& scene, const nlohmann::json& doc) -> Result<void>
    {
        if (!doc.is_object())
        {
            return Err(std::string{ "scene root is not an object" });
        }
        const int version = static_cast<int>(jsonU64Or(doc, "version", 0));
        if (version < 1 || version > SceneVersion)
        {
            return Err(std::format("unsupported scene version {}", version));
        }
        if (!doc.contains("entities") || !doc["entities"].is_array())
        {
            return Err(std::string{ "scene missing 'entities' array" });
        }

        // v1 has no "environment" block; environmentFromJson defaults it. v2+ carries one.
        scene.environment = environmentFromJson(doc.contains("environment") ? doc["environment"] : nlohmann::json{});

        scene.registry.clear();

        // Create entities preserving uuids (NOT createEntity, which mints fresh ones)
        // and deserialize their components.
        for (const nlohmann::json& entry : doc["entities"])
        {
            if (!entry.is_object())
            {
                return Err(std::string{ "entity entry is not an object" });
            }
            const u64 uuid = jsonU64Or(entry, "id", 0);
            if (uuid == 0)
            {
                return Err(std::string{ "entity missing 'id'" });
            }
            entt::entity handle = scene.registry.create();
            scene.registry.emplace<IdComponent>(handle, Uuid{ uuid });

            if (entry.contains("components") && entry["components"].is_object())
            {
                Result<void> result = deserializeEntity(reg, scene, Entity{ handle }, entry["components"]);
                if (!result)
                {
                    return Err(result.error());
                }
            }
        }

        // Resolve cross-entity references (uuid -> live handle). Parent uuids may point
        // at entities created later in the array, so resolution runs only after the
        // whole loop. relinkHierarchy also defaults a Relationship onto pre-v3 entities
        // (root) and downgrades dangling parents to root with a warning.
        relinkHierarchy(scene);
        return {};
    }

    auto writeScene(ComponentRegistry& reg, Scene& scene, const std::string& path) -> Result<void>
    {
        std::ofstream out(path);
        if (!out)
        {
            return Err(std::format("cannot open '{}' for writing", path));
        }
        out << dumpJson(sceneToJson(reg, scene), 2);
        out.flush();
        if (!out)
        {
            return Err(std::format("write failed for '{}'", path));
        }
        return {};
    }

    auto readScene(ComponentRegistry& reg, Scene& scene, const std::string& path) -> Result<void>
    {
        std::ifstream in(path);
        if (!in)
        {
            return Err(std::format("cannot open '{}'", path));
        }
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

        auto doc = parseJson(text);
        if (!doc)
        {
            return Err(std::format("'{}': {}", path, doc.error()));
        }
        return sceneFromJson(reg, scene, *doc);
    }

    // Headless round-trip check: build a registry, populate a scene, write + read
    // it back, and confirm the data survives. Replaces the old ECS smoke test.
    void runSceneSerializationSelfTest()
    {
        ComponentRegistry reg;
        registerComponent<NameComponent>(
            reg, "Name", [](Scene&, Entity) {}, nameComponentToJson, nameComponentFromJson, false);
        registerComponent<TransformComponent>(
            reg, "Transform", [](Scene&, Entity) {}, transformComponentToJson, transformComponentFromJson, false);

        Scene scene;
        createEntity(scene, "Camera");
        Entity cube = createEntity(scene, "Cube");
        getComponent<TransformComponent>(scene, cube).translation = glm::vec3(1.0f, 2.0f, 3.0f);

        const std::string path = "/tmp/saffron_scene_selftest.json";
        auto wrote = writeScene(reg, scene, path);
        if (!wrote)
        {
            logError(std::format("scene self-test write failed: {}", wrote.error()));
            return;
        }

        Scene loaded;
        auto read = readScene(reg, loaded, path);
        if (!read)
        {
            logError(std::format("scene self-test read failed: {}", read.error()));
            return;
        }

        u32 count = 0;
        glm::vec3 cubePos{ 0.0f };
        forEach<NameComponent, TransformComponent>(loaded,
                                                   [&](Entity, NameComponent& name, TransformComponent& transform)
                                                   {
                                                       count = count + 1;
                                                       if (name.name == "Cube")
                                                       {
                                                           cubePos = transform.translation;
                                                       }
                                                   });
        logInfo(std::format("scene round-trip: {} entities, cube at ({:.1f}, {:.1f}, {:.1f})", count, cubePos.x,
                            cubePos.y, cubePos.z));

        // Hierarchy serialization: the durable parent uuid survives a round trip, the
        // post-loop resolve pass rebuilds the caches regardless of entity order, and
        // older or corrupt documents migrate clean.
        registerComponent<RelationshipComponent>(
            reg, "Relationship", [](Scene&, Entity) {}, relationshipComponentToJson, relationshipComponentFromJson,
            false);

        u32 failures = 0;
        auto expect = [&failures](bool condition, std::string_view what)
        {
            if (!condition)
            {
                failures = failures + 1;
                logError(std::format("scene serde self-test failed: {}", what));
            }
        };
        auto findByEntityName = [](Scene& s, std::string_view name) -> Entity
        {
            Entity found{ entt::null };
            forEach<NameComponent>(s,
                                   [&](Entity e, NameComponent& n)
                                   {
                                       if (n.name == name)
                                       {
                                           found = e;
                                       }
                                   });
            return found;
        };

        Scene tree;
        Entity root = createEntity(tree, "Root");
        Entity leaf = createEntity(tree, "Leaf");
        getComponent<TransformComponent>(tree, root).translation = glm::vec3(5.0f, 0.0f, 0.0f);
        static_cast<void>(setParent(tree, leaf, root, false));

        nlohmann::json doc = sceneToJson(reg, tree);

        Scene loadedTree;
        expect(sceneFromJson(reg, loadedTree, doc).has_value(), "hierarchy doc loads");
        {
            Entity loadedRoot = findByEntityName(loadedTree, "Root");
            Entity loadedLeaf = findByEntityName(loadedTree, "Leaf");
            expect(getComponent<RelationshipComponent>(loadedTree, loadedLeaf).parentHandle == loadedRoot.handle,
                   "loaded leaf resolves its parent handle");
            const std::vector<entt::entity>& kids =
                getComponent<RelationshipComponent>(loadedTree, loadedRoot).children;
            expect(std::find(kids.begin(), kids.end(), loadedLeaf.handle) != kids.end(),
                   "loaded root lists the leaf as child");
        }

        // A child entry may precede its parent in the array; resolution is post-loop.
        nlohmann::json reversed = doc;
        std::reverse(reversed["entities"].begin(), reversed["entities"].end());
        Scene reversedTree;
        expect(sceneFromJson(reg, reversedTree, reversed).has_value(), "reversed hierarchy doc loads");
        {
            Entity loadedRoot = findByEntityName(reversedTree, "Root");
            Entity loadedLeaf = findByEntityName(reversedTree, "Leaf");
            expect(getComponent<RelationshipComponent>(reversedTree, loadedLeaf).parentHandle == loadedRoot.handle,
                   "child-before-parent order still resolves");
        }

        // A v2 document has no Relationship key; every entity migrates to root.
        nlohmann::json v2 = doc;
        v2["version"] = 2;
        for (nlohmann::json& entry : v2["entities"])
        {
            entry["components"].erase("Relationship");
        }
        Scene migrated;
        expect(sceneFromJson(reg, migrated, v2).has_value(), "v2 doc loads");
        {
            u32 migratedRoots = 0;
            u32 migratedTotal = 0;
            forEach<RelationshipComponent>(migrated,
                                           [&](Entity, RelationshipComponent& rel)
                                           {
                                               migratedTotal = migratedTotal + 1;
                                               if (rel.parent.value == 0 && rel.parentHandle == entt::null)
                                               {
                                                   migratedRoots = migratedRoots + 1;
                                               }
                                           });
            expect(migratedTotal == 2 && migratedRoots == 2, "v2 entities migrate to roots");
        }

        // A skinned mesh round-trips by uuid: bones + inverseBind survive, and the
        // resolve pass rebuilds boneHandles to the live joints.
        registerComponent<SkinnedMeshComponent>(
            reg, "SkinnedMesh", [](Scene&, Entity) {}, skinnedMeshComponentToJson, skinnedMeshComponentFromJson, true);
        {
            Scene rig;
            Entity boneA = createEntity(rig, "BoneA");
            Entity boneB = createEntity(rig, "BoneB");
            static_cast<void>(setParent(rig, boneB, boneA, false));
            Entity skinnedNode = createEntity(rig, "Skinned");
            SkinnedMeshComponent& skin = addComponent<SkinnedMeshComponent>(rig, skinnedNode);
            skin.mesh = Uuid{ 777 };
            skin.rootBone = getComponent<IdComponent>(rig, boneA).id;
            skin.bones = { getComponent<IdComponent>(rig, boneA).id, getComponent<IdComponent>(rig, boneB).id };
            skin.inverseBind = { glm::translate(glm::mat4(1.0f), glm::vec3(-1.0f, 0.0f, 0.0f)),
                                 glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -2.0f, 0.0f)) };
            relinkHierarchy(rig);

            nlohmann::json rigDoc = sceneToJson(reg, rig);
            Scene rigLoaded;
            expect(sceneFromJson(reg, rigLoaded, rigDoc).has_value(), "skinned rig doc loads");
            Entity loadedSkinned = findByEntityName(rigLoaded, "Skinned");
            Entity loadedBoneB = findByEntityName(rigLoaded, "BoneB");
            SkinnedMeshComponent& loadedSkin = getComponent<SkinnedMeshComponent>(rigLoaded, loadedSkinned);
            expect(loadedSkin.mesh.value == 777 && loadedSkin.bones.size() == 2 && loadedSkin.inverseBind.size() == 2,
                   "skinned mesh uuids + inverse binds survive the round trip");
            expect(glm::abs(loadedSkin.inverseBind[1][3][1] + 2.0f) < 1e-6f,
                   "inverse bind matrix values survive the round trip");
            expect(loadedSkin.boneHandles.size() == 2 && loadedSkin.boneHandles[1] == loadedBoneB.handle,
                   "loaded skinned mesh resolves its joints to live handles");
        }

        // A dangling parent uuid downgrades to root (with a warning), never a crash.
        nlohmann::json dangling = doc;
        for (nlohmann::json& entry : dangling["entities"])
        {
            nlohmann::json& components = entry["components"];
            if (components.contains("Relationship") && components["Relationship"]["parent"].get<std::string>() != "0")
            {
                components["Relationship"]["parent"] = "424242";
            }
        }
        Scene orphaned;
        expect(sceneFromJson(reg, orphaned, dangling).has_value(), "dangling-parent doc loads");
        {
            Entity loadedLeaf = findByEntityName(orphaned, "Leaf");
            RelationshipComponent& rel = getComponent<RelationshipComponent>(orphaned, loadedLeaf);
            expect(rel.parent.value == 0 && rel.parentHandle == entt::null, "dangling parent resolves to root");
        }

        if (failures == 0)
        {
            logInfo("scene hierarchy serde: round-trip, order, v2 migration, dangling parent all pass");
        }
    }

    // Headless hierarchy check: parenting, world-transform propagation, the cycle and
    // self-parent guards, world-preserving reparent, and recursive destroy.
    void runSceneHierarchySelfTest()
    {
        u32 failures = 0;
        auto expect = [&failures](bool condition, std::string_view what)
        {
            if (!condition)
            {
                failures = failures + 1;
                logError(std::format("hierarchy self-test failed: {}", what));
            }
        };
        auto nearEqual = [](const glm::mat4& a, const glm::mat4& b) -> bool
        {
            for (int col = 0; col < 4; col = col + 1)
            {
                for (int row = 0; row < 4; row = row + 1)
                {
                    if (glm::abs(a[col][row] - b[col][row]) > 1e-4f)
                    {
                        return false;
                    }
                }
            }
            return true;
        };

        Scene scene;
        Entity parent = createEntity(scene, "Parent");
        Entity child = createEntity(scene, "Child");
        Entity grandchild = createEntity(scene, "Grandchild");
        getComponent<TransformComponent>(scene, parent).translation = glm::vec3(10.0f, 0.0f, 0.0f);

        // Parent-before-child composition: locals set after parenting (keepWorld=false
        // keeps them as authored), so the pass must compose parent * local.
        expect(setParent(scene, child, parent, false).has_value(), "setParent child -> parent");
        expect(setParent(scene, grandchild, child, false).has_value(), "setParent grandchild -> child");
        getComponent<TransformComponent>(scene, child).translation = glm::vec3(0.0f, 2.0f, 0.0f);
        getComponent<TransformComponent>(scene, grandchild).translation = glm::vec3(0.0f, 0.0f, 3.0f);
        updateWorldTransforms(scene);
        expect(nearEqual(worldMatrix(scene, child),
                         worldMatrix(scene, parent) * transformMatrix(getComponent<TransformComponent>(scene, child))),
               "child world == parent world * child local");
        expect(glm::distance(worldTranslation(scene, grandchild), glm::vec3(10.0f, 2.0f, 3.0f)) < 1e-4f,
               "grandchild world translation");

        // Guards: parenting an ancestor under its descendant, or an entity under itself.
        expect(!setParent(scene, parent, grandchild).has_value(), "cycle guard");
        expect(!setParent(scene, child, child).has_value(), "self-parent guard");

        // keepWorld rebase: move grandchild under a rotated, scaled mover; the world
        // transform must not change while the local TRS does.
        Entity mover = createEntity(scene, "Mover");
        TransformComponent& moverLocal = getComponent<TransformComponent>(scene, mover);
        moverLocal.translation = glm::vec3(-4.0f, 1.0f, 0.5f);
        moverLocal.rotation = glm::vec3(0.0f, 1.5707963f, 0.0f);
        moverLocal.scale = glm::vec3(2.0f);
        const glm::mat4 before = composeWorldMatrix(scene, grandchild);
        expect(setParent(scene, grandchild, mover).has_value(), "setParent grandchild -> mover");
        expect(nearEqual(composeWorldMatrix(scene, grandchild), before), "keepWorld preserves world transform");
        expect(glm::distance(getComponent<TransformComponent>(scene, grandchild).translation,
                             glm::vec3(0.0f, 0.0f, 3.0f)) > 1e-3f,
               "keepWorld rebases the local transform");

        // Same rebase under a generic (non-axis-aligned) parent rotation.
        Entity mover2 = createEntity(scene, "Mover2");
        TransformComponent& mover2Local = getComponent<TransformComponent>(scene, mover2);
        mover2Local.translation = glm::vec3(2.0f, -3.0f, 1.0f);
        mover2Local.rotation = glm::vec3(0.4f, 0.9f, -0.3f);
        mover2Local.scale = glm::vec3(1.5f);
        const glm::mat4 beforeGeneric = composeWorldMatrix(scene, grandchild);
        expect(setParent(scene, grandchild, mover2).has_value(), "setParent grandchild -> mover2");
        expect(nearEqual(composeWorldMatrix(scene, grandchild), beforeGeneric),
               "keepWorld preserves world transform under a generic rotation");

        // Recursive destroy takes the subtree; the reparented grandchild survives.
        destroyEntity(scene, parent);
        expect(!valid(scene, parent) && !valid(scene, child), "destroy removes the subtree");
        expect(valid(scene, grandchild), "reparented entity survives its old ancestor's destroy");
        u32 dangling = 0;
        forEach<RelationshipComponent>(scene,
                                       [&](Entity, RelationshipComponent& rel)
                                       {
                                           for (entt::entity c : rel.children)
                                           {
                                               if (!scene.registry.valid(c))
                                               {
                                                   dangling = dangling + 1;
                                               }
                                           }
                                       });
        expect(dangling == 0, "no children cache holds a destroyed handle");

        u32 roots = 0;
        u32 total = 0;
        forEach<RelationshipComponent>(scene,
                                       [&](Entity, RelationshipComponent& rel)
                                       {
                                           total = total + 1;
                                           if (rel.parent.value == 0 && rel.parentHandle == entt::null)
                                           {
                                               roots = roots + 1;
                                           }
                                       });
        expect(total == 3 && roots == 2, "expected mover + mover2 + grandchild with two roots");

        // A parented primary camera views from its world placement.
        Entity camParent = createEntity(scene, "CamParent");
        getComponent<TransformComponent>(scene, camParent).translation = glm::vec3(3.0f, 4.0f, 5.0f);
        Entity cam = createEntity(scene, "Camera");
        addComponent<CameraComponent>(scene, cam);
        expect(setParent(scene, cam, camParent, false).has_value(), "setParent camera -> parent");
        getComponent<TransformComponent>(scene, cam).translation = glm::vec3(1.0f, 0.0f, 0.0f);
        updateWorldTransforms(scene);
        const CameraView view = primaryCamera(scene);
        expect(view.valid && glm::distance(glm::vec3(glm::inverse(view.view)[3]), glm::vec3(4.0f, 4.0f, 5.0f)) < 1e-4f,
               "parented camera views from its world position");

        // Research gate (CPU half): jointMatrices() must produce worldBone * inverseBind
        // in joint order, identity at bind pose, and never compose the skinned node's
        // own transform.
        Entity jointRoot = createEntity(scene, "JointRoot");
        Entity jointTip = createEntity(scene, "JointTip");
        getComponent<TransformComponent>(scene, jointRoot).translation = glm::vec3(1.0f, 0.0f, 0.0f);
        expect(setParent(scene, jointTip, jointRoot, false).has_value(), "setParent jointTip -> jointRoot");
        getComponent<TransformComponent>(scene, jointTip).translation = glm::vec3(0.0f, 3.0f, 0.0f);
        Entity skinnedNode = createEntity(scene, "SkinnedNode");
        getComponent<TransformComponent>(scene, skinnedNode).translation = glm::vec3(50.0f, 0.0f, 0.0f);
        SkinnedMeshComponent& skin = addComponent<SkinnedMeshComponent>(scene, skinnedNode);
        skin.bones = { getComponent<IdComponent>(scene, jointRoot).id, getComponent<IdComponent>(scene, jointTip).id };
        skin.inverseBind = { glm::translate(glm::mat4(1.0f), glm::vec3(-1.0f, 0.0f, 0.0f)),
                             glm::translate(glm::mat4(1.0f), glm::vec3(-1.0f, -3.0f, 0.0f)) };
        relinkHierarchy(scene);
        updateWorldTransforms(scene);
        std::vector<glm::mat4> palette;
        jointMatrices(scene, skin, palette);
        expect(palette.size() == 2 && nearEqual(palette[0], glm::mat4(1.0f)) && nearEqual(palette[1], glm::mat4(1.0f)),
               "bind-pose joint matrices are identity");
        getComponent<TransformComponent>(scene, jointTip).translation = glm::vec3(0.0f, 5.0f, 0.0f);
        updateWorldTransforms(scene);
        jointMatrices(scene, skin, palette);
        const glm::vec4 movedVertex = palette[1] * glm::vec4(1.0f, 3.0f, 0.0f, 1.0f);
        expect(glm::distance(glm::vec3(movedVertex), glm::vec3(1.0f, 5.0f, 0.0f)) < 1e-4f,
               "a tip-bound vertex follows the moved joint");

        if (failures == 0)
        {
            logInfo("hierarchy self-test: all checks passed");
        }
    }
}
