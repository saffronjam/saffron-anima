module;

// glm is a header-heavy C++ library, so this module uses classic includes
// (no `import std`), like the geometry/scene modules.
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <expected>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

export module Saffron.Animation;

import Saffron.Core;
import Saffron.Geometry;
import Saffron.Scene;

export namespace se
{
    /// A single joint's local transform, decomposed — the form clips sample into and
    /// the blend layer operates on. Rotation is a unit quaternion (w, x, y, z).
    struct JointPose
    {
        glm::vec3 translation{ 0.0f };
        glm::quat rotation{ 1.0f, 0.0f, 0.0f, 0.0f };
        glm::vec3 scale{ 1.0f };
    };

    /// A skeleton-sized pose, indexed 1:1 with SkinnedMeshComponent.bones. `local` is
    /// the sampled/animated TRS; `override_` is where external producers (IK/physics)
    /// write (Phase 13+); `weight` is the inert per-bone blend layer (v1 leaves it 0,
    /// meaning pure animation). `override_`/`weight` are empty/zero until a producer fills them.
    struct PoseBuffer
    {
        std::vector<JointPose> local;
        std::vector<JointPose> override_;
        std::vector<f32> weight;
    };

    /// A per-joint pose offset — the delta that carries one pose onto another: additive
    /// translation, a delta quaternion (`from * inverse(to)`), and a multiplicative scale
    /// ratio. The same delta-pose machinery a physics handoff (ragdoll) uses to nudge an
    /// animated target, so it lives here where it is easy to test before it is load-bearing.
    struct PoseDelta
    {
        glm::vec3 translation{ 0.0f };
        glm::quat rotation{ 1.0f, 0.0f, 0.0f, 0.0f };
        glm::vec3 scale{ 1.0f };
    };

    /// The delta that takes `to` onto `from`: `applyDelta(to, poseDiff(from, to), 1) == from`.
    auto poseDiff(const JointPose& from, const JointPose& to) -> PoseDelta;
    /// `base` shifted by `weight`·`delta` (weight 0 → base, 1 → the pose `delta` was built from).
    auto applyDelta(const JointPose& base, const PoseDelta& delta, f32 weight) -> JointPose;

    /// The two world-space joint rotations a two-bone solve produces (upper = thigh/upper-arm,
    /// lower = shin/forearm). The end effector is positioned by composing these onto the chain.
    struct TwoBoneIkResult
    {
        glm::quat upper{ 1.0f, 0.0f, 0.0f, 0.0f };
        glm::quat lower{ 1.0f, 0.0f, 0.0f, 0.0f };
    };

    /// Solve a 2-bone chain (e.g. thigh→shin→foot) so the end effector reaches `target`,
    /// bending around `poleVector`. `root`/`mid`/`end` are the chain's current world
    /// positions; `upperLen`/`lowerLen` are the segment lengths. Returns world-space
    /// rotations for the upper and lower joints (the caller removes parent world rotation to
    /// land in local space). Pure: a target inside `[|upperLen-lowerLen|, upperLen+lowerLen]`
    /// is reached exactly; an over/under-reach clamps to a straight chain aimed at the target.
    auto solveTwoBoneIk(glm::vec3 root, glm::vec3 mid, glm::vec3 end, glm::vec3 target, glm::vec3 poleVector,
                        f32 upperLen, f32 lowerLen) -> TwoBoneIkResult;

    /// Sample one track at time t. Returns a vec3 in .xyz for Translation/Scale, or a
    /// normalized quaternion as xyzw for Rotation. STEP holds the previous key, LINEAR
    /// lerps (slerp for rotation), CUBICSPLINE is a Hermite spline with dt-scaled
    /// tangents; t is clamped to [first key, last key] (no extrapolation).
    auto sampleTrack(const AnimTrack& track, f32 t) -> glm::vec4;

    /// Sample a whole clip at time t into `out.local` (sized to the joint count by the
    /// caller and pre-filled with the rest pose). Only joints with a track are written;
    /// joints with no track keep their bind/rest value.
    void sampleClip(const AnimClip& clip, f32 t, PoseBuffer& out);

    /// Edit previews a single rig's clip non-destructively; Play advances every rig.
    enum class AnimMode : u8
    {
        Edit,
        Play,
    };

    /// An in-flight clip switch, captured once at the switch and decayed over the
    /// transition (runtime-only, keyed by the mesh entity's uuid).
    struct TransitionState
    {
        std::vector<JointPose> outgoing;  // the frozen outgoing pose (cross-fade)
        std::vector<PoseDelta> offset;    // outgoing − incoming-at-switch (inertialization)
    };

    /// Per-session animation state: clip Uuid -> loaded AnimClip. The Host owns one and
    /// clears it on project (re)load so a reimported clip is picked up fresh.
    struct AnimationRuntime
    {
        std::unordered_map<u64, AnimClip> clipCache;
        std::unordered_map<u64, TransitionState> transitions;  // active clip switches by entity uuid
        // Each rig's previous-frame final local pose (by entity uuid). Snapshotted at the end
        // of every tick; the eventual physics handoff finite-differences it for per-bone
        // velocities so a ragdoll take-over does not pop. No consumer yet — reserved.
        std::unordered_map<u64, std::vector<JointPose>> lastPose;
    };

    /// Sample and (when playing) advance every AnimationPlayerComponent on a SkinnedMesh
    /// rig, writing a PoseOverrideComponent onto each driven bone — and removing it from an
    /// inactive rig's bones so they fall back to the authored rest pose. In Play every rig
    /// animates; in Edit only a `previewInEdit` rig does. `catalog` + `assetRoot` resolve a
    /// clip Uuid to its `.sanim` (loaded once into `runtime`). Never writes a bone's
    /// TransformComponent, so the rest pose and the project's dirty state stay untouched.
    void tickAnimation(AnimationRuntime& runtime, Scene& scene, const AssetCatalog& catalog, std::string_view assetRoot,
                       f32 dt, AnimMode mode);

    /// Headless unit gate (mirrors the jointMatrices self-test): samples known STEP/
    /// LINEAR/CUBICSPLINE keys and asserts endpoints are exact and midpoints match
    /// (slerp midpoint for rotation). An Err means the sampling math is broken.
    auto runAnimationSelfTest() -> Result<void>;
}
