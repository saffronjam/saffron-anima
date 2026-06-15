module;

#include <glm/glm.hpp>

#include <cstdint>

export module Saffron.Physics:Types;

import Saffron.Core;

export namespace sa
{
    /// How a body participates in the simulation. Mirrors Jolt EMotionType 1:1.
    /// A ColliderComponent without a RigidbodyComponent is an implicit Static body;
    /// a present RigidbodyComponent's motion type wins.
    enum class MotionType : u8
    {
        Static,
        Kinematic,
        Dynamic
    };

    /// Object-layer slots a body lives in. v1 is a fixed set; a project-authored matrix is
    /// deferred. A ColliderComponent with no Rigidbody is implicitly Static; RigidbodyComponent.layer
    /// selects a moving slot (0 = Moving, 1 = Character, 2 = Debris); isSensor overrides to Sensor.
    enum class ObjectLayer : u8
    {
        Static,     // immovable world geometry (floors/walls): the implicit layer of a lone collider
        Moving,     // dynamic + kinematic bodies (the default for a Rigidbody)
        Character,  // the character controller's body
        Debris,     // dynamic bodies that collide with world/character but not each other (perf)
        Sensor,     // trigger volumes: overlap-only, never solved
        Count
    };
    inline constexpr u8 ObjectLayerCount = static_cast<u8>(ObjectLayer::Count);

    /// Whether two object layers may collide. Symmetric. The whole v1 collision policy is this table.
    auto layersCollide(ObjectLayer a, ObjectLayer b) -> bool;

    /// The deterministic fixed substep the world advances by, matching SceneEdit's PlayFixedStep
    /// (1/60). The step accumulator advances the sim in fixed increments so the simulation is
    /// decoupled from frame rate and stays bit-exact under the cross-platform-deterministic build.
    inline constexpr f32 PhysicsFixedStep = 1.0f / 60.0f;

    /// A summary of the live world, surfaced over the control plane (Jolt-free POD).
    struct PhysicsWorldStats
    {
        bool active = false;
        i32 bodyCount = 0;
        i32 dynamicCount = 0;
    };

    /// One live body's read-only snapshot for the editor's physics panel (Jolt-free POD): owner
    /// entity uuid, motion type, sleep/active state, and world position.
    struct PhysicsBodyInfo
    {
        u64 entity = 0;
        MotionType motion = MotionType::Static;
        bool active = false;
        glm::vec3 position{ 0.0f };
    };

    /// One ray/shape query hit against the live physics world (world space, Jolt-free POD).
    struct PhysicsRayHit
    {
        bool hit = false;
        u64 entity = 0;            // IdComponent uuid of the hit body's owner (0 = none)
        glm::vec3 point{ 0.0f };   // world-space contact point
        glm::vec3 normal{ 0.0f };  // world-space surface normal at the hit
        f32 distance = 0.0f;       // along the ray from origin, in dir units (fraction * maxDist)
    };
}
