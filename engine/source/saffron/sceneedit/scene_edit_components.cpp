module;

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <string>

module Saffron.SceneEdit;

import Saffron.Core;
import Saffron.Scene;
import Saffron.Json;

namespace sa
{
    void registerBuiltinComponents(ComponentRegistry& reg)
    {
        registerComponent<NameComponent>(
            reg, "Name", [](Scene&, Entity) {}, nameComponentToJson, nameComponentFromJson, false);

        registerComponent<TransformComponent>(
            reg, "Transform", [](Scene&, Entity) {}, transformComponentToJson, transformComponentFromJson, false);

        registerComponent<MeshComponent>(
            reg, "Mesh", [](Scene&, Entity) {}, meshComponentToJson, meshComponentFromJson, true);

        registerComponent<CameraComponent>(
            reg, "Camera", [](Scene&, Entity) {}, cameraComponentToJson, cameraComponentFromJson, true);

        registerComponent<MaterialComponent>(
            reg, "Material", [](Scene&, Entity) {}, materialComponentToJson, materialComponentFromJson, true);

        registerComponent<MaterialSetComponent>(
            reg, "MaterialSet", [](Scene&, Entity) {}, materialSetComponentToJson, materialSetComponentFromJson, true);

        registerComponent<MaterialAssetComponent>(
            reg, "MaterialAsset", [](Scene&, Entity) {}, [](const MaterialAssetComponent& c) -> nlohmann::json
            { return nlohmann::json{ { "material", std::to_string(c.material.value) } }; },
            [](MaterialAssetComponent& c, const nlohmann::json& j) -> Result<void>
            {
                if (auto it = j.find("material"); it != j.end())
                {
                    if (it->is_string())
                    {
                        c.material = Uuid{ std::strtoull(it->get<std::string>().c_str(), nullptr, 10) };
                    }
                    else if (it->is_number_unsigned())
                    {
                        c.material = Uuid{ it->get<u64>() };
                    }
                }
                return {};
            },
            true);

        registerComponent<ModelInstanceComponent>(
            reg, "ModelInstance", [](Scene&, Entity) {}, [](const ModelInstanceComponent& c) -> nlohmann::json
            { return nlohmann::json{ { "modelId", std::to_string(c.modelId.value) } }; },
            [](ModelInstanceComponent& c, const nlohmann::json& j) -> Result<void>
            {
                if (auto it = j.find("modelId"); it != j.end())
                {
                    if (it->is_string())
                    {
                        c.modelId = Uuid{ std::strtoull(it->get<std::string>().c_str(), nullptr, 10) };
                    }
                    else if (it->is_number_unsigned())
                    {
                        c.modelId = Uuid{ it->get<u64>() };
                    }
                }
                return {};
            },
            true);

        registerComponent<ScriptComponent>(
            reg, "Script", [](Scene&, Entity) {}, scriptComponentToJson, scriptComponentFromJson, true);

        registerComponent<AnimationPlayerComponent>(
            reg, "AnimationPlayer", [](Scene&, Entity) {}, animationPlayerComponentToJson,
            animationPlayerComponentFromJson, true);

        registerComponent<DirectionalLightComponent>(
            reg, "DirectionalLight", [](Scene&, Entity) {}, directionalLightComponentToJson,
            directionalLightComponentFromJson, true);

        registerComponent<PointLightComponent>(
            reg, "PointLight", [](Scene&, Entity) {}, pointLightComponentToJson, pointLightComponentFromJson, true);

        registerComponent<SpotLightComponent>(
            reg, "SpotLight", [](Scene&, Entity) {}, spotLightComponentToJson, spotLightComponentFromJson, true);

        registerComponent<ReflectionProbeComponent>(
            reg, "ReflectionProbe", [](Scene&, Entity) {}, reflectionProbeComponentToJson,
            reflectionProbeComponentFromJson, true);

        // Non-removable: parenting is edited through set-parent / the tree, never as a
        // raw uuid field. Serde carries only the durable parent uuid; the runtime caches
        // are rebuilt by relinkHierarchy.
        registerComponent<RelationshipComponent>(
            reg, "Relationship", [](Scene&, Entity) {}, relationshipComponentToJson, relationshipComponentFromJson,
            false);

        // Serde carries the mesh/rootBone/bones uuids + inverse bind matrices; the
        // boneHandles cache is rebuilt by relinkHierarchy, never serialized.
        registerComponent<SkinnedMeshComponent>(
            reg, "SkinnedMesh", [](Scene&, Entity) {}, skinnedMeshComponentToJson, skinnedMeshComponentFromJson, true);

        registerComponent<BoneComponent>(
            reg, "Bone", [](Scene&, Entity) {}, boneComponentToJson, boneComponentFromJson, true);

        // Foot-IK chains on the rig; the animation evaluator reads them as a blend-layer
        // producer. The toggle is also reachable over the control plane (set-foot-ik).
        registerComponent<FootIkComponent>(
            reg, "FootIk", [](Scene&, Entity) {}, footIkComponentToJson, footIkComponentFromJson, true);

        // Reserved per-bone Jolt metadata (no runtime use this phase); serialized so authoring
        // survives a round trip ahead of the physics phase.
        registerComponent<BonePhysicsComponent>(
            reg, "BonePhysics", [](Scene&, Entity) {}, bonePhysicsComponentToJson, bonePhysicsComponentFromJson, true);

        // The split physics components. onAdd is a no-op here — the AABB auto-fit needs the asset +
        // renderer handles the registry's (Scene&, Entity) hook can't reach, so `add-component` runs
        // fitColliderToMesh after the default-construct (a Collider with no Rigidbody is static).
        registerComponent<RigidbodyComponent>(
            reg, "Rigidbody", [](Scene&, Entity) {}, rigidbodyComponentToJson, rigidbodyComponentFromJson, true);
        registerComponent<ColliderComponent>(
            reg, "Collider", [](Scene&, Entity) {}, colliderComponentToJson, colliderComponentFromJson, true);

        // Opts a rig into kinematic-bone physics. onAdd is a no-op here — the per-bone capsule auto-fit
        // needs the skeleton geometry, so set-kinematic-bones runs it from the control plane.
        registerComponent<KinematicBonesComponent>(
            reg, "KinematicBones", [](Scene&, Entity) {}, kinematicBonesComponentToJson,
            kinematicBonesComponentFromJson, true);

        registerComponent<CharacterControllerComponent>(
            reg, "CharacterController", [](Scene&, Entity) {}, characterControllerComponentToJson,
            characterControllerComponentFromJson, true);
    }
}
