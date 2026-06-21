//! std430 byte-layout golden snapshots: the std430 half of the golden gate, paired with
//! the `const _: () = assert!(size_of == N)` and the `offset_of!` unit tests in
//! `gpu_types.rs`.
//!
//! A size `static_assert` alone misses a *size-equal field swap* that moves an offset —
//! which silently mis-deduplicates `MaterialParamsData` (hashed by raw bytes for per-frame
//! dedup, 06-rendering README §3) without changing the struct's size. The detector is a
//! golden *offset map*: a hexdump of a known-valued instance plus a field→offset table
//! (`fixtures/golden/gen/`). A field reorder changes the hexdump byte order; a stride
//! change changes the size line.
//!
//! Each test rebuilds the same known-valued instance, renders the same
//! `struct ... / offset ... / hexdump:` text, and matches it byte-for-byte. Reseed
//! with `UPDATE_GOLDEN=1` only on an intentional layout change.

use saffron_geometry::glam::{Mat4, UVec4, Vec4};
use saffron_rendering::{GpuLight, InstanceData, MaterialParamsData};
use saffron_test_support::assert_bytes_match_golden;

/// Formats raw bytes 16 per row, two-hex-digits + trailing space (a trailing space per
/// byte and a trailing newline).
fn hexdump(bytes: &[u8]) -> String {
    let mut out = String::new();
    for (i, byte) in bytes.iter().enumerate() {
        if i != 0 && i % 16 == 0 {
            out.push('\n');
        }
        out.push_str(&format!("{byte:02x} "));
    }
    out.push('\n');
    out
}

/// Builds the `struct <name> size=<n> align=16` / `offset <field> <n>` / `hexdump:` map,
/// ending with the known-valued instance's hexdump.
fn offset_map(header: &str, offsets: &[(&str, usize)], bytes: &[u8]) -> String {
    let mut out = header.to_owned();
    out.push('\n');
    for (field, offset) in offsets {
        out.push_str(&format!("offset {field} {offset}\n"));
    }
    out.push_str("hexdump:\n");
    out.push_str(&hexdump(bytes));
    out
}

#[test]
fn material_params_data_offset_map_matches_cpp_golden() {
    let data = MaterialParamsData {
        base_color: Vec4::new(0.8, 0.4, 0.2, 1.0),
        pbr: Vec4::new(0.25, 0.7, 1.0, 0.5),
        emissive: Vec4::new(0.1, 0.0, 0.0, 0.05),
        uv: Vec4::new(2.0, 2.0, 0.0, 0.0),
        tex0: UVec4::new(3, 0, 5, 0),
        tex1: UVec4::new(0, 0, 0, 7),
    };
    let map = offset_map(
        "struct MaterialParamsData size=96 align=16",
        &[
            ("baseColor", 0),
            ("pbr", 16),
            ("emissive", 32),
            ("uv", 48),
            ("tex0", 64),
            ("tex1", 80),
        ],
        bytemuck::bytes_of(&data),
    );
    assert_bytes_match_golden("material_params_data.offsets", map.as_bytes());
}

#[test]
fn instance_data_offset_map_matches_cpp_golden() {
    let data = InstanceData {
        model: Mat4::from_cols_array(&[
            1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 2.0, 3.0, 4.0, 1.0,
        ]),
        normal_matrix: Mat4::IDENTITY,
        prev_model: Mat4::from_cols_array(&[
            1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 1.0, 1.0, 1.0, 1.0,
        ]),
        base_color: Vec4::new(0.8, 0.4, 0.2, 1.0),
        texture: UVec4::new(3, 7, 11, 13),
        pbr: Vec4::new(0.25, 0.7, 0.0, 0.0),
        emissive: Vec4::new(0.1, 0.0, 0.0, 0.0),
    };
    let map = offset_map(
        "struct InstanceData size=256 align=16",
        &[
            ("model", 0),
            ("normalMatrix", 64),
            ("prevModel", 128),
            ("baseColor", 192),
            ("texture", 208),
            ("pbr", 224),
            ("emissive", 240),
        ],
        bytemuck::bytes_of(&data),
    );
    assert_bytes_match_golden("instance_data.offsets", map.as_bytes());
}

#[test]
fn gpu_light_offset_map_matches_cpp_golden() {
    let data = GpuLight {
        position_range: Vec4::new(1.0, 2.0, 3.0, 10.0),
        color_intensity: Vec4::new(1.0, 0.5, 0.25, 4.0),
        direction_type: Vec4::new(0.0, -1.0, 0.0, 1.0),
        spot_cos: Vec4::new(0.9, 0.7, 0.0, 0.0),
    };
    let map = offset_map(
        "struct GpuLight size=64 align=16",
        &[
            ("positionRange", 0),
            ("colorIntensity", 16),
            ("directionType", 32),
            ("spotCos", 48),
        ],
        bytemuck::bytes_of(&data),
    );
    assert_bytes_match_golden("gpu_light.offsets", map.as_bytes());
}
