// GENERATED - do not edit.
// Produced by tools/gen-control-dto/gen.ts (emitScriptComponentDefs) from the component wire-shape
// catalog. Appended to library/sa.lua after SaLuaDefs so :get_component(name) returns a typed table.
#pragma once

#include <string_view>

inline constexpr std::string_view SaComponentDefs = R"LUA(
-- Typed component snapshots. get_component(name) returns the component as a read-only table in
-- its serialized wire shape (vectors as {x,y,z} tables, ids as decimal strings); nil when absent.
---@class sa.AnimationPlayer
---@field clip string
---@field time number
---@field speed number
---@field wrap "once"|"loop"|"pingpong"
---@field playing boolean
---@field transitionMode "crossfade"|"inertialize"
---@field loopBlend number

---@class sa.BVec3
---@field x boolean
---@field y boolean
---@field z boolean

---@class sa.Bone

---@class sa.BonePhysics
---@field bones sa.BonePhysicsDto[]

---@class sa.BonePhysicsDto
---@field shapeHalfExtents { x: number, y: number, z: number }
---@field mass number
---@field joint string
---@field swingTwistLimits { x: number, y: number, z: number }
---@field driveStiffness number
---@field driveDamping number
---@field driveMaxForce number

---@class sa.Camera
---@field fov number
---@field near number
---@field far number
---@field primary boolean
---@field showModel boolean
---@field showFrustum boolean
---@field frustumMaxDistance number

---@class sa.CharacterController
---@field maxSpeed number
---@field maxSlopeAngle number
---@field maxStepHeight number
---@field gravityFactor number

---@class sa.Collider
---@field shape "box"|"sphere"|"capsule"|"convexhull"|"mesh"
---@field halfExtents { x: number, y: number, z: number }
---@field sourceMesh string
---@field offset { x: number, y: number, z: number }
---@field material sa.PhysicsMaterial
---@field isSensor boolean

---@class sa.DirectionalLight
---@field direction { x: number, y: number, z: number }
---@field color { x: number, y: number, z: number }
---@field intensity number
---@field ambient number

---@class sa.FootChainDto
---@field upper number
---@field mid number
---@field end number
---@field poleVector { x: number, y: number, z: number }

---@class sa.FootIk
---@field enabled boolean
---@field groundHeight number
---@field chains sa.FootChainDto[]

---@class sa.KinematicBones
---@field enabled boolean
---@field driven number[]

---@class sa.Material
---@field baseColor { x: number, y: number, z: number, w: number }
---@field albedoTexture string
---@field metallicRoughnessTexture string
---@field metallic number
---@field roughness number
---@field emissive { x: number, y: number, z: number }
---@field emissiveStrength number
---@field unlit boolean
---@field normalTexture string
---@field occlusionTexture string
---@field emissiveTexture string
---@field heightTexture string
---@field normalStrength number
---@field heightScale number
---@field alphaClip boolean
---@field alphaCutoff number

---@class sa.MaterialAsset
---@field material string

---@class sa.MaterialSet
---@field slots sa.Material[]

---@class sa.Mesh
---@field mesh string

---@class sa.ModelInstance
---@field modelId string

---@class sa.Name
---@field name string

---@class sa.PhysicsMaterial
---@field friction number
---@field restitution number

---@class sa.PointLight
---@field color { x: number, y: number, z: number }
---@field intensity number
---@field range number

---@class sa.ReflectionProbe
---@field influenceRadius number
---@field intensity number
---@field boxProjection boolean
---@field boxExtent { x: number, y: number, z: number }

---@class sa.Relationship
---@field parent string

---@class sa.Rigidbody
---@field motion "static"|"kinematic"|"dynamic"
---@field mass number
---@field linearDamping number
---@field angularDamping number
---@field gravityFactor number
---@field lockPosition sa.BVec3
---@field lockRotation sa.BVec3
---@field collisionLayer number

---@class sa.Script
---@field scripts sa.ScriptSlot[]

---@class sa.ScriptSlot
---@field scriptPath string
---@field overrides table<string, any>

---@class sa.SkinnedMesh
---@field mesh string
---@field rootBone string
---@field bones string[]
---@field inverseBind number[][]

---@class sa.SpotLight
---@field direction { x: number, y: number, z: number }
---@field color { x: number, y: number, z: number }
---@field intensity number
---@field range number
---@field innerAngle number
---@field outerAngle number

---@class sa.Transform
---@field translation { x: number, y: number, z: number }
---@field scale { x: number, y: number, z: number }
---@field rotation { x: number, y: number, z: number }

---@overload fun(self: sa.Entity, name: "AnimationPlayer"): sa.AnimationPlayer?
---@overload fun(self: sa.Entity, name: "Bone"): sa.Bone?
---@overload fun(self: sa.Entity, name: "BonePhysics"): sa.BonePhysics?
---@overload fun(self: sa.Entity, name: "Camera"): sa.Camera?
---@overload fun(self: sa.Entity, name: "CharacterController"): sa.CharacterController?
---@overload fun(self: sa.Entity, name: "Collider"): sa.Collider?
---@overload fun(self: sa.Entity, name: "DirectionalLight"): sa.DirectionalLight?
---@overload fun(self: sa.Entity, name: "FootIk"): sa.FootIk?
---@overload fun(self: sa.Entity, name: "KinematicBones"): sa.KinematicBones?
---@overload fun(self: sa.Entity, name: "Material"): sa.Material?
---@overload fun(self: sa.Entity, name: "MaterialAsset"): sa.MaterialAsset?
---@overload fun(self: sa.Entity, name: "MaterialSet"): sa.MaterialSet?
---@overload fun(self: sa.Entity, name: "Mesh"): sa.Mesh?
---@overload fun(self: sa.Entity, name: "ModelInstance"): sa.ModelInstance?
---@overload fun(self: sa.Entity, name: "Name"): sa.Name?
---@overload fun(self: sa.Entity, name: "PointLight"): sa.PointLight?
---@overload fun(self: sa.Entity, name: "ReflectionProbe"): sa.ReflectionProbe?
---@overload fun(self: sa.Entity, name: "Relationship"): sa.Relationship?
---@overload fun(self: sa.Entity, name: "Rigidbody"): sa.Rigidbody?
---@overload fun(self: sa.Entity, name: "Script"): sa.Script?
---@overload fun(self: sa.Entity, name: "SkinnedMesh"): sa.SkinnedMesh?
---@overload fun(self: sa.Entity, name: "SpotLight"): sa.SpotLight?
---@overload fun(self: sa.Entity, name: "Transform"): sa.Transform?
function Entity:get_component(name) end  ---@param name sa.ComponentName @return table?
)LUA";
