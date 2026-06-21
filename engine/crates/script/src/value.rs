//! `sa.Vec3`: the one value type scripts construct, a `glam::Vec3`-backed
//! `UserData` with the math + operator surface.
//!
//! The read-write `x`/`y`/`z` fields, the `length`/`normalized`/`dot`/`cross`/`lerp`
//! methods, and the `Add`/`Sub`/`Mul`/`Unm`/`Eq`/`ToString` metamethods. The dual-operand
//! `__mul` is one [`mlua`] `Mul` meta-function that sees both operands as values and
//! dispatches on which is the number.

use glam::Vec3;
use mlua::{MetaMethod, UserData, UserDataMethods, UserDataRef, Value};

use saffron_scene::quat_to_euler_zyx;

/// The `sa.Vec3` value userdata: a `glam::Vec3` exposed to script with read-write
/// `x`/`y`/`z`, vector math methods, and the arithmetic metamethods.
///
/// `Copy` because the inner [`Vec3`] is — script values are by-value, never shared
/// handles, so there is no aliasing for a mutated component to bleed across.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct SaVec3(pub Vec3);

impl SaVec3 {
    /// Wraps a `glam::Vec3` as the script value type.
    #[must_use]
    pub fn new(v: Vec3) -> Self {
        Self(v)
    }
}

impl UserData for SaVec3 {
    fn add_fields<F: mlua::UserDataFields<Self>>(fields: &mut F) {
        fields.add_field_method_get("x", |_, this| Ok(this.0.x));
        fields.add_field_method_set("x", |_, this, value: f32| {
            this.0.x = value;
            Ok(())
        });
        fields.add_field_method_get("y", |_, this| Ok(this.0.y));
        fields.add_field_method_set("y", |_, this, value: f32| {
            this.0.y = value;
            Ok(())
        });
        fields.add_field_method_get("z", |_, this| Ok(this.0.z));
        fields.add_field_method_set("z", |_, this, value: f32| {
            this.0.z = value;
            Ok(())
        });
    }

    fn add_methods<M: UserDataMethods<Self>>(methods: &mut M) {
        methods.add_method("length", |_, this, ()| Ok(this.0.length()));
        methods.add_method("normalized", |_, this, ()| Ok(SaVec3(normalized(this.0))));
        methods.add_method("dot", |_, this, other: UserDataRef<SaVec3>| {
            Ok(this.0.dot(other.0))
        });
        methods.add_method("cross", |_, this, other: UserDataRef<SaVec3>| {
            Ok(SaVec3(this.0.cross(other.0)))
        });
        methods.add_method("lerp", |_, this, (other, t): (UserDataRef<SaVec3>, f32)| {
            Ok(SaVec3(this.0.lerp(other.0, t)))
        });

        methods.add_meta_method(MetaMethod::Add, |_, this, other: UserDataRef<SaVec3>| {
            Ok(SaVec3(this.0 + other.0))
        });
        methods.add_meta_method(MetaMethod::Sub, |_, this, other: UserDataRef<SaVec3>| {
            Ok(SaVec3(this.0 - other.0))
        });
        methods.add_meta_method(MetaMethod::Unm, |_, this, ()| Ok(SaVec3(-this.0)));
        methods.add_meta_method(MetaMethod::Eq, |_, this, other: UserDataRef<SaVec3>| {
            Ok(this.0 == other.0)
        });
        methods.add_meta_method(MetaMethod::ToString, |_, this, ()| {
            Ok(format!("Vec3({}, {}, {})", this.0.x, this.0.y, this.0.z))
        });

        // One handler for both operand orders: `vec * scalar` and `scalar * vec`.
        // Luau calls `__mul(a, b)` in source order, so for `scalar * vec` the left
        // operand is the number — dispatch on which operand is the vector.
        methods.add_meta_function(MetaMethod::Mul, |_, (a, b): (Value, Value)| {
            let scaled = scale_mul(&a, &b)?;
            Ok(SaVec3(scaled))
        });
    }
}

/// `glam::Vec3::normalize` returns NaN for a zero vector. A zero input stays zero
/// rather than poisoning downstream math with NaN.
fn normalized(v: Vec3) -> Vec3 {
    let len = v.length();
    if len > 0.0 { v / len } else { Vec3::ZERO }
}

/// Dispatches `vec * scalar` / `scalar * vec` on which operand is the userdata.
/// Either operand order is accepted, and the scalar may be a Luau integer or float
/// (`2 * v` arrives as an integer literal), so both are coerced through [`as_scalar`].
fn scale_mul(a: &Value, b: &Value) -> mlua::Result<Vec3> {
    if let (Some(ud), Some(s)) = (a.as_userdata(), as_scalar(b)) {
        let v = ud.borrow::<SaVec3>()?;
        return Ok(v.0 * s);
    }
    if let (Some(s), Some(ud)) = (as_scalar(a), b.as_userdata()) {
        let v = ud.borrow::<SaVec3>()?;
        return Ok(v.0 * s);
    }
    Err(mlua::Error::runtime(
        "Vec3 multiply expects a Vec3 and a number",
    ))
}

/// Reads a Luau number operand as `f32`, accepting both the integer and the float
/// representation (a literal like `2` is a Luau integer).
fn as_scalar(value: &Value) -> Option<f32> {
    match value {
        Value::Integer(i) => Some(*i as f32),
        Value::Number(n) => Some(*n as f32),
        _ => None,
    }
}

/// The free `sa.vec3(x, y, z)` constructor — also bound as the static `Vec3.new`.
#[must_use]
pub fn vec3(x: f32, y: f32, z: f32) -> SaVec3 {
    SaVec3(Vec3::new(x, y, z))
}

/// `sa.lerp(a, b, t)`: the linear interpolation `glam` computes (`Vec3::lerp`).
#[must_use]
pub fn lerp(a: SaVec3, b: SaVec3, t: f32) -> SaVec3 {
    SaVec3(a.0.lerp(b.0, t))
}

/// `sa.look_at(eye, target, up)`: a look rotation as engine ZYX-Euler radians so it
/// feeds `set_rotation`. Faces `target` from `eye` with `up` the reference;
/// degenerate (`eye == target`) returns zero.
#[must_use]
pub fn look_at(eye: SaVec3, target: SaVec3, up: SaVec3) -> SaVec3 {
    let dir = target.0 - eye.0;
    if dir.length() < 1e-6 {
        return SaVec3(Vec3::ZERO);
    }
    SaVec3(quat_to_euler_zyx(quat_look_at(dir.normalize(), up.0)))
}

/// A quaternion looking in `direction` with the given `up`, right-handed: the forward
/// maps to `-Z`. Kept private here because the crate boundary forbids reaching into
/// `saffron-sceneedit`, which holds the only other copy.
fn quat_look_at(direction: Vec3, up: Vec3) -> glam::Quat {
    let z = -direction;
    let x = up.cross(z).normalize();
    let y = z.cross(x);
    glam::Quat::from_mat3(&glam::Mat3::from_cols(x, y, z))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn look_at_is_degenerate_when_eye_equals_target() {
        let p = vec3(1.0, 2.0, 3.0);
        assert_eq!(look_at(p, p, vec3(0.0, 1.0, 0.0)), SaVec3(Vec3::ZERO));
    }

    #[test]
    fn look_at_matches_quat_to_euler_of_look_quat() {
        let eye = vec3(0.0, 0.0, 5.0);
        let target = vec3(1.0, 0.0, 0.0);
        let up = vec3(0.0, 1.0, 0.0);
        let dir = (target.0 - eye.0).normalize();
        let expected = quat_to_euler_zyx(quat_look_at(dir, up.0));
        assert_eq!(look_at(eye, target, up).0, expected);
    }

    #[test]
    fn normalized_of_zero_stays_zero() {
        assert_eq!(normalized(Vec3::ZERO), Vec3::ZERO);
    }
}
