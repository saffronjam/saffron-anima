//! Pose-delta arithmetic, the cross-fade joint blend, and the transition weight
//! curves.

use glam::{Quat, Vec3};

use crate::pose::{JointPose, PoseDelta};

/// The delta that takes `to` onto `from`: `apply_delta(to, pose_diff(from, to), 1) == from`.
///
/// Additive translation, a delta quaternion (`from * inverse(to)`), and a
/// per-component scale ratio with a `1e-6` floor on the divisor.
pub fn pose_diff(from: &JointPose, to: &JointPose) -> PoseDelta {
    PoseDelta {
        translation: from.translation - to.translation,
        rotation: (from.rotation * to.rotation.inverse()).normalize(),
        scale: from.scale / to.scale.max(Vec3::splat(1e-6)),
    }
}

/// `base` shifted by `weight`·`delta`.
///
/// Weight `0` returns `base`; weight `1` returns the pose `delta` was built from.
/// The rotation slerps from identity and the scale raises the ratio to `weight`.
pub fn apply_delta(base: &JointPose, delta: &PoseDelta, weight: f32) -> JointPose {
    let step = Quat::IDENTITY.slerp(delta.rotation, weight).normalize();
    JointPose {
        translation: base.translation + delta.translation * weight,
        rotation: (step * base.rotation).normalize(),
        scale: base.scale * delta.scale.powf(weight),
    }
}

/// Per-joint blend of two poses (lerp T/S, slerp R) — the cross-fade primitive.
pub fn blend_joint(base: &JointPose, over: &JointPose, weight: f32) -> JointPose {
    JointPose {
        translation: base.translation.lerp(over.translation, weight),
        rotation: base.rotation.slerp(over.rotation, weight).normalize(),
        scale: base.scale.lerp(over.scale, weight),
    }
}

/// Cubic ease (C¹) for the cross-fade alpha.
pub fn smoothstep01(x: f32) -> f32 {
    let x = x.clamp(0.0, 1.0);
    x * x * (3.0 - 2.0 * x)
}

/// Quintic decay `1 → 0` with zero value, slope, and acceleration at `x = 1`
/// (C², zero-jerk): the inertialization offset weight as the transition runs out.
pub fn quintic_decay(x: f32) -> f32 {
    let x = x.clamp(0.0, 1.0);
    let smoother = x * x * x * (x * (x * 6.0 - 15.0) + 10.0);
    1.0 - smoother
}
