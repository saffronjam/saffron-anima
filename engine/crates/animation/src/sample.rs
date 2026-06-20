//! Track and clip samplers: the deterministic pose-evaluation core.

use glam::{Quat, Vec4};
use saffron_geometry::{AnimClip, AnimInterp, AnimPath, AnimTrack};

use crate::pose::PoseBuffer;

/// Sample one track at time `t`.
///
/// Returns a [`Vec4`] whose `.xyz` is the value for Translation/Scale, or whose
/// four lanes are a normalized quaternion in `xyzw` order for Rotation (no
/// swizzle: glam's `Vec4` and `Quat` share the layout, so `Quat::from_vec4` reads
/// it directly).
///
/// `Step` holds the previous key, `Linear` lerps (slerp for rotation, normalized),
/// `CubicSpline` is a Hermite spline with `dt`-scaled tangents; `t` clamps to
/// `[first key, last key]` (no extrapolation).
pub fn sample_track(track: &AnimTrack, t: f32) -> Vec4 {
    let rotation = track.path == AnimPath::Rotation;
    let cc: usize = if rotation { 4 } else { 3 };
    let stride = cc;
    let times = &track.times;
    let n = times.len();

    if n == 0 || track.values.is_empty() {
        return match track.path {
            AnimPath::Rotation => Vec4::new(0.0, 0.0, 0.0, 1.0),
            AnimPath::Scale => Vec4::new(1.0, 1.0, 1.0, 0.0),
            AnimPath::Translation => Vec4::ZERO,
        };
    }

    // CubicSpline stores [in-tangent, value, out-tangent] per key (3x stride); the
    // sampled value sits one stride in. Step/Linear store the value flat.
    let value_offset = |key: usize| -> usize {
        if track.interp == AnimInterp::CubicSpline {
            key * 3 * stride + stride
        } else {
            key * stride
        }
    };
    let read_vec4 = |offset: usize| -> Vec4 {
        let mut r = Vec4::ZERO;
        for c in 0..cc {
            r[c] = track.values[offset + c];
        }
        r
    };
    let finish = |v: Vec4| -> Vec4 {
        if rotation {
            let q = Quat::from_vec4(v).normalize();
            Vec4::new(q.x, q.y, q.z, q.w)
        } else {
            v
        }
    };

    if t <= times[0] {
        return finish(read_vec4(value_offset(0)));
    }
    if t >= times[n - 1] {
        return finish(read_vec4(value_offset(n - 1)));
    }

    // The first key strictly greater than `t` (the C++ `upper_bound`).
    let i1 = times.partition_point(|&time| time <= t);
    let i0 = i1 - 1;
    let dt = times[i1] - times[i0];
    let local = if dt > 0.0 { (t - times[i0]) / dt } else { 0.0 };

    if track.interp == AnimInterp::Step {
        return finish(read_vec4(value_offset(i0)));
    }

    if track.interp == AnimInterp::Linear {
        if rotation {
            let a = Quat::from_vec4(read_vec4(value_offset(i0))).normalize();
            let b = Quat::from_vec4(read_vec4(value_offset(i1))).normalize();
            let q = a.slerp(b, local).normalize();
            return Vec4::new(q.x, q.y, q.z, q.w);
        }
        return read_vec4(value_offset(i0)).lerp(read_vec4(value_offset(i1)), local);
    }

    let t2 = local * local;
    let t3 = t2 * local;
    let h00 = 2.0 * t3 - 3.0 * t2 + 1.0;
    let h10 = t3 - 2.0 * t2 + local;
    let h01 = -2.0 * t3 + 3.0 * t2;
    let h11 = t3 - t2;
    let p0 = read_vec4(i0 * 3 * stride + stride);
    let p1 = read_vec4(i1 * 3 * stride + stride);
    let m0 = read_vec4(i0 * 3 * stride + 2 * stride) * dt;
    let m1 = read_vec4(i1 * 3 * stride) * dt;
    finish(h00 * p0 + h10 * m0 + h01 * p1 + h11 * m1)
}

/// Sample a whole clip at time `t` into `out.local`.
///
/// The caller sizes `out.local` to the joint count and pre-fills it with the rest
/// pose; only joints with a track are written, so untracked joints (and untracked
/// channels of a tracked joint) keep their bind/rest value.
pub fn sample_clip(clip: &AnimClip, t: f32, out: &mut PoseBuffer) {
    for track in &clip.tracks {
        if track.joint < 0 {
            continue;
        }
        let j = track.joint as usize;
        if j >= out.local.len() {
            continue;
        }
        let v = sample_track(track, t);
        match track.path {
            AnimPath::Translation => out.local[j].translation = v.truncate(),
            AnimPath::Rotation => out.local[j].rotation = Quat::from_vec4(v),
            AnimPath::Scale => out.local[j].scale = v.truncate(),
        }
    }
}
