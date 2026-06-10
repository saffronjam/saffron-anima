module;

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <string>

module Saffron.SceneEdit;

import Saffron.Core;
import Saffron.Scene;
import Saffron.Json;

namespace se
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
    }
}
