//! Track and clip samplers: the deterministic pose-evaluation core.

use glam::{Quat, Vec4};
use saffron_geometry::{AnimInterp, AnimPath, AnimTrack};

/// The keyframe bracket at time `t`: the surrounding key indices, the normalized
/// in-segment position, and the segment `dt` (for CubicSpline tangent scaling).
///
/// `i0 == i1` means `t` clamped to an endpoint (hold that key, no interpolation).
struct KeyBracket {
    i0: usize,
    i1: usize,
    local: f32,
    dt: f32,
}

/// Locate the keyframe bracket for time `t` over the strictly-increasing `times`.
///
/// Shared by [`sample_track`] and [`sample_weights`]; clamps to `[first, last]` (no
/// extrapolation). `None` only for an empty `times`.
fn locate_keys(times: &[f32], t: f32) -> Option<KeyBracket> {
    let n = times.len();
    if n == 0 {
        return None;
    }
    if t <= times[0] {
        return Some(KeyBracket {
            i0: 0,
            i1: 0,
            local: 0.0,
            dt: 0.0,
        });
    }
    if t >= times[n - 1] {
        return Some(KeyBracket {
            i0: n - 1,
            i1: n - 1,
            local: 0.0,
            dt: 0.0,
        });
    }
    // The first key strictly greater than `t`.
    let i1 = times.partition_point(|&time| time <= t);
    let i0 = i1 - 1;
    let dt = times[i1] - times[i0];
    let local = if dt > 0.0 { (t - times[i0]) / dt } else { 0.0 };
    Some(KeyBracket { i0, i1, local, dt })
}

/// Sample one T/R/S track at time `t`.
///
/// Returns a [`Vec4`] whose `.xyz` is the value for Translation/Scale, or whose four lanes
/// are a normalized quaternion in `xyzw` order for Rotation (no swizzle: glam's `Vec4` and
/// `Quat` share the layout, so `Quat::from_vec4` reads it directly).
///
/// `Step` holds the previous key, `Linear` lerps (slerp for rotation, normalized),
/// `CubicSpline` is a Hermite spline with `dt`-scaled tangents; `t` clamps to
/// `[first key, last key]` (no extrapolation).
pub fn sample_track(track: &AnimTrack, t: f32) -> Vec4 {
    let rotation = track.path == AnimPath::Rotation;
    let cc: usize = if rotation { 4 } else { 3 };
    let stride = cc;

    if track.times.is_empty() || track.values.is_empty() {
        return match track.path {
            AnimPath::Rotation => Vec4::new(0.0, 0.0, 0.0, 1.0),
            AnimPath::Scale => Vec4::new(1.0, 1.0, 1.0, 0.0),
            // `Weights` is N-wide and sampled by `sample_weights`, never as a `Vec4`.
            AnimPath::Translation | AnimPath::Weights => Vec4::ZERO,
        };
    }
    let Some(b) = locate_keys(&track.times, t) else {
        return Vec4::ZERO;
    };

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
            r[c] = track.values.get(offset + c).copied().unwrap_or(0.0);
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

    if b.i0 == b.i1 {
        return finish(read_vec4(value_offset(b.i0)));
    }

    if track.interp == AnimInterp::Step {
        return finish(read_vec4(value_offset(b.i0)));
    }

    if track.interp == AnimInterp::Linear {
        if rotation {
            let a = Quat::from_vec4(read_vec4(value_offset(b.i0))).normalize();
            let c = Quat::from_vec4(read_vec4(value_offset(b.i1))).normalize();
            let q = a.slerp(c, b.local).normalize();
            return Vec4::new(q.x, q.y, q.z, q.w);
        }
        return read_vec4(value_offset(b.i0)).lerp(read_vec4(value_offset(b.i1)), b.local);
    }

    let t2 = b.local * b.local;
    let t3 = t2 * b.local;
    let h00 = 2.0 * t3 - 3.0 * t2 + 1.0;
    let h10 = t3 - 2.0 * t2 + b.local;
    let h01 = -2.0 * t3 + 3.0 * t2;
    let h11 = t3 - t2;
    let p0 = read_vec4(b.i0 * 3 * stride + stride);
    let p1 = read_vec4(b.i1 * 3 * stride + stride);
    let m0 = read_vec4(b.i0 * 3 * stride + 2 * stride) * b.dt;
    let m1 = read_vec4(b.i1 * 3 * stride) * b.dt;
    finish(h00 * p0 + h10 * m0 + h01 * p1 + h11 * m1)
}

/// Sample an N-wide morph-weight track at time `t` into `out`.
///
/// Writes `track.morph_count` lanes (clamped to `out.len()`); morph weights are
/// independent scalars, so there is no slerp and no normalization. `Step` holds the
/// previous key, `Linear` lerps per lane, `CubicSpline` runs the per-lane Hermite with
/// `dt`-scaled tangents. An empty clip / zero `morph_count` leaves `out` as the caller
/// seeded it (the rest weights), rather than zeroing.
pub fn sample_weights(track: &AnimTrack, t: f32, out: &mut [f32]) {
    let n = track.morph_count as usize;
    if n == 0 || track.times.is_empty() || track.values.is_empty() {
        return;
    }
    let Some(b) = locate_keys(&track.times, t) else {
        return;
    };
    let cubic = track.interp == AnimInterp::CubicSpline;
    // Per-key value layout: CubicSpline = [in[n], value[n], out[n]] (3n); else [value[n]].
    let stride = if cubic { 3 * n } else { n };
    let value_at = |key: usize, lane: usize| -> f32 {
        let base = if cubic {
            key * stride + n
        } else {
            key * stride
        };
        track.values.get(base + lane).copied().unwrap_or(0.0)
    };

    let lanes = n.min(out.len());
    for (lane, slot) in out.iter_mut().enumerate().take(lanes) {
        let value = if b.i0 == b.i1 || track.interp == AnimInterp::Step {
            value_at(b.i0, lane)
        } else if track.interp == AnimInterp::Linear {
            let a = value_at(b.i0, lane);
            let c = value_at(b.i1, lane);
            a + (c - a) * b.local
        } else {
            let t2 = b.local * b.local;
            let t3 = t2 * b.local;
            let h00 = 2.0 * t3 - 3.0 * t2 + 1.0;
            let h10 = t3 - 2.0 * t2 + b.local;
            let h01 = -2.0 * t3 + 3.0 * t2;
            let h11 = t3 - t2;
            let p0 = value_at(b.i0, lane);
            let p1 = value_at(b.i1, lane);
            // Out-tangent of key i0 sits at `i0*stride + 2n`; in-tangent of i1 at `i1*stride`.
            let m0 = track
                .values
                .get(b.i0 * stride + 2 * n + lane)
                .copied()
                .unwrap_or(0.0)
                * b.dt;
            let m1 = track
                .values
                .get(b.i1 * stride + lane)
                .copied()
                .unwrap_or(0.0)
                * b.dt;
            h00 * p0 + h10 * m0 + h01 * p1 + h11 * m1
        };
        *slot = value;
    }
}
