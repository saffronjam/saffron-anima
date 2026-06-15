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
#include <unordered_set>
#include <utility>
#include <vector>

export module Saffron.Scene;

import Saffron.Core;
import Saffron.Json;

export namespace sa
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

    // Drives a skinned rig from an AssetType::Animation clip. Dumb data — the evaluator
    // and serde live elsewhere (Saffron.Animation / the component registry). Sits beside
    // SkinnedMeshComponent on the mesh entity; the import attaches it for any rig that
    // ships clips. previewInEdit/pingForward and the transition trio are runtime state.
    struct AnimationPlayerComponent
    {
        Uuid clip;        // the AssetType::Animation catalog entry to play
        f32 time = 0.0f;  // playhead, seconds
        f32 speed = 1.0f;
        enum class Wrap : u8
        {
            Once,
            Loop,
            PingPong
        } wrap = Wrap::Loop;
        bool playing = false;        // advance time? (the game loop in Play / the timeline in Edit)
        bool previewInEdit = false;  // runtime: is this entity previewed in Edit? (serialize as false)
        bool pingForward = true;     // runtime: ping-pong direction state
        // How an active clip switch blends: inertialize (decay the pose offset, the default)
        // or sustain a two-clip cross-fade. loopBlend > 0 inertializes across a Loop wrap.
        enum class Transition : u8
        {
            Inertialize,
            CrossFade
        } transitionMode = Transition::Inertialize;
        f32 loopBlend = 0.0f;  // seconds of wrap-blend for a Loop clip (0 = hard cut)
        Uuid prevClip;         // active-transition state (runtime; idle/0 at rest)
        f32 transition = 0.0f;
        f32 transitionDuration = 0.0f;
    };

    // Runtime-only (never serialized): the animated local TRS the evaluator writes onto a
    // driven bone each frame. World-transform composition prefers it over the bone's
    // TransformComponent, so the authored rest pose stays untouched and Edit preview is
    // non-destructive. Removed from a bone when its rig stops animating (reverts to rest).
    struct PoseOverrideComponent
    {
        glm::vec3 translation{ 0.0f };
        glm::quat rotation{ 1.0f, 0.0f, 0.0f, 0.0f };
        glm::vec3 scale{ 1.0f };
    };

    // One leg/arm chain for kinematic foot IK: joint indices into SkinnedMeshComponent.bones
    // (upper→mid→end, e.g. thigh→shin→foot) plus the pole vector that orients the knee plane.
    struct FootChain
    {
        i32 upper = -1;
        i32 mid = -1;
        i32 end = -1;
        glm::vec3 poleVector{ 0.0f, 0.0f, 1.0f };
    };

    // Foot-IK config on the rig entity (beside SkinnedMeshComponent). When enabled, the
    // animation evaluator runs solveTwoBoneIk per chain and feeds the result through the
    // PoseBuffer blend layer (override_/weight), never the bones' TransformComponents.
    // v1 ground is a horizontal plane at `groundHeight`; true terrain waits for physics.
    struct FootIkComponent
    {
        bool enabled = false;
        f32 groundHeight = 0.0f;
        std::vector<FootChain> chains;
    };

    // Reserved per-bone metadata for the eventual Jolt powered-ragdoll (UE-PhAT /
    // Jolt-RagdollSettings shaped). No runtime use this phase — purely the schema the physics
    // phase will read to build collider bodies + constraints. Authored once, mapped 1:1 later.
    struct BonePhysics
    {
        glm::vec3 shapeHalfExtents{ 0.0f };  // capsule/box collider half-size
        f32 mass = 1.0f;
        enum class Joint : u8
        {
            Fixed,
            Hinge,
            SwingTwist,
            Free
        } joint = Joint::SwingTwist;
        glm::vec3 swingTwistLimits{ 0.0f };  // radians
        f32 driveStiffness = 0.0f;
        f32 driveDamping = 0.0f;
        f32 driveMaxForce = 0.0f;  // PD motor gains
    };

    // Optional sidecar on the rig entity: a parallel array to SkinnedMeshComponent.bones of
    // the reserved physics metadata above. Serialized through the component path; inert.
    struct BonePhysicsComponent
    {
        std::vector<BonePhysics> bones;
    };

    // A simulated body. Motion type decides how the solver treats it: Dynamic moves under forces,
    // Kinematic is script/animation-driven (infinite mass, pushes dynamics), Static never moves.
    // A ColliderComponent WITHOUT a RigidbodyComponent is an implicit Static body (floors/walls);
    // with one present, this motion type wins. Per-axis locks freeze a DOF on a Dynamic body.
    struct RigidbodyComponent
    {
        enum class Motion : u8
        {
            Static,
            Kinematic,
            Dynamic
        } motion = Motion::Dynamic;
        f32 mass = 1.0f;            // kg; ignored for Static/Kinematic
        f32 linearDamping = 0.05f;  // per-second velocity decay
        f32 angularDamping = 0.05f;
        f32 gravityFactor = 1.0f;          // 0 = float, 1 = full gravity
        glm::bvec3 lockPosition{ false };  // freeze X/Y/Z translation
        glm::bvec3 lockRotation{ false };  // freeze X/Y/Z rotation
        i32 collisionLayer = 0;            // index into the (later) layer table; 0 = the default moving layer
    };

    // Friction/restitution for a collider's surface (the layers phase makes these load-bearing).
    struct PhysicsMaterial
    {
        f32 friction = 0.5f;     // 0 = ice, 1 = rubber
        f32 restitution = 0.0f;  // bounciness 0..1
    };

    // The collision geometry for an entity. Box-only this phase (Sphere/Capsule/ConvexHull/Mesh
    // land with the shapes phase). Dimensions are interpreted per-shape; auto-fit to the entity
    // mesh AABB on add, editable after. `offset` places the shape in the body's local space.
    struct ColliderComponent
    {
        enum class Shape : u8
        {
            Box,
            Sphere,
            Capsule,
            ConvexHull,
            Mesh
        } shape = Shape::Box;
        glm::vec3 halfExtents{ 0.5f };  // Box: half-size; radius/height packed for other shapes (later)
        Uuid sourceMesh;                // ConvexHull/Mesh cook source; 0 = none (Box ignores it)
        glm::vec3 offset{ 0.0f };       // local-space shape centre offset
        PhysicsMaterial material;
        bool isSensor = false;  // trigger volume: reports overlaps, no contact response (later)
    };

    // Opt a SkinnedMesh rig into kinematic-bone physics: each driven joint gets a kinematic body that
    // follows the animated pose each step, so a moving character shoves the world (binding mode b,
    // animation -> physics; no pose write-back — that is the ragdoll). Per-bone collider sizes come
    // from BonePhysicsComponent.bones[i].shapeHalfExtents, auto-fit on add (editable after).
    struct KinematicBonesComponent
    {
        bool enabled = true;
        std::vector<i32> driven;  // indices into SkinnedMeshComponent.bones; empty = every joint
    };

    // Marks an entity as a walking capsule character driven by a Jolt CharacterVirtual. The capsule
    // is the entity's ColliderComponent (Shape::Capsule) — this carries only movement params. The
    // controller drives the entity-root TransformComponent each step; animation plays independently
    // on top (binding mode a). An entity with this never becomes a static body (the capsule is the
    // CharacterVirtual's, not a world body).
    struct CharacterControllerComponent
    {
        f32 maxSpeed = 4.0f;            // horizontal walk-speed cap, m/s
        f32 maxSlopeAngle = 0.785398f;  // radians (~45°); steeper ground is a wall, not floor
        f32 maxStepHeight = 0.3f;       // ledges/stairs up to this are stepped over
        f32 gravityFactor = 1.0f;       // scales world gravity applied each step
        // Runtime state (serialize as zero): the desired horizontal velocity move-character writes,
        // the vertical velocity the controller integrates, and the last step's ground state.
        glm::vec3 desiredVelocity{ 0.0f };
        f32 verticalVelocity = 0.0f;
        bool onGround = false;
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
        bool unlit = false;     // skip lighting (albedo * base color only) — a distinct PSO
        Uuid normalTexture;     // tangent-space normal map (OpenGL +Y convention)
        Uuid occlusionTexture;  // ambient occlusion (AO in R)
        Uuid emissiveTexture;   // emissive map (modulates the emissive factor)
        Uuid heightTexture;     // height/displacement (R) for parallax occlusion mapping
        f32 normalStrength = 1.0f;
        glm::vec2 uvTiling{ 1.0f };
        glm::vec2 uvOffset{ 0.0f };
        f32 heightScale = 0.05f;
        bool alphaClip = false;  // masked: discard fragments below alphaCutoff
        f32 alphaCutoff = 0.5f;
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
        Uuid normalTexture;
        Uuid occlusionTexture;
        Uuid emissiveTexture;
        Uuid heightTexture;
        f32 normalStrength = 1.0f;
        glm::vec2 uvTiling{ 1.0f };
        glm::vec2 uvOffset{ 0.0f };
        f32 heightScale = 0.05f;
        bool alphaClip = false;
        f32 alphaCutoff = 0.5f;
    };

    // An ordered material table for a mesh with more than one source material; each
    // Submesh.materialSlot indexes `slots`. Supersedes MaterialComponent when present.
    // Single-material meshes keep using MaterialComponent instead.
    struct MaterialSetComponent
    {
        std::vector<MaterialSlot> slots;
    };

    // References a shared .smat material asset by id. Takes precedence over the inline
    // MaterialComponent / MaterialSetComponent when present (edit-once-propagate). A missing
    // or zero id falls back to the built-in default material at resolve time.
    struct MaterialAssetComponent
    {
        Uuid material;
    };

    // Marks an entity (the root of an expanded model) as an instance of a `.smodel` asset. Lets the
    // editor show it as a model instance and lets reimport find live instances. Mesh/material refs
    // live on the usual components as `(sub-id)` resolved through the container.
    struct ModelInstanceComponent
    {
        Uuid modelId;
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
        f32 frustumMaxDistance = 10.0f;
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
        Other,
        Animation,
        Material,
        Model  // a `.smodel` container (parent of its embedded mesh/material/texture sub-assets)
    };

    /// How a texture's bytes are interpreted on upload. Recovered from a container chunk flag (embedded)
    /// or a `.smeta` sidecar (standalone). `Auto` defers the choice to a heuristic at scan time.
    enum class Colorspace : u8
    {
        Auto,
        Srgb,
        Linear,
        Hdr
    };

    struct AssetEntry
    {
        Uuid id;  // for a sub-asset, the stable sub-id (unique across the catalog)
        std::string name;
        AssetType type = AssetType::Mesh;
        std::string path;  // sub-asset: the owning .smodel's path; standalone: its own file
        std::string folder;
        bool hdr = false;     // texture: decode as linear float (.hdr); else sRGB RGBA8
        bool linear = false;  // texture: upload as a linear RGBA8 format (metallic-roughness), not sRGB
        f32 duration = 0.0f;  // animation: clip length in seconds (0 for non-animation entries)
        i32 tracks = 0;       // animation: animated joint-channel count (0 for non-animation entries)
        bool rigged = false;  // belongs to a rigged .smodel (the container has a skin); scan-derived
        Uuid container;       // 0 = standalone file; else the owning model's id
        i32 chunk = -1;       // TOC chunk index inside the container (-1 = standalone / n/a)
        Colorspace colorspace = Colorspace::Auto;  // texture: how its bytes are interpreted on upload
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

    // The entity that carries a model instance's rig — the first descendant with a
    // SkinnedMeshComponent or AnimationPlayerComponent (the skinned-mesh node sits below a
    // model's container root). Returns `root` when none, so a non-skinned single-entity
    // model resolves to itself. Walks the children caches, so call after relinkHierarchy.
    auto animatableDescendant(Scene& scene, Entity root) -> Entity
    {
        Entity found{ entt::null };
        auto visit = [&scene, &found](auto&& self, entt::entity handle) -> void
        {
            if (found.handle != entt::null)
            {
                return;
            }
            if (scene.registry.all_of<SkinnedMeshComponent>(handle) ||
                scene.registry.all_of<AnimationPlayerComponent>(handle))
            {
                found = Entity{ handle };
                return;
            }
            if (scene.registry.all_of<RelationshipComponent>(handle))
            {
                for (entt::entity child : scene.registry.get<RelationshipComponent>(handle).children)
                {
                    self(self, child);
                }
            }
        };
        visit(visit, root.handle);
        if (found.handle != entt::null)
        {
            return found;
        }
        return root;
    }

    // The model instance's root: the nearest ancestor (including `entity`) carrying a
    // ModelInstanceComponent, or `entity` if none. Lets a viewport pick of an inner mesh or
    // bone resolve to the whole model.
    auto modelRootOf(Scene& scene, Entity entity) -> Entity
    {
        entt::entity cursor = entity.handle;
        while (cursor != entt::null && scene.registry.valid(cursor))
        {
            if (scene.registry.all_of<ModelInstanceComponent>(cursor))
            {
                return Entity{ cursor };
            }
            if (scene.registry.all_of<RelationshipComponent>(cursor))
            {
                cursor = scene.registry.get<RelationshipComponent>(cursor).parentHandle;
            }
            else
            {
                cursor = entt::null;
            }
        }
        return entity;
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
                    std::string_view reason = "is the entity itself";
                    if (it == uuidToHandle.end())
                    {
                        reason = "not found";
                    }
                    logWarn(std::format("relationship parent {} {}; treating as root", rel.parent.value, reason));
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

    // A transformable entity's effective local matrix: the animation pose override when
    // present (composed from its quaternion directly, no Euler round-trip), else the
    // authored TransformComponent. Keeps the rest pose pristine under non-destructive preview.
    auto localMatrix(Scene& scene, Entity entity) -> glm::mat4
    {
        if (const auto* pose = scene.registry.try_get<PoseOverrideComponent>(entity.handle))
        {
            return glm::translate(glm::mat4(1.0f), pose->translation) * glm::mat4_cast(pose->rotation) *
                   glm::scale(glm::mat4(1.0f), pose->scale);
        }
        return transformMatrix(getComponent<TransformComponent>(scene, entity));
    }

    // Exact world matrix composed by walking the parent chain. Used where the cached
    // per-frame matrix may lag a just-edited local transform (reparenting math).
    auto composeWorldMatrix(Scene& scene, Entity entity) -> glm::mat4
    {
        glm::mat4 local{ 1.0f };
        if (hasComponent<TransformComponent>(scene, entity))
        {
            local = localMatrix(scene, entity);
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
                world = parentWorld * localMatrix(scene, entity);
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
            glm::mat4 inverseBind{ 1.0f };
            if (i < skin.inverseBind.size())
            {
                inverseBind = skin.inverseBind[i];
            }
            out[i] = worldMatrix(scene, Entity{ bone }) * inverseBind;
        }
    }

    // A quaternion in the engine's stable Rz*Ry*Rx Euler convention. glm::eulerAngles is
    // numerically unstable at yaw +-90 degrees (its asin/atan2 split poisons pitch/roll), so
    // go through the ZYX matrix extraction. The one place quaternions become a TransformComponent Euler.
    auto quatToEulerZYX(const glm::quat& q) -> glm::vec3
    {
        glm::vec3 euler;
        glm::extractEulerAngleZYX(glm::mat4_cast(q), euler.z, euler.y, euler.x);
        return euler;
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
        TransformComponent& transform = getComponent<TransformComponent>(scene, entity);
        transform.translation = translation;
        transform.rotation = quatToEulerZYX(orientation);
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

        glm::mat4 childWorld{ 1.0f };
        if (keepWorld)
        {
            childWorld = composeWorldMatrix(scene, child);
        }

        if (newParent.handle == entt::null)
        {
            getComponent<RelationshipComponent>(scene, child).parent = Uuid{ 0 };
        }
        else
        {
            getComponent<RelationshipComponent>(scene, child).parent = getComponent<IdComponent>(scene, newParent).id;
        }

        if (keepWorld && hasComponent<TransformComponent>(scene, child))
        {
            glm::mat4 parentWorld{ 1.0f };
            if (newParent.handle != entt::null)
            {
                parentWorld = composeWorldMatrix(scene, newParent);
            }
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

    auto bvec3ToJson(const glm::bvec3& v) -> nlohmann::json
    {
        return nlohmann::json{ { "x", v.x }, { "y", v.y }, { "z", v.z } };
    }

    auto bvec3FromJson(const nlohmann::json& j) -> glm::bvec3
    {
        return glm::bvec3{ jsonBoolOr(j, "x", false), jsonBoolOr(j, "y", false), jsonBoolOr(j, "z", false) };
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
    auto animationPlayerComponentToJson(const AnimationPlayerComponent& c) -> nlohmann::json;
    auto animationPlayerComponentFromJson(AnimationPlayerComponent& c, const nlohmann::json& j) -> Result<void>;
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
    auto footIkComponentToJson(const FootIkComponent& c) -> nlohmann::json;
    auto footIkComponentFromJson(FootIkComponent& c, const nlohmann::json& j) -> Result<void>;
    auto bonePhysicsComponentToJson(const BonePhysicsComponent& c) -> nlohmann::json;
    auto bonePhysicsComponentFromJson(BonePhysicsComponent& c, const nlohmann::json& j) -> Result<void>;
    auto rigidbodyComponentToJson(const RigidbodyComponent& c) -> nlohmann::json;
    auto rigidbodyComponentFromJson(RigidbodyComponent& c, const nlohmann::json& j) -> Result<void>;
    auto colliderComponentToJson(const ColliderComponent& c) -> nlohmann::json;
    auto colliderComponentFromJson(ColliderComponent& c, const nlohmann::json& j) -> Result<void>;
    auto kinematicBonesComponentToJson(const KinematicBonesComponent& c) -> nlohmann::json;
    auto kinematicBonesComponentFromJson(KinematicBonesComponent& c, const nlohmann::json& j) -> Result<void>;
    auto characterControllerComponentToJson(const CharacterControllerComponent& c) -> nlohmann::json;
    auto characterControllerComponentFromJson(CharacterControllerComponent& c, const nlohmann::json& j) -> Result<void>;
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

    // The gameplay-input snapshot scripts read (held + derived key edges + mouse). Lives on
    // SceneEditContext (the editor forwards raw input over the script-input command); the Host derives
    // the edges/deltas each tick via deriveScriptInputEdges before tickScripts. Saffron.Script and
    // Saffron.SceneEdit both import Saffron.Scene, so this shared POD avoids a cross-module edge.
    struct ScriptInputState
    {
        std::unordered_set<std::string> held;          // keys down (raw, from script-input)
        std::unordered_set<std::string> mouseButtons;  // "left"/"right"/"middle" (raw)
        f32 mouseX = 0.0f;                             // viewport-relative pointer (raw)
        f32 mouseY = 0.0f;
        f32 scroll = 0.0f;
        std::unordered_set<std::string> pressed;        // key down this tick, up last (derived)
        std::unordered_set<std::string> released;       // key up this tick, down last (derived)
        std::unordered_set<std::string> mousePressed;   // button down this tick, up last (derived)
        std::unordered_set<std::string> mouseReleased;  // button up this tick, down last (derived)
        f32 mouseDX = 0.0f;                             // per-tick pointer delta (derived)
        f32 mouseDY = 0.0f;
        std::unordered_set<std::string> prevHeld;          // derivation memory
        std::unordered_set<std::string> prevMouseButtons;  // derivation memory
        f32 prevMouseX = 0.0f;
        f32 prevMouseY = 0.0f;
    };

    // Derive the key edges + mouse delta from this tick's raw held/mouse against the previous tick,
    // then roll the memory forward. Called once per tick by the Host before tickScripts.
    inline void deriveScriptInputEdges(ScriptInputState& input)
    {
        input.pressed.clear();
        input.released.clear();
        for (const std::string& key : input.held)
        {
            if (!input.prevHeld.contains(key))
            {
                input.pressed.insert(key);
            }
        }
        for (const std::string& key : input.prevHeld)
        {
            if (!input.held.contains(key))
            {
                input.released.insert(key);
            }
        }
        input.mousePressed.clear();
        input.mouseReleased.clear();
        for (const std::string& button : input.mouseButtons)
        {
            if (!input.prevMouseButtons.contains(button))
            {
                input.mousePressed.insert(button);
            }
        }
        for (const std::string& button : input.prevMouseButtons)
        {
            if (!input.mouseButtons.contains(button))
            {
                input.mouseReleased.insert(button);
            }
        }
        input.mouseDX = input.mouseX - input.prevMouseX;
        input.mouseDY = input.mouseY - input.prevMouseY;
        input.prevHeld = input.held;
        input.prevMouseButtons = input.mouseButtons;
        input.prevMouseX = input.mouseX;
        input.prevMouseY = input.mouseY;
    }

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
    // it back, and confirm the data survives.
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
