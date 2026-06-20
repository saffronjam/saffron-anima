//! The `.smesh` (`SMSH`) byte format: a 64-byte header followed by three contiguous
//! sections (vertices, indices, submeshes) with an optional v2 skin section appended.
//!
//! The image is the canonical triple contract: disk bytes == in-memory payload == the
//! GPU vertex buffer. It is byte-for-byte identical to the C++ image so a `.smodel`
//! MESH chunk slice and a standalone `.smesh` file read the same. The bytes are
//! reinterpreted with **safe** `bytemuck` over `#[repr(C)]` Pod structs (`bytes_of` /
//! `cast_slice` to write, `from_bytes` / `cast_slice` to read), so the crate's
//! `#![deny(unsafe_code)]` holds while the bytes stay identical.
//!
//! Two versions live in the format, exactly as the live source has them:
//! [`MESH_FORMAT_VERSION`] = 1 (unskinned) and [`MESH_FORMAT_VERSION_SKINNED`] = 2
//! (the same header and first three sections plus a `VertexSkin` section). The encoder
//! picks the version by whether the skin is non-empty; the loader accepts 1 and 2 and
//! rejects any other.

use std::fs;
use std::path::Path;

use bytemuck::{Pod, Zeroable};

use crate::error::{Error, Result};
use crate::types::{Mesh, MeshCounts, Submesh, Vertex, VertexSkin};

/// The unskinned `.smesh`: a three-section layout (vertices, indices, submeshes).
pub const MESH_FORMAT_VERSION: u32 = 1;
/// The skinned `.smesh`: the same header and first three sections, plus a
/// `VertexSkin` section appended after the submeshes.
pub const MESH_FORMAT_VERSION_SKINNED: u32 = 2;

/// The four-byte tag at the head of every `.smesh` image.
const MAGIC: [u8; 4] = *b"SMSH";

/// The 64-byte fixed header; the three contiguous raw arrays follow at the offsets.
///
/// `#[repr(C)]` Pod with the exact field order/widths of the C++ `SMeshHeader`. The
/// offsets are self-relative (from the start of the image), so an embedded `.smodel`
/// chunk slice reads identically to a standalone file.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Pod, Zeroable)]
struct SMeshHeader {
    /// `b"SMSH"`.
    magic: [u8; 4],
    /// Format version: 1 (unskinned) or 2 (skinned).
    version: u32,
    /// Reserved, always 0.
    flags: u32,
    /// Bytes per vertex; must equal `size_of::<Vertex>()` (32).
    vertex_stride: u32,
    /// Number of vertices.
    vertex_count: u32,
    /// Number of indices.
    index_count: u32,
    /// Bytes per index; must equal 4.
    index_width: u32,
    /// Number of submeshes.
    submesh_count: u32,
    /// Offset to the vertex section; equals `size_of::<SMeshHeader>()` (64).
    vertices_offset: u64,
    /// Offset to the index section.
    indices_offset: u64,
    /// Offset to the submesh section.
    submeshes_offset: u64,
    /// Reserved, always 0.
    reserved: [u32; 2],
}

const _: () = assert!(
    size_of::<SMeshHeader>() == 64,
    "SMeshHeader must be exactly 64 bytes"
);

/// Builds the `.smesh` byte image for `mesh`, appending the v2 skin section when
/// `skin` is non-empty (an empty skin yields a v1 image).
fn encode_mesh_image(mesh: &Mesh, skin: &[VertexSkin]) -> Vec<u8> {
    let vertex_count = mesh.vertices.len() as u32;
    let index_count = mesh.indices.len() as u32;
    let submesh_count = mesh.submeshes.len() as u32;

    let vertices_offset = size_of::<SMeshHeader>() as u64;
    let indices_offset = vertices_offset + u64::from(vertex_count) * size_of::<Vertex>() as u64;
    let submeshes_offset = indices_offset + u64::from(index_count) * size_of::<u32>() as u64;
    let submeshes_end = submeshes_offset + u64::from(submesh_count) * size_of::<Submesh>() as u64;

    let version = if skin.is_empty() {
        MESH_FORMAT_VERSION
    } else {
        MESH_FORMAT_VERSION_SKINNED
    };

    let header = SMeshHeader {
        magic: MAGIC,
        version,
        flags: 0,
        vertex_stride: size_of::<Vertex>() as u32,
        vertex_count,
        index_count,
        index_width: size_of::<u32>() as u32,
        submesh_count,
        vertices_offset,
        indices_offset,
        submeshes_offset,
        reserved: [0, 0],
    };

    let mut bytes = Vec::with_capacity(submeshes_end as usize + size_of_val(skin));
    bytes.extend_from_slice(bytemuck::bytes_of(&header));
    bytes.extend_from_slice(bytemuck::cast_slice(&mesh.vertices));
    bytes.extend_from_slice(bytemuck::cast_slice(&mesh.indices));
    bytes.extend_from_slice(bytemuck::cast_slice(&mesh.submeshes));
    if !skin.is_empty() {
        bytes.extend_from_slice(bytemuck::cast_slice(skin));
    }
    bytes
}

/// Encodes `mesh` to an unskinned (v1) `.smesh` byte image.
pub fn save_mesh_to_buffer(mesh: &Mesh) -> Vec<u8> {
    encode_mesh_image(mesh, &[])
}

/// Encodes `mesh` plus its parallel `skin` stream to a skinned (v2) `.smesh` byte
/// image.
///
/// The skin stream must parallel the vertices one-for-one; a length mismatch is an
/// [`Error::SkinLengthMismatch`].
pub fn save_mesh_skinned_to_buffer(mesh: &Mesh, skin: &[VertexSkin]) -> Result<Vec<u8>> {
    if skin.len() != mesh.vertices.len() {
        return Err(Error::SkinLengthMismatch {
            skin: skin.len(),
            vertices: mesh.vertices.len(),
        });
    }
    Ok(encode_mesh_image(mesh, skin))
}

/// Reads and validates a `.smesh` header from the front of `bytes`.
fn read_header(bytes: &[u8]) -> Result<SMeshHeader> {
    let head = bytes
        .get(..size_of::<SMeshHeader>())
        .ok_or(Error::Truncated)?;
    let header: &SMeshHeader = bytemuck::from_bytes(head);
    if header.magic != MAGIC {
        return Err(Error::BadMagic);
    }
    Ok(*header)
}

/// Decodes the vertices, indices, and submeshes of a `.smesh` image.
///
/// Validates the magic, the version (1 or 2), the vertex/index strides, then
/// recomputes the section layout from the counts and requires the header's stored
/// offsets to match and the span to be long enough. The span length is the chunk
/// length, not a file size, so an embedded `.smodel` chunk reads identically to a
/// standalone file.
pub fn load_mesh_from_bytes(bytes: &[u8]) -> Result<Mesh> {
    let header = read_header(bytes)?;
    if header.version != MESH_FORMAT_VERSION && header.version != MESH_FORMAT_VERSION_SKINNED {
        return Err(Error::UnsupportedVersion(header.version));
    }
    if header.vertex_stride != size_of::<Vertex>() as u32
        || header.index_width != size_of::<u32>() as u32
    {
        return Err(Error::BadLayout);
    }

    let vertices_end = size_of::<SMeshHeader>() as u64
        + u64::from(header.vertex_count) * size_of::<Vertex>() as u64;
    let indices_end = vertices_end + u64::from(header.index_count) * size_of::<u32>() as u64;
    let submeshes_end = indices_end + u64::from(header.submesh_count) * size_of::<Submesh>() as u64;
    if header.vertices_offset != size_of::<SMeshHeader>() as u64
        || header.indices_offset != vertices_end
        || header.submeshes_offset != indices_end
        || (bytes.len() as u64) < submeshes_end
    {
        return Err(Error::BadLayout);
    }

    let vertices: &[Vertex] =
        bytemuck::cast_slice(&bytes[header.vertices_offset as usize..vertices_end as usize]);
    let indices: &[u32] =
        bytemuck::cast_slice(&bytes[header.indices_offset as usize..indices_end as usize]);
    let submeshes: &[Submesh] =
        bytemuck::cast_slice(&bytes[header.submeshes_offset as usize..submeshes_end as usize]);

    Ok(Mesh {
        vertices: vertices.to_vec(),
        indices: indices.to_vec(),
        submeshes: submeshes.to_vec(),
    })
}

/// Decodes the v2 skin section of a `.smesh` image.
///
/// A v1 image carries no skin, so this returns an empty stream (not an error). A v2
/// image with a truncated skin section is an [`Error::Truncated`].
pub fn load_mesh_skin_from_bytes(bytes: &[u8]) -> Result<Vec<VertexSkin>> {
    let header = read_header(bytes)?;
    if header.version != MESH_FORMAT_VERSION_SKINNED {
        return Ok(Vec::new());
    }
    let submeshes_end =
        header.submeshes_offset + u64::from(header.submesh_count) * size_of::<Submesh>() as u64;
    let skin_end = submeshes_end + u64::from(header.vertex_count) * size_of::<VertexSkin>() as u64;
    if (bytes.len() as u64) < skin_end {
        return Err(Error::Truncated);
    }
    let skin: &[VertexSkin] =
        bytemuck::cast_slice(&bytes[submeshes_end as usize..skin_end as usize]);
    Ok(skin.to_vec())
}

/// Reads the vertex/index totals from a `.smesh` header without loading the data.
pub fn mesh_counts_from_bytes(bytes: &[u8]) -> Result<MeshCounts> {
    let header = read_header(bytes)?;
    Ok(MeshCounts {
        vertex_count: header.vertex_count,
        index_count: header.index_count,
    })
}

/// Reads a `.smesh` file and decodes its mesh.
pub fn load_mesh(path: impl AsRef<Path>) -> Result<Mesh> {
    let path = path.as_ref();
    let bytes = fs::read(path).map_err(|e| Error::Io(format!("'{}': {e}", path.display())))?;
    load_mesh_from_bytes(&bytes)
}

/// Reads a `.smesh` file and decodes its v2 skin stream (empty for a v1 file).
pub fn load_mesh_skin(path: impl AsRef<Path>) -> Result<Vec<VertexSkin>> {
    let path = path.as_ref();
    let bytes = fs::read(path).map_err(|e| Error::Io(format!("'{}': {e}", path.display())))?;
    load_mesh_skin_from_bytes(&bytes)
}

/// Reads the vertex/index totals from a `.smesh` file's header without loading the
/// data.
pub fn mesh_file_counts(path: impl AsRef<Path>) -> Result<MeshCounts> {
    let path = path.as_ref();
    let bytes = fs::read(path).map_err(|e| Error::Io(format!("'{}': {e}", path.display())))?;
    mesh_counts_from_bytes(&bytes)
}

/// Encodes `mesh` plus its parallel `skin` (v2) and writes the image to `path`.
pub fn save_mesh_skinned(mesh: &Mesh, skin: &[VertexSkin], path: impl AsRef<Path>) -> Result<()> {
    let bytes = save_mesh_skinned_to_buffer(mesh, skin)?;
    let path = path.as_ref();
    fs::write(path, &bytes).map_err(|e| Error::Io(format!("'{}': {e}", path.display())))
}

#[cfg(test)]
mod tests {
    use super::*;
    use glam::{Vec2, Vec3};

    fn sample_mesh() -> Mesh {
        Mesh {
            vertices: vec![
                Vertex {
                    position: Vec3::new(1.0, 2.0, 3.0),
                    normal: Vec3::Y,
                    uv0: Vec2::new(0.0, 0.0),
                },
                Vertex {
                    position: Vec3::new(-1.0, 0.5, 4.0),
                    normal: Vec3::X,
                    uv0: Vec2::new(0.25, 0.75),
                },
                Vertex {
                    position: Vec3::new(2.0, -3.0, 0.0),
                    normal: Vec3::Z,
                    uv0: Vec2::new(1.0, 1.0),
                },
            ],
            indices: vec![0, 1, 2],
            submeshes: vec![Submesh {
                first_index: 0,
                index_count: 3,
                vertex_offset: 0,
                material_slot: 0,
            }],
        }
    }

    fn sample_skin() -> Vec<VertexSkin> {
        vec![
            VertexSkin {
                joints: [0, 1, 2, 3],
                weights: [0.5, 0.25, 0.15, 0.10],
            },
            VertexSkin {
                joints: [4, 0, 0, 0],
                weights: [1.0, 0.0, 0.0, 0.0],
            },
            VertexSkin {
                joints: [2, 3, 0, 0],
                weights: [0.6, 0.4, 0.0, 0.0],
            },
        ]
    }

    #[test]
    fn unskinned_round_trip() {
        let mesh = sample_mesh();
        let baked = save_mesh_to_buffer(&mesh);
        let loaded = load_mesh_from_bytes(&baked).unwrap();
        assert_eq!(loaded.vertices.len(), mesh.vertices.len());
        assert_eq!(loaded.indices.len(), mesh.indices.len());
        assert_eq!(loaded.submeshes.len(), mesh.submeshes.len());
        assert_eq!(loaded.vertices[0].position, mesh.vertices[0].position);
        assert_eq!(loaded, mesh);
    }

    #[test]
    fn skinned_round_trip() {
        let mesh = sample_mesh();
        let skin = sample_skin();
        let baked = save_mesh_skinned_to_buffer(&mesh, &skin).unwrap();
        let loaded = load_mesh_from_bytes(&baked).unwrap();
        let loaded_skin = load_mesh_skin_from_bytes(&baked).unwrap();
        assert_eq!(loaded, mesh);
        assert_eq!(loaded_skin, skin);
    }

    #[test]
    fn v1_image_yields_empty_skin() {
        let mesh = sample_mesh();
        let baked = save_mesh_to_buffer(&mesh);
        let skin = load_mesh_skin_from_bytes(&baked).unwrap();
        assert!(skin.is_empty());
    }

    #[test]
    fn counts_read_from_header_only() {
        let mesh = sample_mesh();
        let baked = save_mesh_to_buffer(&mesh);
        let counts = mesh_counts_from_bytes(&baked).unwrap();
        assert_eq!(counts.vertex_count, 3);
        assert_eq!(counts.index_count, 3);
    }

    #[test]
    fn golden_bytes_header_is_frozen() {
        // The byte image is the frozen contract; this asserts the exact header
        // fields, the version-by-skin choice, and the three section offsets.
        let mesh = sample_mesh();
        let baked = save_mesh_to_buffer(&mesh);

        // 64-byte header + 3*32 vertices + 3*4 indices + 1*16 submesh = 64+96+12+16.
        assert_eq!(baked.len(), 64 + 3 * 32 + 3 * 4 + 16);

        let header: &SMeshHeader = bytemuck::from_bytes(&baked[..64]);
        assert_eq!(&header.magic, b"SMSH");
        assert_eq!(header.version, 1);
        assert_eq!(header.flags, 0);
        assert_eq!(header.vertex_stride, 32);
        assert_eq!(header.index_width, 4);
        assert_eq!(header.vertex_count, 3);
        assert_eq!(header.index_count, 3);
        assert_eq!(header.submesh_count, 1);
        assert_eq!(header.vertices_offset, 64);
        assert_eq!(header.indices_offset, 64 + 3 * 32);
        assert_eq!(header.submeshes_offset, 64 + 3 * 32 + 3 * 4);
        assert_eq!(header.reserved, [0, 0]);

        // A skinned image is v2 and one VertexSkin stride longer per vertex.
        let skin = sample_skin();
        let baked_v2 = save_mesh_skinned_to_buffer(&mesh, &skin).unwrap();
        let header_v2: &SMeshHeader = bytemuck::from_bytes(&baked_v2[..64]);
        assert_eq!(header_v2.version, 2);
        assert_eq!(baked_v2.len(), baked.len() + 3 * 24);
    }

    #[test]
    fn bad_magic_is_rejected() {
        let mesh = sample_mesh();
        let mut baked = save_mesh_to_buffer(&mesh);
        baked[0] = b'X';
        assert!(matches!(load_mesh_from_bytes(&baked), Err(Error::BadMagic)));
    }

    #[test]
    fn unknown_version_is_rejected() {
        let mesh = sample_mesh();
        let mut baked = save_mesh_to_buffer(&mesh);
        // Overwrite the version field (bytes 4..8) with 3.
        baked[4..8].copy_from_slice(&3u32.to_le_bytes());
        assert!(matches!(
            load_mesh_from_bytes(&baked),
            Err(Error::UnsupportedVersion(3))
        ));
    }

    #[test]
    fn truncated_header_is_rejected() {
        let mesh = sample_mesh();
        let baked = save_mesh_to_buffer(&mesh);
        assert!(matches!(
            load_mesh_from_bytes(&baked[..32]),
            Err(Error::Truncated)
        ));
    }

    #[test]
    fn truncated_body_is_bad_layout() {
        let mesh = sample_mesh();
        let baked = save_mesh_to_buffer(&mesh);
        // Drop the last submesh bytes: header is intact but the span is too short.
        let truncated = &baked[..baked.len() - 8];
        assert!(matches!(
            load_mesh_from_bytes(truncated),
            Err(Error::BadLayout)
        ));
    }

    #[test]
    fn truncated_skin_section_is_rejected() {
        let mesh = sample_mesh();
        let skin = sample_skin();
        let baked = save_mesh_skinned_to_buffer(&mesh, &skin).unwrap();
        let truncated = &baked[..baked.len() - 8];
        assert!(matches!(
            load_mesh_skin_from_bytes(truncated),
            Err(Error::Truncated)
        ));
    }

    #[test]
    fn mismatched_skin_length_is_rejected() {
        let mesh = sample_mesh();
        let skin = vec![VertexSkin::default()]; // 1 vs 3 vertices
        assert!(matches!(
            save_mesh_skinned_to_buffer(&mesh, &skin),
            Err(Error::SkinLengthMismatch {
                skin: 1,
                vertices: 3
            })
        ));
    }

    #[test]
    fn file_round_trip() {
        let mesh = sample_mesh();
        let skin = sample_skin();
        let dir = std::env::temp_dir();
        let path = dir.join(format!("saffron-smesh-test-{}.smesh", std::process::id()));
        save_mesh_skinned(&mesh, &skin, &path).unwrap();

        let loaded = load_mesh(&path).unwrap();
        let loaded_skin = load_mesh_skin(&path).unwrap();
        let counts = mesh_file_counts(&path).unwrap();
        assert_eq!(loaded, mesh);
        assert_eq!(loaded_skin, skin);
        assert_eq!(counts.vertex_count, 3);
        assert_eq!(counts.index_count, 3);

        let _ = fs::remove_file(&path);
    }

    #[test]
    fn missing_file_is_io_error() {
        let path = std::env::temp_dir().join("saffron-smesh-does-not-exist.smesh");
        assert!(matches!(load_mesh(&path), Err(Error::Io(_))));
    }
}
