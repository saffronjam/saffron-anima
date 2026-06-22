//! glTF (`.gltf`/`.glb`) import onto the `gltf` crate.
//!
//! The `gltf` crate exposes an index-only typed API. Three deterministic glue pieces
//! are reconstructed by hand and pinned by tests so the output stays byte-stable for
//! the asset bake hashes that depend on it:
//!
//! - **The parent map** ([`build_parents`]) — the crate's `Node` exposes `children`,
//!   not a parent, so parent indices are built by one walk over every node's children.
//! - **The world-transform walk** ([`world_transform`]) — for the unskinned path the
//!   mesh node's world transform is baked into its vertices; this composes each node's
//!   local TRS up the parent chain.
//! - **Node ordering** — joint-index and channel→joint resolution depend on the node
//!   array being in document order. The crate iterates `nodes()` in document order and
//!   exposes `node.index()`; both index the same array.

use std::path::Path;

use glam::{Affine3A, Mat3, Mat4, Quat, Vec2, Vec3, Vec4};
use gltf::animation::{Interpolation, Property};
use gltf::mesh::Mode;

use crate::error::{Error, Result};
use crate::picking::generate_normals;
use crate::types::{
    AnimClip, AnimInterp, AnimPath, AnimTrack, ImportedMaterial, ImportedModel, ImportedNode,
    ImportedSkin, Mesh, SkinPayload, Submesh, TextureSource, Vertex, VertexSkin,
};

/// Import a `.gltf`/`.glb` model into the in-memory [`ImportedModel`] graph.
///
/// Decodes the merged triangle mesh (first-seen material slots), the optional skin
/// payload (joint list, inverse-bind matrices, the source node forest, decoded
/// clips), and the materials with their texture byte blobs. The skin is imported
/// only when the first skin covers every triangle primitive; a mixed skinned/
/// unskinned model imports as plain geometry.
pub fn import_gltf_model(path: impl AsRef<Path>) -> Result<ImportedModel> {
    let path = path.as_ref();
    let gltf = gltf::Gltf::open(path)
        .map_err(|e| Error::Import(format!("gltf: cannot parse '{}': {e}", path.display())))?;
    let document = gltf.document;
    let base = path.parent();
    let buffers = gltf::import_buffers(&document, base, gltf.blob).map_err(|e| {
        Error::Import(format!(
            "gltf: cannot load buffers for '{}': {e}",
            path.display()
        ))
    })?;

    let parents = build_parents(&document);

    let mut mesh = Mesh::default();
    let mut vertex_skins: Vec<VertexSkin> = Vec::new();
    let mut saw_skinned = false;
    let mut saw_unskinned = false;
    // Distinct source materials in first-seen order, keyed by the material's document
    // index (`None` == a primitive with no material, which gets the default slot).
    let mut material_table: Vec<Option<usize>> = Vec::new();
    let mut material_slot_of = |key: Option<usize>| -> u32 {
        if let Some(pos) = material_table.iter().position(|m| *m == key) {
            return pos as u32;
        }
        let slot = material_table.len() as u32;
        material_table.push(key);
        slot
    };

    let skins_count = document.skins().count();

    if skins_count == 0 {
        let mut saw_mesh_node = false;
        for node in document.nodes() {
            let Some(node_mesh) = node.mesh() else {
                continue;
            };
            saw_mesh_node = true;
            let node_transform = world_transform(&document, &parents, node.index());
            for prim in node_mesh.primitives() {
                append_primitive(
                    &prim,
                    &buffers,
                    Some(&node_transform),
                    &mut mesh,
                    &mut vertex_skins,
                    &mut saw_skinned,
                    &mut saw_unskinned,
                    &mut material_slot_of,
                    path,
                )?;
            }
        }
        if !saw_mesh_node {
            for gltf_mesh in document.meshes() {
                for prim in gltf_mesh.primitives() {
                    append_primitive(
                        &prim,
                        &buffers,
                        None,
                        &mut mesh,
                        &mut vertex_skins,
                        &mut saw_skinned,
                        &mut saw_unskinned,
                        &mut material_slot_of,
                        path,
                    )?;
                }
            }
        }
    } else {
        for gltf_mesh in document.meshes() {
            for prim in gltf_mesh.primitives() {
                append_primitive(
                    &prim,
                    &buffers,
                    None,
                    &mut mesh,
                    &mut vertex_skins,
                    &mut saw_skinned,
                    &mut saw_unskinned,
                    &mut material_slot_of,
                    path,
                )?;
            }
        }
    }

    let mut materials: Vec<ImportedMaterial> = Vec::with_capacity(material_table.len());
    for key in &material_table {
        match key {
            Some(index) => {
                let src = document
                    .materials()
                    .find(|m| m.index() == Some(*index))
                    .ok_or_else(|| Error::Import(format!("gltf: material {index} vanished")))?;
                materials.push(extract_gltf_material(&src, &buffers, path));
            }
            None => materials.push(ImportedMaterial::default()),
        }
    }

    // Skin payload: only when the FIRST skin covers every triangle primitive (a mixed
    // skinned/unskinned model would deform unweighted vertices to the origin, so it
    // imports as plain geometry instead).
    let mut skin: Option<SkinPayload> = None;
    if skins_count > 0 && saw_skinned && !saw_unskinned {
        skin = Some(build_skin(
            &document,
            &buffers,
            &parents,
            vertex_skins,
            path,
        ));
    } else if saw_skinned && saw_unskinned {
        tracing::warn!(
            "gltf: '{}' mixes skinned and unskinned primitives; importing unskinned",
            path.display()
        );
    }

    if mesh.vertices.is_empty() {
        return Err(Error::Import(format!(
            "gltf: '{}' has no triangle geometry",
            path.display()
        )));
    }
    if !any_normals_present(&mesh) {
        generate_normals(&mut mesh);
    }

    Ok(ImportedModel {
        mesh,
        materials,
        skin,
    })
}

/// Build the parent index of every node (`-1` for a root).
///
/// The `gltf` crate's `Node` exposes its children, not its parent, so one pass over
/// every node's children fills the inverse map.
fn build_parents(document: &gltf::Document) -> Vec<i32> {
    let mut parents = vec![-1i32; document.nodes().count()];
    for node in document.nodes() {
        let parent = node.index() as i32;
        for child in node.children() {
            parents[child.index()] = parent;
        }
    }
    parents
}

/// The world transform of a node: the product of its local TRS matrices up the parent
/// chain.
fn world_transform(document: &gltf::Document, parents: &[i32], index: usize) -> Mat4 {
    let mut transform = local_matrix(document, index);
    let mut parent = parents[index];
    while parent >= 0 {
        let p = parent as usize;
        transform = local_matrix(document, p) * transform;
        parent = parents[p];
    }
    transform
}

/// A node's local TRS as a column-major `Mat4`.
fn local_matrix(document: &gltf::Document, index: usize) -> Mat4 {
    let node = document
        .nodes()
        .nth(index)
        .expect("node index within document range");
    Mat4::from_cols_array_2d(&node.transform().matrix())
}

/// Read one triangle primitive's attributes, optionally bake the node world
/// transform into its vertices, and append the resulting vertex/index/submesh data.
///
/// Non-triangle primitives and primitives without positions are skipped. Tracks
/// whether a skinned (joints + weights) or an unskinned primitive was seen, and the
/// first-seen material slot.
#[allow(clippy::too_many_arguments)]
fn append_primitive(
    prim: &gltf::Primitive,
    buffers: &[gltf::buffer::Data],
    node_transform: Option<&Mat4>,
    mesh: &mut Mesh,
    vertex_skins: &mut Vec<VertexSkin>,
    saw_skinned: &mut bool,
    saw_unskinned: &mut bool,
    material_slot_of: &mut impl FnMut(Option<usize>) -> u32,
    path: &Path,
) -> Result<()> {
    if prim.mode() != Mode::Triangles {
        return Ok(());
    }
    let reader = prim.reader(|buffer| buffers.get(buffer.index()).map(|d| d.0.as_slice()));

    let Some(positions) = reader.read_positions() else {
        return Ok(());
    };
    let positions: Vec<[f32; 3]> = positions.collect();
    let vertex_count = positions.len();

    let normals: Option<Vec<[f32; 3]>> = reader.read_normals().map(|it| it.collect());
    let texcoords: Option<Vec<[f32; 2]>> =
        reader.read_tex_coords(0).map(|it| it.into_f32().collect());
    let joints: Option<Vec<[u16; 4]>> = reader.read_joints(0).map(|it| it.into_u16().collect());
    let weights: Option<Vec<[f32; 4]>> = reader.read_weights(0).map(|it| it.into_f32().collect());

    let is_skinned = joints.is_some() && weights.is_some();
    if is_skinned {
        *saw_skinned = true;
    } else {
        *saw_unskinned = true;
    }

    let material_slot = material_slot_of(prim.material().index());

    let vertex_offset = mesh.vertices.len() as i32;
    let first_index = mesh.indices.len() as u32;
    let normal_transform = node_transform.map(|m| {
        // Inverse-transpose of the upper-3x3 transforms normals correctly under a
        // non-uniform scale.
        Mat3::from_mat4(*m).inverse().transpose()
    });

    for i in 0..vertex_count {
        let mut position = Vec3::from_array(positions[i]);
        if let Some(m) = node_transform {
            position = m.transform_point3(position);
        }
        let mut normal = Vec3::ZERO;
        if let Some(ns) = &normals {
            normal = Vec3::from_array(ns[i]);
            if let Some(nt) = &normal_transform {
                normal = (*nt * normal).normalize();
            }
        }
        let uv0 = match &texcoords {
            Some(uvs) => Vec2::from_array(uvs[i]),
            None => Vec2::ZERO,
        };
        mesh.vertices.push(Vertex {
            position,
            normal,
            uv0,
        });

        let mut influence = VertexSkin::default();
        if let (Some(js), Some(ws)) = (&joints, &weights) {
            influence.joints = js[i];
            influence.weights = ws[i];
        }
        vertex_skins.push(influence);
    }

    match reader.read_indices() {
        Some(indices) => {
            for index in indices.into_u32() {
                if index as usize >= vertex_count {
                    return Err(Error::Import(format!(
                        "gltf: '{}' has an out-of-range index",
                        path.display()
                    )));
                }
                mesh.indices.push(index);
            }
        }
        None => {
            for i in 0..vertex_count as u32 {
                mesh.indices.push(i);
            }
        }
    }

    let index_count = mesh.indices.len() as u32 - first_index;
    mesh.submeshes.push(Submesh {
        first_index,
        index_count,
        vertex_offset,
        material_slot,
    });
    Ok(())
}

/// Build the [`SkinPayload`] from the document's first skin: the source node forest,
/// the joint index list, the inverse-bind matrices, the skeleton root and mesh node,
/// the moved skin stream, and the decoded clips.
fn build_skin(
    document: &gltf::Document,
    buffers: &[gltf::buffer::Data],
    parents: &[i32],
    stream: Vec<VertexSkin>,
    path: &Path,
) -> SkinPayload {
    let gltf_skin = document.skins().next().expect("skins_count > 0");

    let mut nodes: Vec<ImportedNode> = Vec::with_capacity(document.nodes().count());
    for node in document.nodes() {
        let index = node.index();
        let name = node
            .name()
            .map(str::to_owned)
            .unwrap_or_else(|| format!("Node {index}"));
        let (translation, rotation, scale) = match node.transform() {
            gltf::scene::Transform::Matrix { matrix } => {
                let (s, r, t) = Affine3A::from_mat4(Mat4::from_cols_array_2d(&matrix))
                    .to_scale_rotation_translation();
                (t, r, s)
            }
            gltf::scene::Transform::Decomposed {
                translation,
                rotation,
                scale,
            } => (
                Vec3::from_array(translation),
                // glam's Quat is xyzw, matching the glTF storage order.
                Quat::from_xyzw(rotation[0], rotation[1], rotation[2], rotation[3]),
                Vec3::from_array(scale),
            ),
        };
        nodes.push(ImportedNode {
            name,
            parent: parents[index],
            translation,
            rotation,
            scale,
        });
    }

    let joints: Vec<i32> = gltf_skin.joints().map(|j| j.index() as i32).collect();
    let joint_count = joints.len();

    let mut inverse_bind = vec![Mat4::IDENTITY; joint_count];
    let skin_reader =
        gltf_skin.reader(|buffer| buffers.get(buffer.index()).map(|d| d.0.as_slice()));
    if let Some(ibms) = skin_reader.read_inverse_bind_matrices() {
        for (j, m) in ibms.enumerate().take(joint_count) {
            inverse_bind[j] = Mat4::from_cols_array_2d(&m);
        }
    }

    let skeleton_root = gltf_skin.skeleton().map(|n| n.index() as i32).unwrap_or(-1);

    let mut mesh_node = -1i32;
    for node in document.nodes() {
        let is_this_skin = node.skin().map(|s| s.index()) == Some(gltf_skin.index());
        if is_this_skin && node.mesh().is_some() {
            mesh_node = node.index() as i32;
            break;
        }
    }

    let desc = ImportedSkin {
        joints,
        inverse_bind,
        skeleton_root,
        mesh_node,
    };

    let animations = decode_clips(document, buffers, &desc, &nodes, path);

    SkinPayload {
        stream,
        nodes,
        desc,
        animations,
    }
}

/// Decode the document's skeletal clips.
///
/// A channel binds to a joint by its position in the skin's joint list; channels
/// targeting a non-joint node, morph weights, or sparse/empty samplers are skipped
/// with a warning. A clip with no surviving tracks is dropped.
fn decode_clips(
    document: &gltf::Document,
    buffers: &[gltf::buffer::Data],
    desc: &ImportedSkin,
    nodes: &[ImportedNode],
    path: &Path,
) -> Vec<AnimClip> {
    let mut clips: Vec<AnimClip> = Vec::new();
    for (a, anim) in document.animations().enumerate() {
        let name = anim
            .name()
            .map(str::to_owned)
            .unwrap_or_else(|| format!("clip_{a}"));
        let mut clip = AnimClip {
            name,
            duration: 0.0,
            tracks: Vec::new(),
        };
        for channel in anim.channels() {
            let target = channel.target();
            let property = target.property();
            if property == Property::MorphTargetWeights {
                tracing::warn!(
                    "gltf: '{}' clip '{}' has a morph-weights channel; skipped",
                    path.display(),
                    clip.name
                );
                continue;
            }
            let node_index = target.node().index() as i32;
            let joint = desc.joints.iter().position(|j| *j == node_index);
            let Some(joint) = joint else {
                tracing::warn!(
                    "gltf: '{}' clip '{}' targets a non-skin node; channel skipped",
                    path.display(),
                    clip.name
                );
                continue;
            };

            let sampler = channel.sampler();
            if sampler.input().sparse().is_some() || sampler.output().sparse().is_some() {
                tracing::warn!(
                    "gltf: '{}' clip '{}' has a sparse or empty sampler; channel skipped",
                    path.display(),
                    clip.name
                );
                continue;
            }

            let reader =
                channel.reader(|buffer| buffers.get(buffer.index()).map(|d| d.0.as_slice()));
            let (Some(inputs), Some(outputs)) = (reader.read_inputs(), reader.read_outputs())
            else {
                tracing::warn!(
                    "gltf: '{}' clip '{}' has a sparse or empty sampler; channel skipped",
                    path.display(),
                    clip.name
                );
                continue;
            };

            let times: Vec<f32> = inputs.collect();
            let mut values: Vec<f32> = Vec::new();
            use gltf::animation::util::ReadOutputs;
            match outputs {
                ReadOutputs::Translations(it) => {
                    for v in it {
                        values.extend_from_slice(&v);
                    }
                }
                ReadOutputs::Scales(it) => {
                    for v in it {
                        values.extend_from_slice(&v);
                    }
                }
                ReadOutputs::Rotations(it) => {
                    for v in it.into_f32() {
                        values.extend_from_slice(&v);
                    }
                }
                // Morph-weights channels are skipped above; nothing reaches here.
                ReadOutputs::MorphTargetWeights(_) => continue,
            }

            let track = AnimTrack {
                joint: joint as i32,
                joint_name: nodes[node_index as usize].name.clone(),
                path: to_track_path(property),
                interp: to_track_interp(sampler.interpolation()),
                times,
                values,
            };
            if let Some(&last) = track.times.last() {
                if last > clip.duration {
                    clip.duration = last;
                }
            }
            clip.tracks.push(track);
        }
        if !clip.tracks.is_empty() {
            clips.push(clip);
        }
    }
    clips
}

/// Map a glTF target property onto the engine's [`AnimPath`].
fn to_track_path(property: Property) -> AnimPath {
    match property {
        Property::Rotation => AnimPath::Rotation,
        Property::Scale => AnimPath::Scale,
        _ => AnimPath::Translation,
    }
}

/// Map a glTF sampler interpolation onto the engine's [`AnimInterp`].
fn to_track_interp(interp: Interpolation) -> AnimInterp {
    match interp {
        Interpolation::Step => AnimInterp::Step,
        Interpolation::CubicSpline => AnimInterp::CubicSpline,
        Interpolation::Linear => AnimInterp::Linear,
    }
}

/// Extract a material's PBR factors and texture byte blobs.
fn extract_gltf_material(
    src: &gltf::Material,
    buffers: &[gltf::buffer::Data],
    path: &Path,
) -> ImportedMaterial {
    let mut material = ImportedMaterial {
        name: src.name().map(str::to_owned).unwrap_or_default(),
        emissive: Vec3::from_array(src.emissive_factor()),
        emissive_strength: src.emissive_strength().unwrap_or(1.0),
        normal: src
            .normal_texture()
            .and_then(|t| read_texture_bytes(&t.texture(), buffers, "normal", path)),
        occlusion: src
            .occlusion_texture()
            .and_then(|t| read_texture_bytes(&t.texture(), buffers, "occlusion", path)),
        emissive_tex: src
            .emissive_texture()
            .and_then(|t| read_texture_bytes(&t.texture(), buffers, "emissive", path)),
        ..Default::default()
    };

    let pbr = src.pbr_metallic_roughness();
    material.base_color = Vec4::from_array(pbr.base_color_factor());
    material.metallic = pbr.metallic_factor();
    material.roughness = pbr.roughness_factor();
    material.albedo = pbr
        .base_color_texture()
        .and_then(|t| read_texture_bytes(&t.texture(), buffers, "albedo", path));
    material.metallic_roughness = pbr
        .metallic_roughness_texture()
        .and_then(|t| read_texture_bytes(&t.texture(), buffers, "metallic-roughness", path));
    material
}

/// Read a texture's encoded image bytes from an embedded buffer view or external
/// file, percent-decoding the URI. A `data:` URI is logged and skipped.
fn read_texture_bytes(
    texture: &gltf::Texture,
    buffers: &[gltf::buffer::Data],
    label: &str,
    path: &Path,
) -> Option<TextureSource> {
    let source = texture.source().source();
    match source {
        gltf::image::Source::View { view, mime_type } => {
            let buffer = &buffers[view.buffer().index()];
            let start = view.offset();
            let end = start + view.length();
            let bytes = buffer.0.get(start..end)?.to_vec();
            if bytes.is_empty() {
                return None;
            }
            Some(TextureSource {
                bytes,
                ext: extension_from_mime(mime_type),
            })
        }
        gltf::image::Source::Uri { uri, .. } => {
            if uri.starts_with("data:") {
                tracing::warn!(
                    "gltf: '{}' embeds its {label} as a data: URI (not yet supported)",
                    path.display()
                );
                return None;
            }
            let decoded = percent_decode(uri);
            let dir = path.parent().unwrap_or_else(|| Path::new("."));
            let full = dir.join(&decoded);
            let bytes = std::fs::read(&full).ok()?;
            Some(TextureSource {
                bytes,
                ext: extension_of(&decoded),
            })
        }
    }
}

/// Map an image MIME type onto a file extension.
fn extension_from_mime(mime: &str) -> String {
    match mime {
        "image/jpeg" => "jpg".to_owned(),
        _ => "png".to_owned(),
    }
}

/// The extension of a path (the substring after the last `.`), empty if none.
fn extension_of(name: &str) -> String {
    match name.rfind('.') {
        Some(dot) => name[dot + 1..].to_owned(),
        None => String::new(),
    }
}

/// Percent-decode a URI in place (`%20` -> space).
fn percent_decode(uri: &str) -> String {
    let bytes = uri.as_bytes();
    let mut out = Vec::with_capacity(bytes.len());
    let mut i = 0;
    while i < bytes.len() {
        if bytes[i] == b'%' && i + 2 < bytes.len() {
            let hi = (bytes[i + 1] as char).to_digit(16);
            let lo = (bytes[i + 2] as char).to_digit(16);
            if let (Some(hi), Some(lo)) = (hi, lo) {
                out.push((hi * 16 + lo) as u8);
                i += 3;
                continue;
            }
        }
        out.push(bytes[i]);
        i += 1;
    }
    String::from_utf8_lossy(&out).into_owned()
}

/// Whether any vertex carries a non-zero normal (the importer recomputes normals only
/// when a source provides none).
fn any_normals_present(mesh: &Mesh) -> bool {
    mesh.vertices.iter().any(|v| v.normal.dot(v.normal) > 1e-12)
}
