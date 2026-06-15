// GENERATED - do not edit.
// Produced by tools/gen-control-dto/gen.ts (emitScriptComponentDefs) from the component wire-shape
// catalog. Appended to library/se.lua after SeLuaDefs so :get_component(name) returns a typed table.
#pragma once

#include <string_view>

inline constexpr std::string_view SeComponentDefs = R"LUA(
-- Typed component snapshots. get_component(name) returns the component as a read-only table in
-- its serialized wire shape (vectors as {x,y,z} tables, ids as decimal strings); nil when absent.
---@class se.AnimationPlayer
---@field clip string
---@field time number
---@field speed number
---@field wrap "once"|"loop"|"pingpong"
---@field playing boolean
---@field transitionMode "crossfade"|"inertialize"
---@field loopBlend number

---@class se.BVec3
---@field x boolean
---@field y boolean
---@field z boolean

---@class se.Bone

---@class se.BonePhysics
---@field bones se.BonePhysicsDto[]

---@class se.BonePhysicsDto
---@field shapeHalfExtents { x: number, y: number, z: number }
---@field mass number
---@field joint string
---@field swingTwistLimits { x: number, y: number, z: number }
---@field driveStiffness number
---@field driveDamping number
---@field driveMaxForce number

---@class se.Camera
---@field fov number
---@field near number
---@field far number
---@field primary boolean
---@field showModel boolean
---@field showFrustum boolean
---@field frustumMaxDistance number

---@class se.CharacterController
---@field maxSpeed number
---@field maxSlopeAngle number
---@field maxStepHeight number
---@field gravityFactor number

---@class se.Collider
---@field shape "box"|"sphere"|"capsule"|"convexhull"|"mesh"
---@field halfExtents { x: number, y: number, z: number }
---@field sourceMesh string
---@field offset { x: number, y: number, z: number }
---@field material se.PhysicsMaterial
---@field isSensor boolean

---@class se.DirectionalLight
---@field direction { x: number, y: number, z: number }
---@field color { x: number, y: number, z: number }
---@field intensity number
---@field ambient number

---@class se.FootChainDto
---@field upper number
---@field mid number
---@field end number
---@field poleVector { x: number, y: number, z: number }

---@class se.FootIk
---@field enabled boolean
---@field groundHeight number
---@field chains se.FootChainDto[]

---@class se.KinematicBones
---@field enabled boolean
---@field driven number[]

---@class se.Material
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

---@class se.MaterialAsset
---@field material string

---@class se.MaterialSet
---@field slots se.Material[]

---@class se.Mesh
---@field mesh string

---@class se.ModelInstance
---@field modelId string

---@class se.Name
---@field name string

---@class se.PhysicsMaterial
---@field friction number
---@field restitution number

---@class se.PointLight
---@field color { x: number, y: number, z: number }
---@field intensity number
---@field range number

---@class se.ReflectionProbe
---@field influenceRadius number
---@field intensity number
---@field boxProjection boolean
---@field boxExtent { x: number, y: number, z: number }

---@class se.Relationship
---@field parent string

---@class se.Rigidbody
---@field motion "static"|"kinematic"|"dynamic"
---@field mass number
---@field linearDamping number
---@field angularDamping number
---@field gravityFactor number
---@field lockPosition se.BVec3
---@field lockRotation se.BVec3
---@field collisionLayer number

---@class se.Script
---@field scripts se.ScriptSlot[]

---@class se.ScriptSlot
---@field scriptPath string
---@field overrides table<string, any>

---@class se.SkinnedMesh
---@field mesh string
---@field rootBone string
---@field bones string[]
---@field inverseBind number[][]

---@class se.SpotLight
---@field direction { x: number, y: number, z: number }
---@field color { x: number, y: number, z: number }
---@field intensity number
---@field range number
---@field innerAngle number
---@field outerAngle number

---@class se.Transform
---@field translation { x: number, y: number, z: number }
---@field scale { x: number, y: number, z: number }
---@field rotation { x: number, y: number, z: number }

---@overload fun(self: se.Entity, name: "AnimationPlayer"): se.AnimationPlayer?
---@overload fun(self: se.Entity, name: "Bone"): se.Bone?
---@overload fun(self: se.Entity, name: "BonePhysics"): se.BonePhysics?
---@overload fun(self: se.Entity, name: "Camera"): se.Camera?
---@overload fun(self: se.Entity, name: "CharacterController"): se.CharacterController?
---@overload fun(self: se.Entity, name: "Collider"): se.Collider?
---@overload fun(self: se.Entity, name: "DirectionalLight"): se.DirectionalLight?
---@overload fun(self: se.Entity, name: "FootIk"): se.FootIk?
---@overload fun(self: se.Entity, name: "KinematicBones"): se.KinematicBones?
---@overload fun(self: se.Entity, name: "Material"): se.Material?
---@overload fun(self: se.Entity, name: "MaterialAsset"): se.MaterialAsset?
---@overload fun(self: se.Entity, name: "MaterialSet"): se.MaterialSet?
---@overload fun(self: se.Entity, name: "Mesh"): se.Mesh?
---@overload fun(self: se.Entity, name: "ModelInstance"): se.ModelInstance?
---@overload fun(self: se.Entity, name: "Name"): se.Name?
---@overload fun(self: se.Entity, name: "PointLight"): se.PointLight?
---@overload fun(self: se.Entity, name: "ReflectionProbe"): se.ReflectionProbe?
---@overload fun(self: se.Entity, name: "Relationship"): se.Relationship?
---@overload fun(self: se.Entity, name: "Rigidbody"): se.Rigidbody?
---@overload fun(self: se.Entity, name: "Script"): se.Script?
---@overload fun(self: se.Entity, name: "SkinnedMesh"): se.SkinnedMesh?
---@overload fun(self: se.Entity, name: "SpotLight"): se.SpotLight?
---@overload fun(self: se.Entity, name: "Transform"): se.Transform?
function Entity:get_component(name) end  ---@param name se.ComponentName @return table?
)LUA";
