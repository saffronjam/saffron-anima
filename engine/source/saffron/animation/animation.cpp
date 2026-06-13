module;

// Same global-module-fragment shape as the interface unit: glm + entt via classic
// includes, no `import std`.
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <expected>
#include <format>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

module Saffron.Animation;

import Saffron.Core;
import Saffron.Geometry;

namespace se
{
    namespace
    {
        auto asQuat(glm::vec4 v) -> glm::quat
        {
            return { v.w, v.x, v.y, v.z };
        }

        auto fromQuat(glm::quat q) -> glm::vec4
        {
            return { q.x, q.y, q.z, q.w };
        }

        // A bone's authored rest local TRS, read from its TransformComponent (Euler ->
        // quat, matching transformMatrix). Identity if the handle is stale.
        auto restPoseOf(Scene& scene, const SkinnedMeshComponent& skin, std::size_t i) -> JointPose
        {
            JointPose rest;
            if (i >= skin.boneHandles.size())
            {
                return rest;
            }
            const Entity bone{ skin.boneHandles[i] };
            if (!valid(scene, bone) || !hasComponent<TransformComponent>(scene, bone))
            {
                return rest;
            }
            const TransformComponent& transform = getComponent<TransformComponent>(scene, bone);
            rest.translation = transform.translation;
            rest.rotation = glm::quat(transform.rotation);
            rest.scale = transform.scale;
            return rest;
        }

        // Drop every pose override on a rig's bones so they revert to the rest pose.
        void clearOverrides(Scene& scene, const SkinnedMeshComponent& skin)
        {
            for (const entt::entity handle : skin.boneHandles)
            {
                if (handle != entt::null && scene.registry.valid(handle))
                {
                    scene.registry.remove<PoseOverrideComponent>(handle);
                }
            }
        }

        // Resolve (and cache) a clip Uuid to its loaded AnimClip. A broken asset is
        // negative-cached as an empty clip so it is not re-read every frame.
        auto loadClip(AnimationRuntime& runtime, Uuid clip) -> const AnimClip*
        {
            if (clip.value == 0)
            {
                return nullptr;
            }
            if (auto it = runtime.clipCache.find(clip.value); it != runtime.clipCache.end())
            {
                return &it->second;
            }
            if (!runtime.clipLoader)
            {
                return nullptr;
            }
            auto loaded = runtime.clipLoader(clip);
            if (!loaded)
            {
                logWarn(std::format("animation: clip {} failed to load: {}", clip.value, loaded.error()));
                return &runtime.clipCache.emplace(clip.value, AnimClip{}).first->second;
            }
            return &runtime.clipCache.emplace(clip.value, std::move(*loaded)).first->second;
        }

        // Advance the playhead by dt*speed under the wrap mode. Once clamps + stops at an
        // end; Loop wraps; PingPong bounces and flips direction.
        void advanceTime(AnimationPlayerComponent& player, f32 duration, f32 dt)
        {
            const f32 delta = dt * player.speed;
            if (player.wrap == AnimationPlayerComponent::Wrap::PingPong)
            {
                if (player.pingForward)
                {
                    player.time = player.time + delta;
                }
                else
                {
                    player.time = player.time - delta;
                }
                if (player.time >= duration)
                {
                    player.time = 2.0f * duration - player.time;
                    player.pingForward = false;
                }
                if (player.time <= 0.0f)
                {
                    player.time = -player.time;
                    player.pingForward = true;
                }
                player.time = glm::clamp(player.time, 0.0f, duration);
                return;
            }
            player.time = player.time + delta;
            if (player.wrap == AnimationPlayerComponent::Wrap::Loop)
            {
                player.time = std::fmod(player.time, duration);
                if (player.time < 0.0f)
                {
                    player.time = player.time + duration;
                }
                return;
            }
            if (player.time >= duration)
            {
                player.time = duration;
                player.playing = false;
            }
            else if (player.time < 0.0f)
            {
                player.time = 0.0f;
                player.playing = false;
            }
        }

        // Shortest-arc rotation taking unit `from` onto unit `to`. Falls back to a stable
        // perpendicular axis for the antiparallel case and identity for degenerate inputs.
        auto rotationBetween(glm::vec3 from, glm::vec3 to) -> glm::quat
        {
            const f32 lf = glm::length(from);
            const f32 lt = glm::length(to);
            if (lf < 1e-8f || lt < 1e-8f)
            {
                return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
            }
            from = from / lf;
            to = to / lt;
            const f32 d = glm::clamp(glm::dot(from, to), -1.0f, 1.0f);
            if (d > 1.0f - 1e-6f)
            {
                return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
            }
            if (d < -1.0f + 1e-6f)
            {
                // Antiparallel: any perpendicular axis is a valid 180 deg flip.
                glm::vec3 axis = glm::cross(glm::vec3(1.0f, 0.0f, 0.0f), from);
                if (glm::length(axis) < 1e-6f)
                {
                    axis = glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), from);
                }
                return glm::normalize(glm::angleAxis(glm::pi<f32>(), glm::normalize(axis)));
            }
            const glm::vec3 axis = glm::normalize(glm::cross(from, to));
            return glm::normalize(glm::angleAxis(std::acos(d), axis));
        }

        // Per-joint blend of two poses (lerp T/S, slerp R). The cross-fade primitive.
        auto blendJoint(const JointPose& base, const JointPose& over, f32 weight) -> JointPose
        {
            JointPose out;
            out.translation = glm::mix(base.translation, over.translation, weight);
            out.rotation = glm::normalize(glm::slerp(base.rotation, over.rotation, weight));
            out.scale = glm::mix(base.scale, over.scale, weight);
            return out;
        }

        // Cubic ease (C¹) for the cross-fade alpha.
        auto smoothstep01(f32 x) -> f32
        {
            x = glm::clamp(x, 0.0f, 1.0f);
            return x * x * (3.0f - 2.0f * x);
        }

        // Quintic decay 1 → 0 with zero value, slope, and acceleration at x = 1 (C²,
        // zero-jerk): the inertialization offset weight as the transition runs out.
        auto quinticDecay(f32 x) -> f32
        {
            x = glm::clamp(x, 0.0f, 1.0f);
            const f32 smoother = x * x * x * (x * (x * 6.0f - 15.0f) + 10.0f);
            return 1.0f - smoother;
        }

        // A bone's parent world rotation, identity at the root or when the parent is stale.
        auto parentWorldRotation(Scene& scene, entt::entity handle) -> glm::quat
        {
            if (handle == entt::null || !scene.registry.valid(handle))
            {
                return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
            }
            if (const auto* rel = scene.registry.try_get<RelationshipComponent>(handle))
            {
                const entt::entity parent = rel->parentHandle;
                if (parent != entt::null && scene.registry.valid(parent))
                {
                    return worldRotation(scene, Entity{ parent });
                }
            }
            return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        }

        // Foot-IK blend-layer producer: for each enabled chain, solve the two-bone IK against
        // the ground plane and write the result into the override_/weight layer. The chain's
        // world transforms come from the cached WorldTransformComponent (one frame stale, which
        // the e2e settles over). Never touches a bone's TransformComponent — the blend layer is
        // the only path. Writes the per-chain joints' local poses into `finalLocal`.
        void applyFootIk(Scene& scene, const SkinnedMeshComponent& skin, const FootIkComponent& ik,
                         std::vector<JointPose>& finalLocal)
        {
            const auto jointCount = static_cast<i32>(finalLocal.size());
            auto handleOf = [&](i32 idx) -> entt::entity
            {
                if (idx < 0 || idx >= static_cast<i32>(skin.boneHandles.size()))
                {
                    return entt::null;
                }
                return skin.boneHandles[static_cast<std::size_t>(idx)];
            };

            for (const FootChain& chain : ik.chains)
            {
                const entt::entity upperH = handleOf(chain.upper);
                const entt::entity midH = handleOf(chain.mid);
                const entt::entity endH = handleOf(chain.end);
                if (upperH == entt::null || midH == entt::null || endH == entt::null)
                {
                    continue;
                }
                if (chain.upper >= jointCount || chain.mid >= jointCount || chain.end >= jointCount)
                {
                    continue;
                }
                if (!scene.registry.valid(upperH) || !scene.registry.valid(midH) || !scene.registry.valid(endH))
                {
                    continue;
                }

                // Resolve the chain from THIS frame's animated pose (finalLocal) by forward
                // kinematics, NOT the cached WorldTransformComponent — that cache is last frame's
                // post-IK result, so reading it feeds the solver its own output and a near-straight
                // chain oscillates/flips. The chain root's parent sits above the chain, so its
                // cached world transform is stable. v1 assumes a directly-parented chain
                // (upper→mid→end, unit bone scale), which the foot-chain config describes.
                const std::size_t ui = static_cast<std::size_t>(chain.upper);
                const std::size_t mi = static_cast<std::size_t>(chain.mid);
                const std::size_t ei = static_cast<std::size_t>(chain.end);
                glm::vec3 parentPos{ 0.0f };
                glm::quat parentRot{ 1.0f, 0.0f, 0.0f, 0.0f };
                if (const auto* rel = scene.registry.try_get<RelationshipComponent>(upperH);
                    rel != nullptr && rel->parentHandle != entt::null && scene.registry.valid(rel->parentHandle))
                {
                    parentPos = worldTranslation(scene, Entity{ rel->parentHandle });
                    parentRot = worldRotation(scene, Entity{ rel->parentHandle });
                }
                const glm::quat wUpperRot = glm::normalize(parentRot * finalLocal[ui].rotation);
                const glm::vec3 rootPos = parentPos + parentRot * finalLocal[ui].translation;
                const glm::quat wMidRot = glm::normalize(wUpperRot * finalLocal[mi].rotation);
                const glm::vec3 midPos = rootPos + wUpperRot * finalLocal[mi].translation;
                const glm::vec3 endPos = midPos + wMidRot * finalLocal[ei].translation;
                const f32 upperLen = glm::length(midPos - rootPos);
                const f32 lowerLen = glm::length(endPos - midPos);
                if (upperLen < 1e-5f || lowerLen < 1e-5f)
                {
                    continue;
                }

                // v1 ground = a horizontal plane at groundHeight: plant the foot by lifting its
                // world Y up to the plane (never pull it below — a foot already above stays).
                glm::vec3 target = endPos;
                target.y = glm::max(target.y, ik.groundHeight);

                const TwoBoneIkResult solved =
                    solveTwoBoneIk(rootPos, midPos, endPos, target, chain.poleVector, upperLen, lowerLen);

                // The solved quats are world deltas: the upper swings the whole chain, the mid
                // additionally bends (it inherits the upper's swing as the upper's child). Strip
                // the (new) parent world rotation to land each in local space.
                const glm::quat newUpperWorld = glm::normalize(solved.upper * wUpperRot);
                const glm::quat newMidWorld = glm::normalize(solved.upper * solved.lower * wMidRot);
                finalLocal[ui].rotation = glm::normalize(glm::inverse(parentRot) * newUpperWorld);
                finalLocal[mi].rotation = glm::normalize(glm::inverse(newUpperWorld) * newMidWorld);
            }
        }

        // sampleClip, but each track is bound to its joint by index when sound and re-resolved
        // by the durable node name when the index is stale (out of range or names disagree).
        void sampleClipResolved(const AnimClip& clip, f32 t, const std::vector<std::string>& boneNames,
                                const std::unordered_map<std::string, i32>& nameToIndex, PoseBuffer& out)
        {
            const auto jointCount = static_cast<i32>(out.local.size());
            for (const AnimTrack& track : clip.tracks)
            {
                i32 joint = track.joint;
                const bool stale = joint < 0 || joint >= jointCount ||
                                   (!track.jointName.empty() && static_cast<std::size_t>(joint) < boneNames.size() &&
                                    boneNames[static_cast<std::size_t>(joint)] != track.jointName);
                if (stale)
                {
                    auto it = nameToIndex.find(track.jointName);
                    if (it != nameToIndex.end())
                    {
                        joint = it->second;
                    }
                    else
                    {
                        joint = -1;
                    }
                }
                if (joint < 0 || joint >= jointCount)
                {
                    continue;
                }
                const glm::vec4 v = sampleTrack(track, t);
                const auto j = static_cast<std::size_t>(joint);
                switch (track.path)
                {
                case AnimTrack::Path::Translation:
                    out.local[j].translation = glm::vec3(v);
                    break;
                case AnimTrack::Path::Rotation:
                    out.local[j].rotation = asQuat(v);
                    break;
                case AnimTrack::Path::Scale:
                    out.local[j].scale = glm::vec3(v);
                    break;
                }
            }
        }
    }

    auto sampleTrack(const AnimTrack& track, f32 t) -> glm::vec4
    {
        const bool rotation = track.path == AnimTrack::Path::Rotation;
        i32 cc = 3;
        if (rotation)
        {
            cc = 4;
        }
        const auto stride = static_cast<std::size_t>(cc);
        const std::vector<f32>& times = track.times;
        const std::size_t n = times.size();

        if (n == 0 || track.values.empty())
        {
            if (rotation)
            {
                return { 0.0f, 0.0f, 0.0f, 1.0f };
            }
            if (track.path == AnimTrack::Path::Scale)
            {
                return { 1.0f, 1.0f, 1.0f, 0.0f };
            }
            return glm::vec4(0.0f);
        }

        // CubicSpline stores [in-tangent, value, out-tangent] per key (3x stride); the
        // sampled value sits one stride in. STEP/LINEAR store the value flat.
        auto valueOffset = [&](std::size_t key) -> std::size_t
        {
            if (track.interp == AnimTrack::Interp::CubicSpline)
            {
                return key * 3 * stride + stride;
            }
            return key * stride;
        };
        auto readVec4 = [&](std::size_t offset) -> glm::vec4
        {
            glm::vec4 r(0.0f);
            for (i32 c = 0; c < cc; c = c + 1)
            {
                r[c] = track.values[offset + static_cast<std::size_t>(c)];
            }
            return r;
        };
        auto finish = [&](glm::vec4 v) -> glm::vec4
        {
            if (rotation)
            {
                return fromQuat(glm::normalize(asQuat(v)));
            }
            return v;
        };

        if (t <= times.front())
        {
            return finish(readVec4(valueOffset(0)));
        }
        if (t >= times.back())
        {
            return finish(readVec4(valueOffset(n - 1)));
        }

        const auto upper = std::ranges::upper_bound(times, t);
        const std::size_t i1 = static_cast<std::size_t>(upper - times.begin());
        const std::size_t i0 = i1 - 1;
        const f32 dt = times[i1] - times[i0];
        f32 local = 0.0f;
        if (dt > 0.0f)
        {
            local = (t - times[i0]) / dt;
        }

        if (track.interp == AnimTrack::Interp::Step)
        {
            return finish(readVec4(valueOffset(i0)));
        }

        if (track.interp == AnimTrack::Interp::Linear)
        {
            if (rotation)
            {
                const glm::quat a = glm::normalize(asQuat(readVec4(valueOffset(i0))));
                const glm::quat b = glm::normalize(asQuat(readVec4(valueOffset(i1))));
                return fromQuat(glm::normalize(glm::slerp(a, b, local)));
            }
            return glm::mix(readVec4(valueOffset(i0)), readVec4(valueOffset(i1)), local);
        }

        const f32 t2 = local * local;
        const f32 t3 = t2 * local;
        const f32 h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
        const f32 h10 = t3 - 2.0f * t2 + local;
        const f32 h01 = -2.0f * t3 + 3.0f * t2;
        const f32 h11 = t3 - t2;
        const glm::vec4 p0 = readVec4(i0 * 3 * stride + stride);
        const glm::vec4 p1 = readVec4(i1 * 3 * stride + stride);
        const glm::vec4 m0 = readVec4(i0 * 3 * stride + 2 * stride) * dt;
        const glm::vec4 m1 = readVec4(i1 * 3 * stride) * dt;
        return finish(h00 * p0 + h10 * m0 + h01 * p1 + h11 * m1);
    }

    void sampleClip(const AnimClip& clip, f32 t, PoseBuffer& out)
    {
        for (const AnimTrack& track : clip.tracks)
        {
            if (track.joint < 0)
            {
                continue;
            }
            const auto j = static_cast<std::size_t>(track.joint);
            if (j >= out.local.size())
            {
                continue;
            }
            const glm::vec4 v = sampleTrack(track, t);
            switch (track.path)
            {
            case AnimTrack::Path::Translation:
                out.local[j].translation = glm::vec3(v);
                break;
            case AnimTrack::Path::Rotation:
                out.local[j].rotation = asQuat(v);
                break;
            case AnimTrack::Path::Scale:
                out.local[j].scale = glm::vec3(v);
                break;
            }
        }
    }

    auto poseDiff(const JointPose& from, const JointPose& to) -> PoseDelta
    {
        PoseDelta delta;
        delta.translation = from.translation - to.translation;
        delta.rotation = glm::normalize(from.rotation * glm::inverse(to.rotation));
        delta.scale = from.scale / glm::max(to.scale, glm::vec3(1e-6f));
        return delta;
    }

    auto applyDelta(const JointPose& base, const PoseDelta& delta, f32 weight) -> JointPose
    {
        JointPose out;
        out.translation = base.translation + delta.translation * weight;
        const glm::quat step = glm::normalize(glm::slerp(glm::quat(1.0f, 0.0f, 0.0f, 0.0f), delta.rotation, weight));
        out.rotation = glm::normalize(step * base.rotation);
        out.scale = base.scale * glm::pow(delta.scale, glm::vec3(weight));
        return out;
    }

    auto solveTwoBoneIk(glm::vec3 root, glm::vec3 mid, glm::vec3 end, glm::vec3 target, glm::vec3 poleVector,
                        f32 upperLen, f32 lowerLen) -> TwoBoneIkResult
    {
        // The returned quaternions are world-space DELTA rotations: pre-multiply each onto the
        // joint's current world rotation, then strip the parent world rotation, to land in
        // local space (the caller does that). Pure law-of-cosines solve (ozz IKTwoBoneJob / UE
        // two-bone): straighten + re-bend the knee to the reach angle, swing the chain onto the
        // target, then twist the bend plane onto the pole.
        TwoBoneIkResult out;
        const f32 a = glm::max(upperLen, 1e-6f);
        const f32 b = glm::max(lowerLen, 1e-6f);

        const glm::vec3 toTarget = target - root;
        const f32 reach = glm::length(toTarget);
        if (reach < 1e-6f)
        {
            return out;  // target on the root: nothing to aim at, stay put
        }
        // Clamp the reach into the chain's range so each acos stays valid (graceful over/under).
        const f32 reachClamped = glm::clamp(reach, glm::abs(a - b) + 1e-4f, a + b - 1e-4f);

        const glm::vec3 startMid = mid - root;
        const glm::vec3 startEnd = end - root;
        const f32 lenStartEnd = glm::length(startEnd);

        auto angleOpposite = [](f32 adj0, f32 adj1, f32 opp) -> f32
        {
            const f32 cosA = glm::clamp((adj0 * adj0 + adj1 * adj1 - opp * opp) / (2.0f * adj0 * adj1), -1.0f, 1.0f);
            return std::acos(cosA);
        };

        // The bend axis: perpendicular to the limb plane. Seed it from the current bend; if the
        // chain is straight, fall back to the pole so the knee has a definite hinge direction.
        glm::vec3 bendAxis = glm::cross(startMid, startEnd);
        if (glm::length(bendAxis) < 1e-6f)
        {
            bendAxis = glm::cross(startMid, poleVector);
            if (glm::length(bendAxis) < 1e-6f)
            {
                bendAxis = glm::cross(startMid, glm::vec3(0.0f, 0.0f, 1.0f));
            }
            if (glm::length(bendAxis) < 1e-6f)
            {
                bendAxis = glm::vec3(0.0f, 0.0f, 1.0f);
            }
        }
        bendAxis = glm::normalize(bendAxis);

        // Knee bend at the mid joint: change the interior angle at the mid (between the reversed
        // upper segment and the lower segment) from its current value to the reach value. Rotating
        // the lower bone about the bend axis by this delta sets |start-end| to the clamped reach.
        f32 currentMidAngle = glm::pi<f32>();  // a degenerate (folded-back) chain reads as pi
        if (lenStartEnd > 1e-6f)
        {
            currentMidAngle = angleOpposite(a, b, lenStartEnd);
        }
        const f32 targetMidAngle = angleOpposite(a, b, reachClamped);
        const f32 bendDelta = targetMidAngle - currentMidAngle;

        // Rotating the lower bone about the bend axis by ±bendDelta both yield a valid chain;
        // pick the sign that lands |start-end| on the reach length (handles the axis-sign
        // ambiguity in cross(startMid, startEnd) without a separate orientation argument).
        glm::quat bend = glm::angleAxis(bendDelta, bendAxis);
        glm::vec3 startEndBent = startMid + bend * (end - mid);
        if (glm::abs(glm::length(startEndBent) - reachClamped) > 1e-3f)
        {
            bend = glm::angleAxis(-bendDelta, bendAxis);
            startEndBent = startMid + bend * (end - mid);
        }
        out.lower = glm::normalize(bend);

        // Swing: rotate the whole chain about the root so the bent start->end points at the
        // target. Applied to the upper joint (the lower inherits it through the hierarchy).
        const glm::quat swing = rotationBetween(startEndBent, toTarget);
        out.upper = glm::normalize(swing);

        // Pole: twist about the root->target axis so the mid joint sits in the plane spanned by
        // the target direction and the pole vector (knee/elbow points the intended way). The swung
        // mid joint (start-relative) is swing*startMid; project it off the chain axis to get the
        // current knee direction, and likewise the desired pole direction.
        const glm::vec3 targetDir = toTarget / reach;
        const glm::vec3 midBent = swing * startMid;  // start-relative mid after the swing
        const glm::vec3 currentPole = midBent - targetDir * glm::dot(midBent, targetDir);
        const glm::vec3 desiredPole = poleVector - targetDir * glm::dot(poleVector, targetDir);
        // Twist the chain about the root->target axis so the knee lands on the pole's side. The
        // rotation MUST be about targetDir (not the shortest arc between the poles): both poles are
        // perpendicular to targetDir, so when they are anti-aligned the shortest arc would pick an
        // arbitrary axis and flip the whole chain off the target. Use the signed angle about
        // targetDir instead, which keeps the solved end exactly on the target. Skip it on a
        // (near-)straight chain, where the knee lies on the axis and the pole plane is undefined.
        const f32 poleScale = glm::max(a, b);
        if (glm::length(currentPole) > 0.02f * poleScale && glm::length(desiredPole) > 1e-5f)
        {
            const glm::vec3 cp = glm::normalize(currentPole);
            const glm::vec3 dp = glm::normalize(desiredPole);
            const f32 twistAngle = std::atan2(glm::dot(glm::cross(cp, dp), targetDir), glm::dot(cp, dp));
            const glm::quat twist = glm::angleAxis(twistAngle, targetDir);
            out.upper = glm::normalize(twist * out.upper);
        }
        return out;
    }

    void tickAnimation(AnimationRuntime& runtime, Scene& scene, f32 dt, AnimMode mode)
    {
        forEach<AnimationPlayerComponent, SkinnedMeshComponent>(
            scene,
            [&](Entity entity, AnimationPlayerComponent& player, SkinnedMeshComponent& skin)
            {
                // Play animates every rig; Edit previews only the timeline-selected one.
                const bool active = mode == AnimMode::Play || player.previewInEdit;
                const AnimClip* clip = nullptr;
                if (active)
                {
                    clip = loadClip(runtime, player.clip);
                }
                u64 key = 0;
                if (hasComponent<IdComponent>(scene, entity))
                {
                    key = getComponent<IdComponent>(scene, entity).id.value;
                }
                if (clip == nullptr)
                {
                    clearOverrides(scene, skin);
                    runtime.transitions.erase(key);
                    runtime.lastPose.erase(key);
                    return;
                }

                // Seed each bone's rest local TRS so untracked joints (and untracked
                // channels of a tracked joint) keep their authored value, and collect the
                // name<->index maps for durable track resolution.
                const std::size_t jointCount = skin.bones.size();
                std::vector<JointPose> rest(jointCount);
                std::vector<std::string> boneNames(jointCount);
                std::unordered_map<std::string, i32> nameToIndex;
                for (std::size_t i = 0; i < jointCount; i = i + 1)
                {
                    rest[i] = restPoseOf(scene, skin, i);
                    if (i < skin.boneHandles.size())
                    {
                        const Entity bone{ skin.boneHandles[i] };
                        if (valid(scene, bone) && hasComponent<NameComponent>(scene, bone))
                        {
                            boneNames[i] = getComponent<NameComponent>(scene, bone).name;
                            nameToIndex.emplace(boneNames[i], static_cast<i32>(i));
                        }
                    }
                }
                auto sampleInto = [&](const AnimClip& source, f32 time) -> std::vector<JointPose>
                {
                    PoseBuffer pose;
                    pose.local = rest;
                    sampleClipResolved(source, time, boneNames, nameToIndex, pose);
                    return std::move(pose.local);
                };
                // The bone's current pose (last frame's override, else rest) — the outgoing
                // pose a just-started transition freezes.
                auto outgoingAt = [&](std::size_t i) -> JointPose
                {
                    if (i < skin.boneHandles.size())
                    {
                        const entt::entity h = skin.boneHandles[i];
                        if (h != entt::null && scene.registry.valid(h))
                        {
                            if (const auto* over = scene.registry.try_get<PoseOverrideComponent>(h))
                            {
                                return JointPose{ over->translation, over->rotation, over->scale };
                            }
                        }
                    }
                    return rest[i];
                };

                // Advance playback, noting a Loop wrap so it can be blended across the seam.
                const f32 prevTime = player.time;
                if (player.playing && clip->duration > 0.0f)
                {
                    advanceTime(player, clip->duration, dt);
                }
                const bool wrapped = player.wrap == AnimationPlayerComponent::Wrap::Loop && player.time < prevTime;
                if (wrapped && player.loopBlend > 0.0f && player.transition >= player.transitionDuration)
                {
                    player.prevClip = player.clip;  // a Loop wrap is a transition from end-pose to start-pose
                    player.transition = 0.0f;
                    player.transitionDuration = player.loopBlend;
                }

                std::vector<JointPose> finalLocal = sampleInto(*clip, player.time);

                const bool transitioning =
                    player.transitionDuration > 0.0f && player.transition < player.transitionDuration;
                if (transitioning)
                {
                    // Freeze the outgoing pose + capture the offset once, at the switch frame.
                    if (player.transition <= 0.0f || !runtime.transitions.contains(key))
                    {
                        TransitionState state;
                        state.outgoing.resize(jointCount);
                        state.offset.resize(jointCount);
                        for (std::size_t i = 0; i < jointCount; i = i + 1)
                        {
                            state.outgoing[i] = outgoingAt(i);
                            state.offset[i] = poseDiff(state.outgoing[i], finalLocal[i]);
                        }
                        runtime.transitions[key] = std::move(state);
                    }
                    const TransitionState& state = runtime.transitions[key];
                    const f32 x = glm::clamp(player.transition / player.transitionDuration, 0.0f, 1.0f);
                    for (std::size_t i = 0; i < jointCount && i < state.offset.size(); i = i + 1)
                    {
                        if (player.transitionMode == AnimationPlayerComponent::Transition::CrossFade)
                        {
                            finalLocal[i] = blendJoint(state.outgoing[i], finalLocal[i], smoothstep01(x));
                        }
                        else
                        {
                            finalLocal[i] = applyDelta(finalLocal[i], state.offset[i], quinticDecay(x));
                        }
                    }
                    player.transition = player.transition + dt;
                    if (player.transition >= player.transitionDuration)
                    {
                        runtime.transitions.erase(key);
                        player.prevClip = Uuid{ 0 };
                        player.transition = 0.0f;
                        player.transitionDuration = 0.0f;
                    }
                }
                else
                {
                    runtime.transitions.erase(key);
                }

                // External pose producer: kinematic foot IK feeds the same override_/weight
                // blend layer ragdoll will use, mixed into finalLocal before the bones are
                // written. Gated on the component so non-IK rigs pay nothing.
                if (const auto* ik = scene.registry.try_get<FootIkComponent>(entity.handle);
                    ik != nullptr && ik->enabled)
                {
                    applyFootIk(scene, skin, *ik, finalLocal);
                }

                for (std::size_t i = 0; i < jointCount; i = i + 1)
                {
                    if (i >= skin.boneHandles.size())
                    {
                        continue;
                    }
                    const entt::entity handle = skin.boneHandles[i];
                    if (handle == entt::null || !scene.registry.valid(handle))
                    {
                        continue;
                    }
                    PoseOverrideComponent& override_ = scene.registry.emplace_or_replace<PoseOverrideComponent>(handle);
                    override_.translation = finalLocal[i].translation;
                    override_.rotation = finalLocal[i].rotation;
                    override_.scale = finalLocal[i].scale;
                }

                // Snapshot this frame's final pose for the next frame's finite-difference
                // velocities (the physics handoff). Cheap; no consumer yet.
                runtime.lastPose[key] = finalLocal;
            });
    }

    auto runAnimationSelfTest() -> Result<void>
    {
        u32 failures = 0;
        auto expect = [&failures](bool condition, std::string_view what)
        {
            if (!condition)
            {
                failures = failures + 1;
                logError(std::format("animation self-test failed: {}", what));
            }
        };
        auto quatClose = [](glm::quat a, glm::quat b) -> bool
        {
            // Quaternions double-cover rotations, so q and -q are the same orientation.
            return glm::abs(glm::dot(a, b)) > 1.0f - 1e-4f;
        };
        constexpr f32 eps = 1e-4f;

        // LINEAR translation: endpoints exact, midpoint lerps, t clamps past the ends.
        {
            AnimTrack track;
            track.path = AnimTrack::Path::Translation;
            track.interp = AnimTrack::Interp::Linear;
            track.times = { 0.0f, 2.0f };
            track.values = { 0.0f, 0.0f, 0.0f, 10.0f, 0.0f, 0.0f };
            expect(glm::distance(glm::vec3(sampleTrack(track, 0.0f)), glm::vec3(0.0f)) < eps, "linear T start");
            expect(glm::distance(glm::vec3(sampleTrack(track, 2.0f)), glm::vec3(10.0f, 0.0f, 0.0f)) < eps,
                   "linear T end");
            expect(glm::distance(glm::vec3(sampleTrack(track, 1.0f)), glm::vec3(5.0f, 0.0f, 0.0f)) < eps,
                   "linear T mid");
            expect(glm::distance(glm::vec3(sampleTrack(track, -1.0f)), glm::vec3(0.0f)) < eps, "linear T clamp low");
            expect(glm::distance(glm::vec3(sampleTrack(track, 9.0f)), glm::vec3(10.0f, 0.0f, 0.0f)) < eps,
                   "linear T clamp high");
        }

        // STEP scale: holds the previous key until the next key's time.
        {
            AnimTrack track;
            track.path = AnimTrack::Path::Scale;
            track.interp = AnimTrack::Interp::Step;
            track.times = { 0.0f, 1.0f };
            track.values = { 1.0f, 1.0f, 1.0f, 3.0f, 3.0f, 3.0f };
            expect(glm::distance(glm::vec3(sampleTrack(track, 0.9f)), glm::vec3(1.0f)) < eps, "step S holds key0");
            expect(glm::distance(glm::vec3(sampleTrack(track, 1.0f)), glm::vec3(3.0f)) < eps, "step S at next key");
        }

        // CUBICSPLINE translation: endpoints exact; asymmetric tangents bend the midpoint
        // to 0.75 (distinct from the linear 0.5), proving the Hermite path runs.
        {
            AnimTrack track;
            track.path = AnimTrack::Path::Translation;
            track.interp = AnimTrack::Interp::CubicSpline;
            track.times = { 0.0f, 1.0f };
            track.values = {
                0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 2.0f, 0.0f, 0.0f,  // key0: in, value, out
                0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,  // key1: in, value, out
            };
            expect(glm::distance(glm::vec3(sampleTrack(track, 0.0f)), glm::vec3(0.0f)) < eps, "cubic T start");
            expect(glm::distance(glm::vec3(sampleTrack(track, 1.0f)), glm::vec3(1.0f, 0.0f, 0.0f)) < eps,
                   "cubic T end");
            expect(glm::abs(sampleTrack(track, 0.5f).x - 0.75f) < eps, "cubic T mid");
        }

        // LINEAR rotation = slerp: 0 deg -> 90 deg about Y, midpoint is exactly 45 deg.
        {
            const f32 s = std::sqrt(0.5f);
            AnimTrack track;
            track.path = AnimTrack::Path::Rotation;
            track.interp = AnimTrack::Interp::Linear;
            track.times = { 0.0f, 1.0f };
            track.values = { 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, s, 0.0f, s };  // xyzw: identity, 90 deg Y
            const glm::quat q0(1.0f, 0.0f, 0.0f, 0.0f);
            const glm::quat q90 = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f));
            const glm::quat q45 = glm::angleAxis(glm::radians(45.0f), glm::vec3(0.0f, 1.0f, 0.0f));
            expect(quatClose(asQuat(sampleTrack(track, 0.0f)), q0), "slerp R start");
            expect(quatClose(asQuat(sampleTrack(track, 1.0f)), q90), "slerp R end");
            expect(quatClose(asQuat(sampleTrack(track, 0.5f)), q45), "slerp R mid");
        }

        // sampleClip integration on joint 0 (T cubic, R slerp, S step); an untracked
        // joint keeps its pre-filled rest value.
        {
            AnimClip clip;
            clip.name = "selftest";
            clip.duration = 1.0f;

            AnimTrack t;
            t.joint = 0;
            t.path = AnimTrack::Path::Translation;
            t.interp = AnimTrack::Interp::CubicSpline;
            t.times = { 0.0f, 1.0f };
            t.values = {
                0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 2.0f, 0.0f, 0.0f,
                0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
            };
            clip.tracks.push_back(t);

            const f32 s = std::sqrt(0.5f);
            AnimTrack r;
            r.joint = 0;
            r.path = AnimTrack::Path::Rotation;
            r.interp = AnimTrack::Interp::Linear;
            r.times = { 0.0f, 1.0f };
            r.values = { 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, s, 0.0f, s };
            clip.tracks.push_back(r);

            AnimTrack sc;
            sc.joint = 0;
            sc.path = AnimTrack::Path::Scale;
            sc.interp = AnimTrack::Interp::Step;
            sc.times = { 0.0f, 1.0f };
            sc.values = { 1.0f, 1.0f, 1.0f, 3.0f, 3.0f, 3.0f };
            clip.tracks.push_back(sc);

            PoseBuffer pose;
            pose.local.resize(2);
            pose.local[0].translation = glm::vec3(99.0f);             // overwritten by the T track
            pose.local[1].translation = glm::vec3(7.0f, 8.0f, 9.0f);  // untracked rest sentinel
            sampleClip(clip, 0.5f, pose);

            expect(glm::abs(pose.local[0].translation.x - 0.75f) < eps, "clip T cubic mid");
            const glm::quat q45 = glm::angleAxis(glm::radians(45.0f), glm::vec3(0.0f, 1.0f, 0.0f));
            expect(quatClose(pose.local[0].rotation, q45), "clip R slerp mid");
            expect(glm::distance(pose.local[0].scale, glm::vec3(1.0f)) < eps, "clip S step holds");
            expect(glm::distance(pose.local[1].translation, glm::vec3(7.0f, 8.0f, 9.0f)) < eps,
                   "untracked joint keeps rest");
        }

        // .sanim IO round-trip: a two-track clip survives save -> load exactly.
        {
            AnimClip clip;
            clip.name = "io_roundtrip";
            clip.duration = 1.0f;
            AnimTrack rot;
            rot.joint = 1;
            rot.jointName = "Tip";
            rot.path = AnimTrack::Path::Rotation;
            rot.interp = AnimTrack::Interp::Linear;
            rot.times = { 0.0f, 1.0f };
            rot.values = { 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f };
            clip.tracks.push_back(rot);
            AnimTrack trans;
            trans.joint = 0;
            trans.jointName = "Root";
            trans.path = AnimTrack::Path::Translation;
            trans.interp = AnimTrack::Interp::Step;
            trans.times = { 0.0f, 0.5f };
            trans.values = { 0.0f, 0.0f, 0.0f, 1.0f, 2.0f, 3.0f };
            clip.tracks.push_back(trans);

            const std::string path = "/tmp/saffron_selftest.sanim";
            auto saved = saveAnimation(clip, path);
            expect(saved.has_value(), "sanim save");
            if (saved)
            {
                auto loaded = loadAnimation(path);
                expect(loaded.has_value(), "sanim load");
                if (loaded)
                {
                    expect(loaded->name == clip.name, "sanim name");
                    expect(glm::abs(loaded->duration - clip.duration) < eps, "sanim duration");
                    expect(loaded->tracks.size() == clip.tracks.size(), "sanim track count");
                    bool tracksMatch = loaded->tracks.size() == clip.tracks.size();
                    for (std::size_t i = 0; i < clip.tracks.size() && i < loaded->tracks.size(); i = i + 1)
                    {
                        const AnimTrack& a = clip.tracks[i];
                        const AnimTrack& b = loaded->tracks[i];
                        tracksMatch = tracksMatch && a.joint == b.joint && a.jointName == b.jointName &&
                                      a.path == b.path && a.interp == b.interp && a.times == b.times &&
                                      a.values == b.values;
                    }
                    expect(tracksMatch, "sanim track fields");
                }
            }
        }

        // Evaluator: a previewInEdit rig writes a PoseOverrideComponent that
        // animates the bone and shows through world composition, while the authored
        // rest-pose TransformComponent stays put; clearing the preview reverts it.
        {
            const f32 s = std::sqrt(0.5f);
            Scene scene;
            Entity rootBone = createEntity(scene, "Root");
            Entity tipBone = createEntity(scene, "Tip");
            Entity meshEntity = createEntity(scene, "Rig");
            SkinnedMeshComponent& skin = addComponent<SkinnedMeshComponent>(scene, meshEntity);
            skin.bones = { getComponent<IdComponent>(scene, rootBone).id,
                           getComponent<IdComponent>(scene, tipBone).id };
            skin.boneHandles = { rootBone.handle, tipBone.handle };
            AnimationPlayerComponent& player = addComponent<AnimationPlayerComponent>(scene, meshEntity);
            player.clip = Uuid{ 1234 };
            player.wrap = AnimationPlayerComponent::Wrap::Loop;

            // Seed the clip cache directly (bypass the catalog/disk): joint 0 rotates 90 deg
            // about Y over 1s, LINEAR.
            AnimationRuntime runtime;
            AnimClip clip;
            clip.name = "spin";
            clip.duration = 1.0f;
            AnimTrack rot;
            rot.joint = 0;
            rot.jointName = "Root";
            rot.path = AnimTrack::Path::Rotation;
            rot.interp = AnimTrack::Interp::Linear;
            rot.times = { 0.0f, 1.0f };
            rot.values = { 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, s, 0.0f, s };
            clip.tracks.push_back(rot);
            runtime.clipCache.emplace(player.clip.value, clip);

            const glm::quat q45 = glm::angleAxis(glm::radians(45.0f), glm::vec3(0.0f, 1.0f, 0.0f));

            // Edit, no preview: nothing animates, no override appears.
            tickAnimation(runtime, scene, 0.5f, AnimMode::Edit);
            expect(!scene.registry.all_of<PoseOverrideComponent>(rootBone.handle), "edit without preview is inert");

            // Edit + preview + playing: the bone is driven to the 45 deg midpoint.
            player.previewInEdit = true;
            player.playing = true;
            player.time = 0.0f;
            tickAnimation(runtime, scene, 0.5f, AnimMode::Edit);
            expect(glm::abs(player.time - 0.5f) < eps, "edit preview advances the playhead");
            const auto* over = scene.registry.try_get<PoseOverrideComponent>(rootBone.handle);
            expect(over != nullptr, "preview writes a pose override");
            if (over != nullptr)
            {
                expect(quatClose(over->rotation, q45), "override holds the sampled rotation");
            }
            // Rest pose untouched (non-destructive), so preview never dirties the project.
            expect(glm::length(getComponent<TransformComponent>(scene, rootBone).rotation) < eps,
                   "rest-pose TransformComponent stays at identity");
            // World composition prefers the override.
            updateWorldTransforms(scene);
            expect(quatClose(worldRotation(scene, rootBone), q45), "world transform reflects the override");

            // Clearing the preview reverts the bone to rest next tick.
            player.previewInEdit = false;
            tickAnimation(runtime, scene, 0.0f, AnimMode::Edit);
            expect(!scene.registry.all_of<PoseOverrideComponent>(rootBone.handle),
                   "clearing preview removes the override");
            updateWorldTransforms(scene);
            expect(quatClose(worldRotation(scene, rootBone), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)),
                   "bone reverts to rest after preview clears");

            // Play animates every rig regardless of previewInEdit.
            player.previewInEdit = false;
            player.playing = true;
            player.time = 0.0f;
            tickAnimation(runtime, scene, 0.5f, AnimMode::Play);
            expect(scene.registry.all_of<PoseOverrideComponent>(rootBone.handle), "play animates without preview");
        }

        // PoseDelta: the delta carries `to` onto `from` at weight 1 and is the
        // identity at weight 0 — the reusable offset machinery transitions build on.
        {
            JointPose from;
            from.translation = glm::vec3(1.0f, 2.0f, 3.0f);
            from.rotation = glm::angleAxis(glm::radians(30.0f), glm::vec3(0.0f, 1.0f, 0.0f));
            from.scale = glm::vec3(2.0f);
            JointPose to;  // identity rest
            const PoseDelta delta = poseDiff(from, to);
            const JointPose full = applyDelta(to, delta, 1.0f);
            const JointPose none = applyDelta(to, delta, 0.0f);
            expect(glm::distance(full.translation, from.translation) < eps, "applyDelta w=1 translation");
            expect(quatClose(full.rotation, from.rotation), "applyDelta w=1 rotation");
            expect(glm::distance(full.scale, from.scale) < eps, "applyDelta w=1 scale");
            expect(glm::distance(none.translation, to.translation) < eps && quatClose(none.rotation, to.rotation),
                   "applyDelta w=0 is the base");
        }

        // Transitions: a 90 deg Y clip, switched in from the rest (identity)
        // outgoing pose. Cross-fade and inertialization both start at the outgoing pose and
        // end at the incoming clip; inertialization is C0 at the switch (no pop).
        {
            const f32 s = std::sqrt(0.5f);
            const glm::quat q90 = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f));
            AnimClip clip;
            clip.name = "spin90";
            clip.duration = 1.0f;
            AnimTrack rot;
            rot.joint = 0;
            rot.jointName = "J0";
            rot.path = AnimTrack::Path::Rotation;
            rot.interp = AnimTrack::Interp::Linear;
            rot.times = { 0.0f, 1.0f };
            rot.values = { 0.0f, s, 0.0f, s, 0.0f, s, 0.0f, s };
            clip.tracks.push_back(rot);

            auto run = [&](AnimationPlayerComponent::Transition mode, glm::quat& switchRot, glm::quat& endRot)
            {
                Scene scene;
                Entity bone = createEntity(scene, "J0");
                Entity meshEntity = createEntity(scene, "Rig");
                SkinnedMeshComponent& skin = addComponent<SkinnedMeshComponent>(scene, meshEntity);
                skin.bones = { getComponent<IdComponent>(scene, bone).id };
                skin.boneHandles = { bone.handle };
                AnimationPlayerComponent& player = addComponent<AnimationPlayerComponent>(scene, meshEntity);
                player.clip = Uuid{ 9001 };
                player.previewInEdit = true;
                player.playing = true;
                player.transitionMode = mode;
                player.prevClip = Uuid{ 1 };  // a distinct outgoing clip id (its pose comes from the bone)
                player.transition = 0.0f;
                player.transitionDuration = 1.0f;
                AnimationRuntime runtime;
                runtime.clipCache.emplace(player.clip.value, clip);

                tickAnimation(runtime, scene, 0.5f, AnimMode::Edit);  // switch frame, x=0
                switchRot = scene.registry.get<PoseOverrideComponent>(bone.handle).rotation;
                tickAnimation(runtime, scene, 0.5f, AnimMode::Edit);  // x=0.5 -> 1, transition ends
                tickAnimation(runtime, scene, 0.5f, AnimMode::Edit);  // steady incoming
                endRot = scene.registry.get<PoseOverrideComponent>(bone.handle).rotation;
            };

            glm::quat cfSwitch;
            glm::quat cfEnd;
            run(AnimationPlayerComponent::Transition::CrossFade, cfSwitch, cfEnd);
            expect(quatClose(cfSwitch, glm::quat(1.0f, 0.0f, 0.0f, 0.0f)), "crossfade starts at the outgoing pose");
            expect(quatClose(cfEnd, q90), "crossfade ends at the incoming clip");

            glm::quat inSwitch;
            glm::quat inEnd;
            run(AnimationPlayerComponent::Transition::Inertialize, inSwitch, inEnd);
            expect(quatClose(inSwitch, glm::quat(1.0f, 0.0f, 0.0f, 0.0f)),
                   "inertialization is C0 at the switch (no pop)");
            expect(quatClose(inEnd, q90), "inertialization ends at the incoming clip");
        }

        // Loop-wrap blend: a clip whose start (0 deg) and end (90 deg Y) differ
        // would pop at the wrap; loopBlend inertializes across it, so the wrap frame holds
        // the pre-wrap (end) pose rather than snapping to the start.
        {
            const f32 s = std::sqrt(0.5f);
            Scene scene;
            Entity bone = createEntity(scene, "J0");
            Entity meshEntity = createEntity(scene, "Rig");
            SkinnedMeshComponent& skin = addComponent<SkinnedMeshComponent>(scene, meshEntity);
            skin.bones = { getComponent<IdComponent>(scene, bone).id };
            skin.boneHandles = { bone.handle };
            AnimationPlayerComponent& player = addComponent<AnimationPlayerComponent>(scene, meshEntity);
            player.clip = Uuid{ 9002 };
            player.previewInEdit = true;
            player.playing = true;
            player.wrap = AnimationPlayerComponent::Wrap::Loop;
            player.loopBlend = 0.5f;
            player.time = 0.8f;

            AnimClip clip;
            clip.name = "ramp";
            clip.duration = 1.0f;
            AnimTrack rot;
            rot.joint = 0;
            rot.jointName = "J0";
            rot.path = AnimTrack::Path::Rotation;
            rot.interp = AnimTrack::Interp::Linear;
            rot.times = { 0.0f, 1.0f };
            rot.values = { 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, s, 0.0f, s };  // identity -> 90 deg Y
            clip.tracks.push_back(rot);
            AnimationRuntime runtime;
            runtime.clipCache.emplace(player.clip.value, clip);

            tickAnimation(runtime, scene, 0.1f, AnimMode::Edit);  // time -> 0.9, near the end pose
            const glm::quat preWrap = scene.registry.get<PoseOverrideComponent>(bone.handle).rotation;
            tickAnimation(runtime, scene, 0.2f, AnimMode::Edit);  // time wraps past the end to ~0.1
            const glm::quat wrapFrame = scene.registry.get<PoseOverrideComponent>(bone.handle).rotation;
            // With the blend the wrap frame stays at the pre-wrap pose; a hard cut would jump
            // ~72 deg toward the start pose, which quatClose would reject.
            expect(quatClose(wrapFrame, preWrap), "loop wrap holds the pre-wrap pose (no pop)");
        }

        // Two-bone IK: the solver returns world deltas that, composed onto the
        // chain's (identity) bone world rotations, place the end on an in-range target exactly,
        // and clamp gracefully (straight chain toward the target, no NaN) when over-reached.
        {
            const glm::vec3 root(0.0f, 0.0f, 0.0f);
            const glm::vec3 mid(1.0f, 0.0f, 0.0f);
            const glm::vec3 end(2.0f, 0.0f, 0.0f);  // straight chain along +X, lengths 1 + 1
            const glm::vec3 pole(0.0f, 1.0f, 0.0f);
            const f32 upperLen = 1.0f;
            const f32 lowerLen = 1.0f;

            // Compose the world deltas onto the chain (bone world rotations start at identity).
            auto solvedEnd = [&](const TwoBoneIkResult& r) -> glm::vec3
            {
                const glm::vec3 newMid = root + r.upper * (mid - root);
                return newMid + r.upper * r.lower * (end - mid);
            };
            auto hasNaN = [](const glm::vec3& v) -> bool { return glm::any(glm::isnan(v)); };

            // In range: target reachable, end lands on it exactly.
            {
                const glm::vec3 target(1.0f, 1.0f, 0.0f);  // |root->target| = sqrt(2), in [0, 2]
                const TwoBoneIkResult r = solveTwoBoneIk(root, mid, end, target, pole, upperLen, lowerLen);
                const glm::vec3 reached = solvedEnd(r);
                expect(!hasNaN(reached), "two-bone IK in-range has no NaN");
                expect(glm::distance(reached, target) < 1e-3f, "two-bone IK reaches an in-range target exactly");
            }

            // Bent chain start (the common case): mid already off-axis, still reaches. The
            // segment lengths match upperLen/lowerLen exactly so the law of cosines is consistent.
            {
                const glm::vec3 bentMid(0.5f, 0.86602540f, 0.0f);  // |root->mid| = 1
                const glm::vec3 bentEnd(1.5f, 0.86602540f, 0.0f);  // |mid->end| = 1 (along +X)
                const glm::vec3 target(0.5f, -1.2f, 0.3f);         // |root->target| = ~1.34, in [0, 2]
                const TwoBoneIkResult r = solveTwoBoneIk(root, bentMid, bentEnd, target, pole, upperLen, lowerLen);
                const glm::vec3 newMid = root + r.upper * (bentMid - root);
                const glm::vec3 reached = newMid + r.upper * r.lower * (bentEnd - bentMid);
                expect(glm::distance(reached, target) < 1e-3f, "two-bone IK reaches from a pre-bent chain");
            }

            // Over-extended: target past the chain's reach. The chain straightens toward the
            // target and the end lands at max reach along the target direction (no NaN).
            {
                const glm::vec3 target(5.0f, 0.0f, 0.0f);  // distance 5 > maxReach 2
                const TwoBoneIkResult r = solveTwoBoneIk(root, mid, end, target, pole, upperLen, lowerLen);
                const glm::vec3 reached = solvedEnd(r);
                expect(!hasNaN(reached), "two-bone IK over-reach has no NaN");
                const f32 dist = glm::length(reached - root);
                expect(glm::abs(dist - (upperLen + lowerLen)) < 1e-2f,
                       "two-bone IK clamps an over-reach to a straight chain");
                expect(glm::length(glm::normalize(reached - root) - glm::vec3(1.0f, 0.0f, 0.0f)) < 1e-2f,
                       "two-bone IK aims the clamped chain at the target");
            }
        }

        if (failures != 0)
        {
            return Err(std::format("animation self-test: {} check(s) failed", failures));
        }
        logInfo("animation self-test: all checks passed");
        return {};
    }
}
