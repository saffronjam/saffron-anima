//! The pure-value CPU types every downstream crate builds on: the mesh vocabulary,
//! the picking ray, the animation track/clip types, and the import-graph aggregates.
//!
//! The format-bearing structs (`Vertex`, `Submesh`, `VertexSkin`) are `#[repr(C)]`
//! Pod with byte strides pinned by [`super::tests`]. All format fields use glam's
//! 12-byte `Vec3` (never the 16-byte SIMD `Vec3A`), so the strides match the C++
//! `static_assert`s exactly.

use bytemuck::{Pod, Zeroable};
use glam::{Mat4, Quat, Vec2, Vec3, Vec4};

use crate::error::{Error, Result};

/// One interleaved vertex stream entry: position, normal, and the first UV set.
///
/// Exactly 32 bytes — the `.smesh` on-disk vertex stride and the GPU vertex buffer
/// layout. Tangents are deferred to material time, so they are not stored here.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Pod, Zeroable)]
pub struct Vertex {
    /// Object-space position.
    pub position: Vec3,
    /// Object-space normal.
    pub normal: Vec3,
    /// First UV set.
    pub uv0: Vec2,
}

/// One `drawIndexed` range over the shared vertex+index buffers.
///
/// Exactly 16 bytes (baked directly into a `.smesh`). `vertex_offset` is signed to
/// match `vkCmdDrawIndexed`; `material_slot` indexes the model's material table.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Pod, Zeroable)]
pub struct Submesh {
    /// First index of this range in the shared index buffer.
    pub first_index: u32,
    /// Number of indices in this range.
    pub index_count: u32,
    /// Value added to each index before vertex fetch (signed, per Vulkan).
    pub vertex_offset: i32,
    /// Index into the model's material table.
    pub material_slot: u32,
}

/// Per-vertex skin influences: a second stream parallel to [`Mesh::vertices`].
///
/// Exactly 24 bytes (the `.smesh` v2 skin stride and the GPU skin-stream stride).
/// Kept out of [`Vertex`] so the unskinned layout and the v1 `.smesh` stay intact;
/// an empty skin stream means the mesh is unskinned.
///
/// `weights` is a raw `[f32; 4]` rather than glam's `Vec4`: glam's `Vec4` is
/// 16-byte SIMD-aligned, which would pad this struct to 32 bytes and break the
/// stride — the same reason format-bearing 3-vectors use `Vec3`, not `Vec3A`.
/// The 24-byte stride is the load-bearing invariant; the array is its expression.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Pod, Zeroable)]
pub struct VertexSkin {
    /// Indices into the skin's joint list.
    pub joints: [u16; 4],
    /// Normalized blend weights, one per joint.
    pub weights: [f32; 4],
}

/// The canonical CPU-side mesh every importer converts into.
///
/// Not `#[repr(C)]`: it is the in-memory aggregate the byte formats serialize *from*,
/// not a byte layout itself.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Mesh {
    /// The interleaved vertex stream.
    pub vertices: Vec<Vertex>,
    /// 32-bit triangle indices into [`Mesh::vertices`].
    pub indices: Vec<u32>,
    /// The draw ranges, each over a slice of the shared buffers.
    pub submeshes: Vec<Submesh>,
}

/// A world-space ray for picking and spatial queries.
///
/// `dir` is assumed unit-length; the caller normalizes before constructing.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Ray {
    /// Ray origin.
    pub origin: Vec3,
    /// Ray direction, caller-normalized to unit length.
    pub dir: Vec3,
}

impl Default for Ray {
    fn default() -> Self {
        Self {
            origin: Vec3::ZERO,
            dir: Vec3::new(0.0, 0.0, -1.0),
        }
    }
}

/// Vertex/index totals read from a `.smesh` header without loading the data.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct MeshCounts {
    /// Number of vertices in the mesh.
    pub vertex_count: u32,
    /// Number of indices in the mesh.
    pub index_count: u32,
}

/// The channel an [`AnimTrack`] targets on its joint.
///
/// `#[repr(u8)]`: the discriminants are the pinned on-disk byte values
/// (`Translation = 0`, `Rotation = 1`, `Scale = 2`).
#[repr(u8)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum AnimPath {
    /// The joint's translation.
    #[default]
    Translation = 0,
    /// The joint's rotation.
    Rotation = 1,
    /// The joint's scale.
    Scale = 2,
}

impl AnimPath {
    /// Maps an on-disk discriminant byte back to the enum.
    ///
    /// An out-of-range byte is rejected with [`Error::BadLayout`] rather than
    /// transmuted, so a malformed `.sanim` track record can never produce UB
    /// (the crate's `#![deny(unsafe_code)]` holds).
    pub fn from_u8(byte: u8) -> Result<Self> {
        match byte {
            0 => Ok(Self::Translation),
            1 => Ok(Self::Rotation),
            2 => Ok(Self::Scale),
            _ => Err(Error::BadLayout),
        }
    }
}

/// The interpolation a sampler applies between keyframes.
///
/// `#[repr(u8)]`: the discriminants are the pinned on-disk byte values
/// (`Step = 0`, `Linear = 1`, `CubicSpline = 2`).
#[repr(u8)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum AnimInterp {
    /// Hold the previous keyframe value (no interpolation).
    Step = 0,
    /// Linear interpolation between keyframes.
    #[default]
    Linear = 1,
    /// Cubic-spline interpolation; values store 3x (in-tangent, value, out-tangent).
    CubicSpline = 2,
}

impl AnimInterp {
    /// Maps an on-disk discriminant byte back to the enum.
    ///
    /// An out-of-range byte is rejected with [`Error::BadLayout`] rather than
    /// transmuted, so a malformed `.sanim` track record can never produce UB
    /// (the crate's `#![deny(unsafe_code)]` holds).
    pub fn from_u8(byte: u8) -> Result<Self> {
        match byte {
            0 => Ok(Self::Step),
            1 => Ok(Self::Linear),
            2 => Ok(Self::CubicSpline),
            _ => Err(Error::BadLayout),
        }
    }
}

/// One animated joint channel: a sampled curve targeting a joint's translation,
/// rotation, or scale.
///
/// A faithful, lossless mirror of a glTF animation channel + sampler, bound to a
/// joint by a stable index plus the durable node name.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct AnimTrack {
    /// Stable index into the skinned mesh's bones (resolved at import by name);
    /// `-1` until resolved.
    pub joint: i32,
    /// The glTF node name — the durable binding key that survives reorder/reimport.
    pub joint_name: String,
    /// The targeted channel.
    pub path: AnimPath,
    /// The sampler's interpolation.
    pub interp: AnimInterp,
    /// `sampler.input` — strictly increasing keyframe times, in seconds.
    pub times: Vec<f32>,
    /// `sampler.output`, flat: a `Vec3` per key for T/S, a quaternion `xyzw` per key
    /// for R; `CubicSpline` stores 3x (in-tangent, value, out-tangent) per key.
    pub values: Vec<f32>,
}

/// A named animation clip: a bundle of joint tracks with a total duration.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct AnimClip {
    /// The clip name.
    pub name: String,
    /// Total duration in seconds (the max track end time).
    pub duration: f32,
    /// The joint tracks that make up the clip.
    pub tracks: Vec<AnimTrack>,
}

/// One node of the imported scene graph: name, parent index, and the local TRS.
///
/// `rotation` is the source quaternion in glam's `xyzw` order (the glTF storage
/// order — no swizzle); consumers convert to their own Euler convention.
#[derive(Clone, Debug, PartialEq)]
pub struct ImportedNode {
    /// The glTF node name.
    pub name: String,
    /// Parent node index, or `-1` for a root node.
    pub parent: i32,
    /// Local translation.
    pub translation: Vec3,
    /// Local rotation (glam `xyzw`, the glTF storage order).
    pub rotation: Quat,
    /// Local scale.
    pub scale: Vec3,
}

impl Default for ImportedNode {
    fn default() -> Self {
        Self {
            name: String::new(),
            parent: -1,
            translation: Vec3::ZERO,
            rotation: Quat::IDENTITY,
            scale: Vec3::ONE,
        }
    }
}

/// One glTF skin: the ordered joint node indices (the `jointMatrices[]` order) and
/// the parallel inverse bind matrices.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct ImportedSkin {
    /// Joint node indices, in `jointMatrices[]` order.
    pub joints: Vec<i32>,
    /// The inverse bind matrix per joint, parallel to [`ImportedSkin::joints`].
    pub inverse_bind: Vec<Mat4>,
    /// The skin's declared root node index, or `-1` if unspecified.
    pub skeleton_root: i32,
    /// The node carrying the skinned mesh, or `-1` if unspecified.
    pub mesh_node: i32,
}

/// One imported material texture: the encoded (png/jpg) bytes plus their extension.
///
/// Replaces the C++ `has*` bool + parallel byte/ext blob pair — an
/// `Option<TextureSource>` makes "is the texture present?" and the payload one field,
/// so the flag can never disagree with the bytes.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct TextureSource {
    /// The encoded image bytes (png/jpg), as read from an external file or embedded.
    pub bytes: Vec<u8>,
    /// The source extension, e.g. `"png"` / `"jpg"`.
    pub ext: String,
}

/// One material extracted from a model: the PBR factors and any optional textures.
///
/// Each optional texture is an `Option<TextureSource>` rather than the C++ `has*`
/// bool + parallel fields (NO LEGACY: the bool/blob pairs do not survive).
#[derive(Clone, Debug, PartialEq)]
pub struct ImportedMaterial {
    /// The source material name (the stable key for its baked sub-id).
    pub name: String,
    /// Base color factor (RGBA).
    pub base_color: Vec4,
    /// Metallic factor.
    pub metallic: f32,
    /// Roughness factor.
    pub roughness: f32,
    /// Emissive factor (RGB).
    pub emissive: Vec3,
    /// Emissive strength multiplier.
    pub emissive_strength: f32,
    /// The base-color (albedo) texture, if any (sRGB color).
    pub albedo: Option<TextureSource>,
    /// The glTF metallic-roughness texture, if any (roughness in G, metalness in B; linear).
    pub metallic_roughness: Option<TextureSource>,
    /// The tangent-space normal map, if any (linear).
    pub normal: Option<TextureSource>,
    /// The ambient-occlusion texture, if any (AO in R; linear).
    pub occlusion: Option<TextureSource>,
    /// The emissive texture, if any (sRGB).
    pub emissive_tex: Option<TextureSource>,
}

impl Default for ImportedMaterial {
    fn default() -> Self {
        Self {
            name: String::new(),
            base_color: Vec4::ONE,
            metallic: 0.0,
            roughness: 1.0,
            emissive: Vec3::ZERO,
            emissive_strength: 1.0,
            albedo: None,
            metallic_roughness: None,
            normal: None,
            occlusion: None,
            emissive_tex: None,
        }
    }
}

/// The skin payload of a skinned model.
///
/// Collapses the C++ `hasSkin` bool that gated three separate vectors into one
/// `Option<SkinPayload>` on [`ImportedModel`]: present means skinned, and the four
/// skin-only fields travel together (NO LEGACY: no bool that can disagree).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct SkinPayload {
    /// Per-vertex skin influences, parallel to [`Mesh::vertices`].
    pub stream: Vec<VertexSkin>,
    /// The source node forest.
    pub nodes: Vec<ImportedNode>,
    /// The skin descriptor; `desc.joints` indexes into [`SkinPayload::nodes`] in
    /// glTF joint order — the single source of `jointMatrices` order.
    pub desc: ImportedSkin,
    /// The skeletal clips decoded from the glTF animations.
    pub animations: Vec<AnimClip>,
}

/// The in-memory import graph a source model (`.gltf`/`.glb`/`.obj`) translates into.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct ImportedModel {
    /// The merged CPU mesh.
    pub mesh: Mesh,
    /// The material table; each [`Submesh::material_slot`] indexes it. Always at
    /// least one entry (a default material when the source declares none).
    pub materials: Vec<ImportedMaterial>,
    /// The skin payload (glTF only); `None` for an unskinned model.
    pub skin: Option<SkinPayload>,
}

/// Decoded RGBA8 pixels, tightly packed (`width * height * 4` bytes).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct DecodedImage {
    /// The tightly packed RGBA8 pixel bytes.
    pub rgba: Vec<u8>,
    /// Image width in pixels.
    pub width: u32,
    /// Image height in pixels.
    pub height: u32,
}

/// Decoded linear-float RGBA, tightly packed (`width * height * 4` floats).
///
/// From `.hdr`/`.exr`-class sources; values are real radiance (may exceed `1.0`),
/// never sRGB-encoded.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct DecodedImageFloat {
    /// The tightly packed linear-float RGBA values.
    pub rgba: Vec<f32>,
    /// Image width in pixels.
    pub width: u32,
    /// Image height in pixels.
    pub height: u32,
}

/// A material texture slot's semantic role.
///
/// The import-options colorspace policy keys on it (albedo/emissive → sRGB color;
/// the rest → linear data), so one source of truth decides how a baked or scanned
/// texture is interpreted.
#[repr(u8)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum MaterialMapRole {
    /// Base color (sRGB).
    #[default]
    Albedo,
    /// Metallic-roughness (linear).
    MetallicRoughness,
    /// Tangent-space normal map (linear).
    Normal,
    /// Ambient occlusion (linear).
    Occlusion,
    /// Emissive (sRGB).
    Emissive,
    /// Height/displacement (linear).
    Height,
}
