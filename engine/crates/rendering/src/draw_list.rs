//! The per-frame scene draw list: the inputs ([`DrawItem`] + [`SubmeshMaterial`]),
//! the batched output ([`DrawBatch`] / [`SceneDrawList`]), and the [`RenderStats`]
//! counters.
//!
//! [`crate::Instancing::submit_draw_list`] resolves each item's material to a cached
//! PSO, buckets by (pipeline, mesh) into instanced draws, deduplicates the per-frame
//! material table, and produces the [`SceneDrawList`] the scene + depth passes record.
//! These are the Rust expression of the C++ `DrawItem` (`renderer_types.cppm:582`),
//! `DrawBatch` (`:604`), `SceneDrawList` (`:644`), and `RenderStats` (`:710`).
//!
//! Skinned items carry a joint palette: [`crate::Instancing::submit_draw_list`] deforms
//! each into its slice of the frame's deformed-vertex buffer (the [`SkinDispatch`] the
//! `skin` compute pass replays), then draws it as a static instance reading that slice.
//! The [`SkinnedRtInstance`] list rides for the RT refit BLAS (phase 13).

use std::sync::Arc;

use ash::vk;
use saffron_geometry::glam::{Mat3, Mat4, Vec2, Vec3, Vec4};

use crate::gpu_types::Material;
use crate::resources::{GpuMesh, GpuTexture, Pipeline};

/// One submesh's material: its textures (each `None` → the default white slot) plus
/// the PBR factors that fold into the per-frame [`crate::MaterialParamsData`].
///
/// A [`DrawItem`] carries one per mesh submesh, indexed by `Submesh::material_slot`
/// order; a single entry applies to every submesh (clamped). The C++ `SubmeshMaterial`
/// (`renderer_types.cppm:557`).
#[derive(Clone)]
pub struct SubmeshMaterial {
    /// Base-color / albedo texture (`None` → default white, factors unchanged).
    pub albedo_texture: Option<Arc<GpuTexture>>,
    /// Metallic-roughness / ORM texture (`None` → default white).
    pub metallic_roughness_texture: Option<Arc<GpuTexture>>,
    /// Tangent-space normal map (sets the `NORMAL` feature bit when present).
    pub normal_texture: Option<Arc<GpuTexture>>,
    /// Ambient-occlusion map (AO in R; sets the `OCCLUSION` feature bit).
    pub occlusion_texture: Option<Arc<GpuTexture>>,
    /// Emissive map (modulates the emissive factor; sets `EMISSIVE_TEX`).
    pub emissive_texture: Option<Arc<GpuTexture>>,
    /// Height / displacement map for parallax (sets the `HEIGHT` feature bit).
    pub height_texture: Option<Arc<GpuTexture>>,
    /// Base color (RGBA), multiplied with the albedo texture.
    pub base_color: Vec4,
    /// Metallic factor.
    pub metallic: f32,
    /// Roughness factor.
    pub roughness: f32,
    /// Emissive radiance factor.
    pub emissive: Vec3,
    /// Emissive strength multiplier on [`SubmeshMaterial::emissive`].
    pub emissive_strength: f32,
    /// Normal-map strength.
    pub normal_strength: f32,
    /// UV tiling (multiplied into the sampled UV).
    pub uv_tiling: Vec2,
    /// UV offset (added to the tiled UV).
    pub uv_offset: Vec2,
    /// Parallax height scale.
    pub height_scale: f32,
    /// Masked / alpha-clip: discard fragments below [`SubmeshMaterial::alpha_cutoff`].
    pub alpha_clip: bool,
    /// The alpha-clip cutoff threshold.
    pub alpha_cutoff: f32,
}

impl SubmeshMaterial {
    /// The default glTF metallic-roughness defaults the draw path applies when an item
    /// carries no per-submesh material — white base color, dielectric, fully rough.
    /// Matches the per-row defaults `submitDrawList` seeds before reading the material.
    pub fn defaults() -> Self {
        Self {
            albedo_texture: None,
            metallic_roughness_texture: None,
            normal_texture: None,
            occlusion_texture: None,
            emissive_texture: None,
            height_texture: None,
            base_color: Vec4::ONE,
            metallic: 0.0,
            roughness: 1.0,
            emissive: Vec3::ZERO,
            emissive_strength: 1.0,
            normal_strength: 1.0,
            uv_tiling: Vec2::ONE,
            uv_offset: Vec2::ZERO,
            height_scale: 0.05,
            alpha_clip: false,
            alpha_cutoff: 0.5,
        }
    }
}

impl Default for SubmeshMaterial {
    fn default() -> Self {
        Self::defaults()
    }
}

/// One renderable submitted to the scene draw list: a mesh, its world transform, the
/// per-submesh materials, and the PSO-selecting [`Material`].
///
/// `submit_draw_list` resolves the material to a cached PSO and batches by
/// (pipeline, mesh) into instanced draws. The C++ `DrawItem` (`renderer_types.cppm:582`).
#[derive(Clone)]
pub struct DrawItem {
    /// The mesh to draw.
    pub mesh: Arc<GpuMesh>,
    /// World matrix.
    pub model: Mat4,
    /// `transpose(inverse(mat3(model)))` for correct normals under non-uniform scale.
    pub normal_matrix: Mat4,
    /// One entry per mesh submesh; a single entry applies to all submeshes (clamped).
    pub submesh_materials: Vec<SubmeshMaterial>,
    /// Selects the PSO (the übershader permutation), shared by all submeshes.
    pub material: Material,
    /// GPU skinning: when set the item is deformed once by the `skin` compute pass into
    /// its slice of the frame's deformed-vertex buffer, then drawn as a lone static
    /// instance reading that slice. A skinned item with no mesh skin stream is dropped.
    pub skinned: bool,
    /// Skinning: the base of this instance's joints in the frame palette.
    pub joint_offset: u32,
    /// Skinning: matrices this instance contributes (its palette slice length).
    pub joint_count: u32,
    /// Source entity id (0 = none), keying the cross-frame motion caches (TAA + skin).
    pub entity: u64,
}

impl DrawItem {
    /// A static draw item: a mesh + transform + materials with the default (lit
    /// übershader) [`Material`] and no skinning.
    pub fn new(mesh: Arc<GpuMesh>, model: Mat4, submesh_materials: Vec<SubmeshMaterial>) -> Self {
        Self {
            mesh,
            model,
            normal_matrix: normal_matrix(model),
            submesh_materials,
            material: Material::default(),
            skinned: false,
            joint_offset: 0,
            joint_count: 0,
            entity: 0,
        }
    }
}

/// `transpose(inverse(mat3(model)))` extended to a `Mat4` — the normal matrix the
/// instance row carries so non-uniform scale leaves normals orthogonal to the surface.
pub fn normal_matrix(model: Mat4) -> Mat4 {
    Mat4::from_mat3(Mat3::from_mat4(model).inverse().transpose())
}

/// A batch of instances sharing a pipeline + mesh, drawn as one instanced draw per
/// submesh. Bindless means the per-instance texture indices live in the instance SSBO,
/// not a per-batch descriptor — so texture differences never split a batch.
/// `base_instance` offsets into the frame's instance buffer. The C++ `DrawBatch`
/// (`renderer_types.cppm:604`).
#[derive(Clone)]
pub struct DrawBatch {
    /// The PSO resolved from the material via the cache.
    pub pipeline: Arc<Pipeline>,
    /// The mesh whose vertex/index streams the batch binds and draws.
    pub mesh: Arc<GpuMesh>,
    /// The base offset into the frame's instance buffer (submesh 0, instance 0).
    pub base_instance: u32,
    /// The number of logical instances in the batch.
    pub instance_count: u32,
    /// When set the batch draws the frame's compute-deformed buffer as its binding-0
    /// vertex stream (the static stream otherwise); a skinned batch is always one
    /// instance.
    pub skinned: bool,
    /// The base vertex of this batch's instance in the deformed buffer (0 for the static
    /// path), added to each submesh's `vertex_offset` in the deformed draw.
    pub deformed_vertex_offset: u32,
}

/// One skinned mesh-instance's compute work for the frame: the descriptor set wiring its
/// static + skin streams, the joint palette, and the deformed output, plus the push the
/// `skin` kernel reads. Built by [`crate::Instancing::submit_draw_list`] and replayed in
/// the `skin` pass. The C++ `SkinDispatch` (`renderer_types.cppm:620`).
#[derive(Clone, Copy)]
pub struct SkinDispatch {
    /// The per-dispatch descriptor set (static vertices, skin, palette, deformed output).
    pub set: vk::DescriptorSet,
    /// The skinned mesh-instance's vertex count (one compute invocation each).
    pub vertex_count: u32,
    /// The base of this instance's joints in the bound palette.
    pub joint_offset: u32,
    /// The base of this instance's vertices in the deformed output buffer.
    pub deformed_offset: u32,
}

/// One skinned mesh-instance the TLAS references via its own per-frame refit BLAS. The
/// deformed vertices are already in world space (the palette is `worldBone * inverseBind`
/// and the skin kernel omits the model matrix), so the TLAS transform is identity. The
/// C++ `SkinnedRtInstance` (`renderer_types.cppm:638`); the AS build lands in phase 13.
#[derive(Clone)]
pub struct SkinnedRtInstance {
    /// Keys the grow-only per-instance refit BLAS (built once, then updated).
    pub entity: u64,
    /// The instance's base vertex in the frame's deformed buffer.
    pub deformed_offset: u32,
    /// The skinned vertex count.
    pub vertex_count: u32,
    /// The index count (the BLAS geometry's triangle source).
    pub index_count: u32,
    /// The mesh supplying the index stream for the BLAS geometry.
    pub mesh: Arc<GpuMesh>,
}

/// The frame's structured draw list, built by `submit_draw_list` and recorded by the
/// scene pass (shaded) and the optional depth pre-pass (depth only). The C++
/// `SceneDrawList` (`renderer_types.cppm:644`).
#[derive(Default)]
pub struct SceneDrawList {
    /// The camera view-projection (the per-frame vertex push constant).
    pub view_proj: Mat4,
    /// The batched instanced draws, in first-seen bucket order.
    pub batches: Vec<DrawBatch>,
    /// Per skinned mesh-instance: the compute work the `skin` pass dispatches before any
    /// geometry pass reads the deformed buffer. Empty when no skinned instances exist.
    pub skin_dispatches: Vec<SkinDispatch>,
    /// The parallel dispatches that deform the previous pose into the prev-deformed
    /// buffer (previous palette + previous-deformed output), read only by the motion pass.
    pub prev_skin_dispatches: Vec<SkinDispatch>,
    /// Per skinned instance: the entity + deformed offset the RT refit BLAS reads (phase
    /// 13). Empty unless an RT consumer is armed.
    pub skinned_rt_instances: Vec<SkinnedRtInstance>,
    /// Textures pinned live for the frame (their bindless indices are referenced by
    /// the instance SSBO, so the `Arc`s must outlive the GPU read).
    pub live_textures: Vec<Arc<GpuTexture>>,
    /// `true` once a draw list has been built this frame (the C++ `valid`).
    pub valid: bool,
}

impl SceneDrawList {
    /// A recording-only copy: the batches (their `Arc`s cloned cheaply) plus the
    /// view-projection and validity, with no `live_textures`. Both the depth pre-pass
    /// and scene-pass bodies take one; the texture pins stay on the owning list until
    /// the frame's fence is waited next, so they outlive the GPU read.
    pub fn shallow_clone(&self) -> Self {
        Self {
            view_proj: self.view_proj,
            batches: self.batches.clone(),
            skin_dispatches: self.skin_dispatches.clone(),
            prev_skin_dispatches: self.prev_skin_dispatches.clone(),
            skinned_rt_instances: self.skinned_rt_instances.clone(),
            live_textures: Vec::new(),
            valid: self.valid,
        }
    }
}

/// Per-frame scene draw counters, refreshed each `submit_draw_list` and inspectable to
/// verify the batching is not O(draws). The C++ `RenderStats` (`renderer_types.cppm:710`)
/// reduced to the draw-path counters this phase produces.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct RenderStats {
    /// `drawIndexed` calls (one per submesh per batch).
    pub draw_calls: u32,
    /// Distinct (pipeline, mesh) buckets.
    pub batches: u32,
    /// Total logical instances drawn.
    pub instances: u32,
    /// Triangles submitted (sum of `index_count / 3` over instances).
    pub triangles: u32,
    /// Descriptor-set binds recorded in the scene pass.
    pub descriptor_binds: u32,
    /// Primary command buffers submitted this frame (the C++ `commandBuffers`).
    pub command_buffers: u32,
    /// `vkQueueSubmit2` calls this frame (the C++ `queueSubmits`).
    pub queue_submits: u32,
    /// PSOs compiled this frame (non-zero on a steady-state frame = a compile hitch).
    pub pipelines_created: u32,
}
