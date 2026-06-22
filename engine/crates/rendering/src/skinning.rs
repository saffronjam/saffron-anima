//! The compute skinning pre-pass apparatus.
//!
//! [`Skinning`] owns, per frame-in-flight, a deformed-vertex buffer (base 32-byte
//! [`saffron_geometry::Vertex`] layout, `STORAGE|VERTEX`, grow-only) that every skinned
//! mesh-instance deforms into, plus a parallel prev-deformed buffer the motion pass reads
//! as the prev-position stream. The `skin` compute PSO deforms once per vertex; every
//! geometry pass then reads the result as an ordinary static vertex stream — the
//! deform-once win. The per-instance descriptor sets are allocated from a per-frame pool
//! that is reset wholesale each frame.
//!
//! It also owns the cross-frame motion caches keyed by entity uuid: last frame's palette
//! slice (deformation motion) + last frame's world matrix (object motion). A new entity
//! reads back current == previous, so its first frame emits zero motion (no velocity
//! flash). These are single-threaded host state mutated through [`Skinning`]'s methods.
//!
//! The joint + prev-joint palettes live in [`crate::Instancing`] (set 2, binding 1).

use std::collections::HashMap;
use std::sync::Arc;

use ash::vk;
use saffron_geometry::Vertex;
use saffron_geometry::glam::Mat4;

use crate::draw_list::SkinDispatch;
use crate::frame::MAX_FRAMES_IN_FLIGHT;
use crate::pipelines::Pipelines;
use crate::resources::{Buffer, DeviceResources, GpuMesh};
use crate::{Device, Result, checked};

/// Per-frame ceiling on compute-skinning descriptor sets — one per skinned mesh-instance.
/// The skin pool is reset and re-allocated each frame; instances past this are skipped
/// (logged), not an error.
pub const SKIN_MAX_SETS_PER_FRAME: u32 = 64;

/// Initial deformed-vertex-buffer capacity in [`Vertex`] (32-byte) elements.
const INITIAL_DEFORMED_CAPACITY: u32 = 4096;

/// The descriptor-set capacity of each per-frame skin pool. Each skinned instance takes
/// one set for the current dispatch and one for its prev-pose sibling, so the pool holds
/// twice the per-frame instance budget.
const SKIN_POOL_SET_CAPACITY: u32 = SKIN_MAX_SETS_PER_FRAME * 2;

/// One frame-in-flight's skinning storage: the descriptor pool (reset each frame) plus the
/// grow-only deformed + prev-deformed vertex buffers.
struct FrameSkinning {
    pool: vk::DescriptorPool,
    deformed: Option<Buffer>,
    deformed_capacity: u32,
    prev_deformed: Option<Buffer>,
    prev_deformed_capacity: u32,
}

/// The per-dispatch buffer handles the set wiring binds, resolved once per frame from the
/// frame's deformed buffers and the instance palette buffers.
#[derive(Clone, Copy)]
pub struct SkinBufferSet {
    /// The current joint palette (set 2, binding 1 of [`crate::Instancing`]).
    pub palette: vk::Buffer,
    /// The current palette's bound size in bytes.
    pub palette_size: vk::DeviceSize,
    /// The previous joint palette (the prev-pose dispatch's bound palette).
    pub prev_palette: vk::Buffer,
    /// The previous palette's bound size in bytes.
    pub prev_palette_size: vk::DeviceSize,
}

/// Per-skinned-bucket metadata the draw-list batcher assembles before wiring: the mesh
/// (its static + skin streams + vertex count) and where the bucket lands in the palette /
/// deformed buffer.
pub struct SkinBucket {
    /// The mesh supplying the static vertex + skin streams (binding 0/1) and the index
    /// stream for the RT refit BLAS.
    pub mesh: Arc<GpuMesh>,
    /// The base of this bucket's joints in the frame palette.
    pub joint_offset: u32,
    /// The base vertex of this bucket's instance in the deformed buffer.
    pub deformed_offset: u32,
}

/// The compute skinning pre-pass apparatus: the per-frame deformed buffers + descriptor
/// pools + the cross-frame motion caches. Built once in [`Skinning::new`], then mutated
/// only through [`Skinning::wire_dispatches`] taking `&mut self`. Each [`Buffer`] is a
/// Drop type holding the allocator `Arc`, so the deformed buffers free without a live
/// `&Device`; [`Drop`] destroys the device-borrowing pools + set layout (the run loop
/// idles the GPU before any teardown, README §4).
pub struct Skinning {
    resources: Arc<DeviceResources>,
    set_layout: vk::DescriptorSetLayout,
    frames: Vec<FrameSkinning>,
    peak_vertices: u32,
    /// Last frame's joint palette slice per entity (deformation motion); a missing entry
    /// means "uncached" → the prev palette copies the current one (zero deformation
    /// motion on the first frame).
    prev_palette_by_entity: HashMap<u64, Vec<Mat4>>,
    /// Last frame's world matrix per entity (object motion); a missing entry means the
    /// instance reprojects against itself (zero object motion on the first frame).
    prev_model_by_entity: HashMap<u64, Mat4>,
    /// Whether RT is supported — when set, the deformed buffer also feeds the per-frame
    /// skinned BLAS refit, so it carries shader-device-address + AS-build-input usage.
    rt_supported: bool,
}

impl Skinning {
    /// Creates the skin descriptor-set layout (four storage buffers: static vertices,
    /// skin, palette, deformed output) and the per-frame descriptor pools.
    ///
    /// # Errors
    ///
    /// Returns [`crate::Error::Vk`] if the layout or a pool cannot be created.
    pub fn new(device: &Device) -> Result<Self> {
        let raw = device.resources().device();
        let set_layout = create_skin_set_layout(raw)?;
        let mut frames: Vec<FrameSkinning> = Vec::with_capacity(MAX_FRAMES_IN_FLIGHT);
        for _ in 0..MAX_FRAMES_IN_FLIGHT {
            let pool = match create_skin_pool(raw) {
                Ok(pool) => pool,
                Err(err) => {
                    // Destroy what was built before propagating (no Drop runs on `Self`).
                    for frame in &frames {
                        // SAFETY: the ash seam. Each pool was created above; freed once.
                        unsafe { raw.destroy_descriptor_pool(frame.pool, None) };
                    }
                    // SAFETY: the ash seam. The layout was created above; freed once.
                    unsafe { raw.destroy_descriptor_set_layout(set_layout, None) };
                    return Err(err);
                }
            };
            frames.push(FrameSkinning {
                pool,
                deformed: None,
                deformed_capacity: 0,
                prev_deformed: None,
                prev_deformed_capacity: 0,
            });
        }
        Ok(Self {
            resources: Arc::clone(device.resources()),
            set_layout,
            frames,
            peak_vertices: 0,
            prev_palette_by_entity: HashMap::new(),
            prev_model_by_entity: HashMap::new(),
            rt_supported: device.rt_supported(),
        })
    }

    /// The frame's deformed-vertex buffer handle, or `None` before its first grow. The
    /// scene / depth / shadow passes bind it as binding 0 for a skinned batch.
    pub fn deformed_buffer(&self, frame: usize) -> Option<vk::Buffer> {
        self.frames[frame].deformed.as_ref().map(Buffer::handle)
    }

    /// The frame's prev-deformed-vertex buffer handle, or `None` before its first grow.
    /// The motion pass binds it as binding 1 for a skinned batch (the prev-position
    /// stream).
    pub fn prev_deformed_buffer(&self, frame: usize) -> Option<vk::Buffer> {
        self.frames[frame]
            .prev_deformed
            .as_ref()
            .map(Buffer::handle)
    }

    /// The peak deformed-buffer capacity ever allocated (the grow-only high-water mark in
    /// [`Vertex`] elements). Never shrinks.
    pub fn peak_vertices(&self) -> u32 {
        self.peak_vertices
    }

    /// Looks up the entity's cached previous world matrix, or `None` when uncached (a new
    /// entity reprojects against its current pose → zero object motion on frame one). The
    /// draw-list batcher reads this when building each instance's `prev_model`.
    pub fn prev_model(&self, entity: u64) -> Option<Mat4> {
        self.prev_model_by_entity.get(&entity).copied()
    }

    /// Replaces an entity's cached previous palette slice with `current`, then returns the
    /// slice to seed the prev palette: the entity's cached slice when its length matches,
    /// else a copy of `current` (uncached or length-changed → zero deformation motion).
    ///
    /// Committing the cache here (rather than after the loop) is sound because each entity
    /// appears once per draw list; the read-then-overwrite order preserves last-frame
    /// semantics for this frame's prev palette.
    pub fn swap_palette(&mut self, entity: u64, current: &[Mat4]) -> Vec<Mat4> {
        let prev = match self.prev_palette_by_entity.get(&entity) {
            Some(cached) if cached.len() == current.len() => cached.clone(),
            _ => current.to_vec(),
        };
        self.prev_palette_by_entity.insert(entity, current.to_vec());
        prev
    }

    /// Commits an entity's current world matrix into the cross-frame cache, so next frame
    /// reprojects from this pose. Called after the prev matrix was read for this frame.
    pub fn commit_model(&mut self, entity: u64, model: Mat4) {
        self.prev_model_by_entity.insert(entity, model);
    }

    /// Sizes the frame's deformed + prev-deformed buffers to `vertex_count`, resets the
    /// frame's descriptor pool, and allocates + writes one descriptor set per bucket for
    /// the current dispatch and one for its prev-pose sibling.
    ///
    /// On success the `set` field of each [`SkinDispatch`] in `dispatches` /
    /// `prev_dispatches` (which run parallel to `buckets`) is filled. A wiring failure
    /// clears both lists (so the `skin` pass is skipped) and returns `Ok` — the
    /// dispatches are dropped rather than failing the frame.
    ///
    /// # Errors
    ///
    /// Returns [`crate::Error::Vk`] if growing a deformed buffer fails.
    pub fn wire_dispatches(
        &mut self,
        frame: usize,
        vertex_count: u32,
        buffers: SkinBufferSet,
        buckets: &[SkinBucket],
        dispatches: &mut [SkinDispatch],
        prev_dispatches: &mut [SkinDispatch],
    ) -> Result<bool> {
        debug_assert_eq!(buckets.len(), dispatches.len());
        debug_assert_eq!(buckets.len(), prev_dispatches.len());

        self.ensure_deformed_capacity(frame, vertex_count)?;
        self.ensure_prev_deformed_capacity(frame, vertex_count)?;

        let raw = self.resources.device();
        // SAFETY: the ash seam. The slot's prior GPU work was waited (the caller resets the
        // command pool under the frame fence), so every set in the pool is idle.
        if let Err(result) = unsafe {
            raw.reset_descriptor_pool(
                self.frames[frame].pool,
                vk::DescriptorPoolResetFlags::empty(),
            )
        } {
            tracing::error!("skinning: resetDescriptorPool failed: {result:?}");
            return Ok(false);
        }

        let deformed = self.frames[frame]
            .deformed
            .as_ref()
            .expect("deformed buffer grown");
        let prev_deformed = self.frames[frame]
            .prev_deformed
            .as_ref()
            .expect("prev-deformed buffer grown");
        let deformed_buffer = deformed.handle();
        let deformed_size = deformed.size();
        let prev_deformed_buffer = prev_deformed.handle();
        let prev_deformed_size = prev_deformed.size();

        let pool = self.frames[frame].pool;
        for (i, bucket) in buckets.iter().enumerate() {
            let cur = wire_set(
                raw,
                pool,
                self.set_layout,
                &bucket.mesh,
                buffers.palette,
                buffers.palette_size,
                deformed_buffer,
                deformed_size,
            );
            let prev = wire_set(
                raw,
                pool,
                self.set_layout,
                &bucket.mesh,
                buffers.prev_palette,
                buffers.prev_palette_size,
                prev_deformed_buffer,
                prev_deformed_size,
            );
            match (cur, prev) {
                (Some(cur), Some(prev)) => {
                    dispatches[i].set = cur;
                    prev_dispatches[i].set = prev;
                }
                _ => {
                    // The deformed buffer is unwritten; drop every dispatch so the skin
                    // pass is skipped and the batches read the undeformed bind pose.
                    for d in dispatches.iter_mut() {
                        d.set = vk::DescriptorSet::null();
                    }
                    for d in prev_dispatches.iter_mut() {
                        d.set = vk::DescriptorSet::null();
                    }
                    return Ok(false);
                }
            }
        }
        Ok(true)
    }

    /// Ensures the frame's deformed buffer holds at least `vertex_count` [`Vertex`]
    /// elements, growing to the next power of two (never shrinking). Logs the new peak.
    fn ensure_deformed_capacity(&mut self, frame: usize, vertex_count: u32) -> Result<()> {
        if self.frames[frame].deformed.is_some()
            && self.frames[frame].deformed_capacity >= vertex_count
        {
            return Ok(());
        }
        let capacity = grow_capacity(self.frames[frame].deformed_capacity, vertex_count);
        let buffer = make_deformed_buffer(&self.resources, capacity, self.rt_supported)?;
        self.frames[frame].deformed = Some(buffer);
        self.frames[frame].deformed_capacity = capacity;
        if capacity > self.peak_vertices {
            self.peak_vertices = capacity;
            tracing::info!(
                "skinning: deformed-vertex buffer grew to {} vertices ({} KiB)",
                capacity,
                u64::from(capacity) * size_of::<Vertex>() as u64 / 1024
            );
        }
        Ok(())
    }

    /// The prev-deformed sibling of [`Skinning::ensure_deformed_capacity`] (same grow-only
    /// policy, laid out identically so the per-instance offset matches).
    fn ensure_prev_deformed_capacity(&mut self, frame: usize, vertex_count: u32) -> Result<()> {
        if self.frames[frame].prev_deformed.is_some()
            && self.frames[frame].prev_deformed_capacity >= vertex_count
        {
            return Ok(());
        }
        let capacity = grow_capacity(self.frames[frame].prev_deformed_capacity, vertex_count);
        let buffer = make_deformed_buffer(&self.resources, capacity, self.rt_supported)?;
        self.frames[frame].prev_deformed = Some(buffer);
        self.frames[frame].prev_deformed_capacity = capacity;
        Ok(())
    }

    /// Replays the frame's skin dispatches on `cmd`: bind the skin PSO, then per dispatch
    /// bind its set + push (vertexCount / jointOffset / deformedOffset) and dispatch one
    /// group of 64 invocations per vertex. The current pose into the deformed buffer, the
    /// previous pose into the prev-deformed buffer (the kernel is identical).
    pub fn record_skin(
        raw: &ash::Device,
        cmd: vk::CommandBuffer,
        pipeline: vk::Pipeline,
        layout: vk::PipelineLayout,
        dispatches: &[SkinDispatch],
        prev_dispatches: &[SkinDispatch],
    ) {
        if dispatches.is_empty() {
            return;
        }
        // SAFETY: the ash seam. The PSO is valid this frame; each set wires the bucket's
        // static + skin streams, palette, and deformed output; the dispatch covers the
        // vertex count (64 per group).
        unsafe {
            raw.cmd_bind_pipeline(cmd, vk::PipelineBindPoint::COMPUTE, pipeline);
        }
        for d in dispatches.iter().chain(prev_dispatches) {
            if d.set == vk::DescriptorSet::null() {
                continue;
            }
            let push = SkinPush {
                vertex_count: d.vertex_count,
                joint_offset: d.joint_offset,
                deformed_offset: d.deformed_offset,
                pad: 0,
            };
            // SAFETY: the ash seam. As above; the push spans the declared 16-byte range.
            unsafe {
                raw.cmd_bind_descriptor_sets(
                    cmd,
                    vk::PipelineBindPoint::COMPUTE,
                    layout,
                    0,
                    &[d.set],
                    &[],
                );
                raw.cmd_push_constants(
                    cmd,
                    layout,
                    vk::ShaderStageFlags::COMPUTE,
                    0,
                    bytemuck::bytes_of(&push),
                );
                raw.cmd_dispatch(cmd, d.vertex_count.div_ceil(64), 1, 1);
            }
        }
    }
}

impl Drop for Skinning {
    fn drop(&mut self) {
        // The per-frame pools + the set layout borrow the device; the `Arc<DeviceResources>`
        // keeps it alive for this call. The run loop idled the GPU before any teardown
        // (README §4), so no set in a pool is still in flight. The deformed buffers (the
        // `Option<Buffer>` fields) Drop themselves after this, freeing through the allocator.
        let raw = self.resources.device();
        for frame in &self.frames {
            // SAFETY: the ash seam. The GPU was idled; each pool is freed exactly once.
            unsafe { raw.destroy_descriptor_pool(frame.pool, None) };
        }
        // SAFETY: the ash seam. The layout is freed exactly once after the pools.
        unsafe { raw.destroy_descriptor_set_layout(self.set_layout, None) };
    }
}

/// The skin kernel's 16-byte push constant — `vertexCount / jointOffset / deformedOffset`
/// plus a pad, matching `skin.slang`'s `Push`.
#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
struct SkinPush {
    vertex_count: u32,
    joint_offset: u32,
    deformed_offset: u32,
    pad: u32,
}

/// Allocates one skin descriptor set from `pool` and writes its four storage-buffer
/// bindings (static vertices, skin, palette, deformed output). Returns `None` on an
/// allocation failure (logged), so the caller drops the dispatches.
#[allow(clippy::too_many_arguments)]
fn wire_set(
    raw: &ash::Device,
    pool: vk::DescriptorPool,
    layout: vk::DescriptorSetLayout,
    mesh: &GpuMesh,
    palette: vk::Buffer,
    palette_size: vk::DeviceSize,
    deformed: vk::Buffer,
    deformed_size: vk::DeviceSize,
) -> Option<vk::DescriptorSet> {
    let skin = mesh.skin_buffer()?;
    let layouts = [layout];
    let info = vk::DescriptorSetAllocateInfo::default()
        .descriptor_pool(pool)
        .set_layouts(&layouts);
    // SAFETY: the ash seam. The layout outlives the call; the set lives until the pool is
    // reset (next frame) or destroyed.
    let set = match unsafe { raw.allocate_descriptor_sets(&info) } {
        Ok(sets) => sets[0],
        Err(result) => {
            tracing::error!("skinning: allocate skin set failed: {result:?}");
            return None;
        }
    };
    let infos = [
        vk::DescriptorBufferInfo {
            buffer: mesh.vertex_buffer(),
            offset: 0,
            range: vk::WHOLE_SIZE,
        },
        vk::DescriptorBufferInfo {
            buffer: skin,
            offset: 0,
            range: vk::WHOLE_SIZE,
        },
        vk::DescriptorBufferInfo {
            buffer: palette,
            offset: 0,
            range: palette_size,
        },
        vk::DescriptorBufferInfo {
            buffer: deformed,
            offset: 0,
            range: deformed_size,
        },
    ];
    let writes: Vec<vk::WriteDescriptorSet> = (0..4)
        .map(|b| {
            vk::WriteDescriptorSet::default()
                .dst_set(set)
                .dst_binding(b as u32)
                .descriptor_type(vk::DescriptorType::STORAGE_BUFFER)
                .buffer_info(std::slice::from_ref(&infos[b]))
        })
        .collect();
    // SAFETY: the ash seam. The set + buffers outlive the call; each write targets a
    // single binding the layout declares.
    unsafe { raw.update_descriptor_sets(&writes, &[]) };
    Some(set)
}

/// Grows `current` (a [`Vertex`]-element capacity) to the next power of two that holds
/// `count`, seeding from the initial capacity when empty and never shrinking.
fn grow_capacity(current: u32, count: u32) -> u32 {
    let mut capacity = if current == 0 {
        INITIAL_DEFORMED_CAPACITY
    } else {
        current
    };
    while capacity < count {
        capacity *= 2;
    }
    capacity
}

/// Allocates a device-local deformed-vertex buffer of `capacity` [`Vertex`] elements with
/// `STORAGE|VERTEX` usage. When `rt_supported`, the buffer also feeds the per-frame
/// skinned BLAS refit, so it adds shader-device-address + AS-build-input usage.
fn make_deformed_buffer(
    resources: &Arc<DeviceResources>,
    capacity: u32,
    rt_supported: bool,
) -> Result<Buffer> {
    let size = u64::from(capacity) * size_of::<Vertex>() as u64;
    let mut usage = vk::BufferUsageFlags::STORAGE_BUFFER | vk::BufferUsageFlags::VERTEX_BUFFER;
    if rt_supported {
        usage |= vk::BufferUsageFlags::SHADER_DEVICE_ADDRESS
            | vk::BufferUsageFlags::ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_KHR;
    }
    let alloc_info = vk_mem::AllocationCreateInfo {
        usage: vk_mem::MemoryUsage::AutoPreferDevice,
        ..Default::default()
    };
    Buffer::new(resources, size, usage, &alloc_info)
}

/// The skin set layout: four compute-stage storage buffers (static vertices, skin,
/// palette, deformed output) matching `skin.slang`'s bindings 0-3.
fn create_skin_set_layout(raw: &ash::Device) -> Result<vk::DescriptorSetLayout> {
    let bindings: Vec<vk::DescriptorSetLayoutBinding> = (0..4)
        .map(|b| {
            vk::DescriptorSetLayoutBinding::default()
                .binding(b)
                .descriptor_type(vk::DescriptorType::STORAGE_BUFFER)
                .descriptor_count(1)
                .stage_flags(vk::ShaderStageFlags::COMPUTE)
        })
        .collect();
    let info = vk::DescriptorSetLayoutCreateInfo::default().bindings(&bindings);
    // SAFETY: the ash seam.
    checked(
        unsafe { raw.create_descriptor_set_layout(&info, None) },
        "skinSetLayout",
    )
}

/// Creates a per-frame skin descriptor pool sized for [`SKIN_POOL_SET_CAPACITY`] sets,
/// each with four storage buffers.
fn create_skin_pool(raw: &ash::Device) -> Result<vk::DescriptorPool> {
    let sizes = [vk::DescriptorPoolSize::default()
        .ty(vk::DescriptorType::STORAGE_BUFFER)
        .descriptor_count(SKIN_POOL_SET_CAPACITY * 4)];
    let info = vk::DescriptorPoolCreateInfo::default()
        .max_sets(SKIN_POOL_SET_CAPACITY)
        .pool_sizes(&sizes);
    // SAFETY: the ash seam.
    checked(
        unsafe { raw.create_descriptor_pool(&info, None) },
        "skinPool",
    )
}

/// Clamps the parallel dispatch / prev-dispatch / bucket / RT lists to
/// [`SKIN_MAX_SETS_PER_FRAME`], logging the overflow (a clamp, not an error). Returns the
/// retained count.
pub fn clamp_to_set_budget(count: usize) -> usize {
    if count > SKIN_MAX_SETS_PER_FRAME as usize {
        tracing::warn!(
            "skinning: {count} skinned instances exceed the {SKIN_MAX_SETS_PER_FRAME}-set frame budget; clamping"
        );
        SKIN_MAX_SETS_PER_FRAME as usize
    } else {
        count
    }
}

/// Requests the `skin` compute PSO from `pipelines`, returning `None` on a build failure
/// (logged). The PSO binds the skin set layout owned by [`Skinning`] and a 16-byte push.
pub fn request_skin_pipeline(
    pipelines: &mut Pipelines,
    skinning: &Skinning,
) -> Option<Arc<crate::Pipeline>> {
    pipelines.request_skin(skinning.set_layout)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The grow policy seeds from the initial capacity when empty, doubles to cover the
    /// vertex count, and never shrinks below the existing capacity — the deformed buffer
    /// grows to fit peak vertices and is not shrunk.
    #[test]
    fn grow_capacity_doubles_and_never_shrinks() {
        assert_eq!(grow_capacity(0, 1), INITIAL_DEFORMED_CAPACITY);
        assert_eq!(grow_capacity(0, INITIAL_DEFORMED_CAPACITY + 1), 8192);
        assert_eq!(grow_capacity(8192, 100), 8192, "never shrinks");
        assert_eq!(grow_capacity(4096, 4096), 4096, "exact fit holds");
        assert_eq!(grow_capacity(4096, 20000), 32768);
    }

    /// Dispatches past the per-frame set budget are clamped (and logged), not an error.
    #[test]
    fn clamp_to_set_budget_caps_at_max() {
        assert_eq!(clamp_to_set_budget(10), 10, "under the cap is unchanged");
        assert_eq!(
            clamp_to_set_budget(SKIN_MAX_SETS_PER_FRAME as usize),
            SKIN_MAX_SETS_PER_FRAME as usize,
            "exactly at the cap is unchanged"
        );
        assert_eq!(
            clamp_to_set_budget(SKIN_MAX_SETS_PER_FRAME as usize + 50),
            SKIN_MAX_SETS_PER_FRAME as usize,
            "past the cap clamps to the budget"
        );
    }

    /// The skin compute kernel deforms a known bind pose with a known palette to a
    /// committed golden, validation-clean — the phase's GPU-runtime gate (llvmpipe runs
    /// compute). A translation joint matrix must shift every vertex position by exactly
    /// that translation (the kernel applies the skin matrix without the model matrix) and
    /// leave the normal + UV untouched, and the run must add no validation issues (the
    /// pipeline / descriptor-set wiring + the dispatch are real-GPU-valid).
    #[test]
    fn skin_kernel_deforms_to_golden_validation_clean() {
        use crate::device::SurfaceSource;
        use crate::pipelines::Pipelines;
        use crate::resources::BindlessFreeList;
        use crate::validation_issue_count;
        use ash::vk;
        use saffron_geometry::glam::{Vec2, Vec3};
        use std::sync::Mutex;

        let device = match Device::new(&SurfaceSource::Offscreen) {
            Ok(device) => device,
            Err(err) => {
                eprintln!("skipping: no Vulkan device obtainable ({err})");
                return;
            }
        };
        let before = validation_issue_count();

        let free_list: BindlessFreeList = Arc::new(Mutex::new(Vec::new()));
        let descriptors = crate::Descriptors::new(&device, &free_list).expect("Descriptors");
        let mut pipelines = Pipelines::new(&device, &descriptors, vk::SampleCountFlags::TYPE_1);
        let skinning = Skinning::new(&device).expect("Skinning");
        let skin_pso = request_skin_pipeline(&mut pipelines, &skinning).expect("skin PSO");

        // Three bind-pose vertices weighted fully on joint 0, a palette translating by
        // (10, 20, 30), and a host-visible deformed output we read back.
        let positions = [
            Vec3::new(1.0, 2.0, 3.0),
            Vec3::new(-1.0, 0.0, 5.0),
            Vec3::new(0.5, -2.5, 1.0),
        ];
        let normals = Vec3::new(0.0, 0.0, 1.0);
        let verts: Vec<Vertex> = positions
            .iter()
            .map(|&p| Vertex {
                position: p,
                normal: normals,
                uv0: Vec2::new(0.25, 0.75),
            })
            .collect();
        let skins = vec![
            saffron_geometry::VertexSkin {
                joints: [0, 0, 0, 0],
                weights: [1.0, 0.0, 0.0, 0.0],
            };
            3
        ];
        let translation = Vec3::new(10.0, 20.0, 30.0);
        let palette = [Mat4::from_translation(translation)];

        let resources = device.resources();
        let host = |bytes: &[u8], usage: vk::BufferUsageFlags| -> Buffer {
            let info = vk_mem::AllocationCreateInfo {
                usage: vk_mem::MemoryUsage::Auto,
                flags: vk_mem::AllocationCreateFlags::HOST_ACCESS_RANDOM
                    | vk_mem::AllocationCreateFlags::MAPPED,
                ..Default::default()
            };
            let mut buffer = Buffer::new(resources, bytes.len() as u64, usage, &info)
                .expect("host-visible buffer");
            buffer.mapped_bytes().expect("mapped")[..bytes.len()].copy_from_slice(bytes);
            buffer
        };
        let storage = vk::BufferUsageFlags::STORAGE_BUFFER;
        let in_verts = host(bytemuck::cast_slice(&verts), storage);
        let in_skins = host(bytemuck::cast_slice(&skins), storage);
        let in_palette = host(bytemuck::cast_slice(&palette), storage);
        let out_bytes = vec![0u8; verts.len() * size_of::<Vertex>()];
        let mut out_verts = host(&out_bytes, storage);

        // Wire the skin set against the four raw host buffers (the production path wires
        // it off a GpuMesh; here the bind-pose lives in plain host buffers), then dispatch.
        let set = alloc_and_wire_raw(
            resources.device(),
            skinning.frames[0].pool,
            skinning.set_layout,
            in_verts.handle(),
            in_skins.handle(),
            in_palette.handle(),
            in_palette.size(),
            out_verts.handle(),
            out_verts.size(),
        );

        dispatch_skin(&device, &skin_pso, set, verts.len() as u32);

        device.wait_idle().expect("idle after dispatch");
        let deformed: &[Vertex] = bytemuck::cast_slice(out_verts.mapped_bytes().expect("mapped"));
        for (i, src) in positions.iter().enumerate() {
            let got = deformed[i].position;
            let want = *src + translation;
            assert!(
                (got - want).length() < 1e-4,
                "vertex {i}: deformed {got:?} != golden {want:?}"
            );
            assert!(
                (deformed[i].normal - normals).length() < 1e-4,
                "vertex {i}: normal must survive the identity-rotation skin"
            );
            assert_eq!(
                deformed[i].uv0,
                Vec2::new(0.25, 0.75),
                "vertex {i}: uv passes through unchanged"
            );
        }

        drop(in_verts);
        drop(in_skins);
        drop(in_palette);
        drop(out_verts);
        drop(skin_pso);
        drop(skinning);
        drop(pipelines);
        drop(descriptors);
        drop(device);

        let after = validation_issue_count();
        assert_eq!(
            before,
            after,
            "the skin dispatch must be validation-clean (saw {} new issue(s))",
            after.saturating_sub(before)
        );
    }

    /// Allocates one skin set and writes the four raw storage buffers directly — the
    /// test-only sibling of [`wire_set`] that takes raw handles rather than a `GpuMesh`.
    #[allow(clippy::too_many_arguments)]
    fn alloc_and_wire_raw(
        raw: &ash::Device,
        pool: vk::DescriptorPool,
        layout: vk::DescriptorSetLayout,
        verts: vk::Buffer,
        skins: vk::Buffer,
        palette: vk::Buffer,
        palette_size: vk::DeviceSize,
        out: vk::Buffer,
        out_size: vk::DeviceSize,
    ) -> vk::DescriptorSet {
        let layouts = [layout];
        let info = vk::DescriptorSetAllocateInfo::default()
            .descriptor_pool(pool)
            .set_layouts(&layouts);
        // SAFETY: the ash seam. The layout outlives the call; the set lives until the pool
        // is reset / destroyed.
        let set = unsafe { raw.allocate_descriptor_sets(&info) }.expect("alloc skin set")[0];
        let whole = vk::WHOLE_SIZE;
        let infos = [
            vk::DescriptorBufferInfo {
                buffer: verts,
                offset: 0,
                range: whole,
            },
            vk::DescriptorBufferInfo {
                buffer: skins,
                offset: 0,
                range: whole,
            },
            vk::DescriptorBufferInfo {
                buffer: palette,
                offset: 0,
                range: palette_size,
            },
            vk::DescriptorBufferInfo {
                buffer: out,
                offset: 0,
                range: out_size,
            },
        ];
        let writes: Vec<vk::WriteDescriptorSet> = (0..4)
            .map(|b| {
                vk::WriteDescriptorSet::default()
                    .dst_set(set)
                    .dst_binding(b as u32)
                    .descriptor_type(vk::DescriptorType::STORAGE_BUFFER)
                    .buffer_info(std::slice::from_ref(&infos[b]))
            })
            .collect();
        // SAFETY: the ash seam. Each write targets a binding the layout declares.
        unsafe { raw.update_descriptor_sets(&writes, &[]) };
        set
    }

    /// Records the skin compute dispatch on a one-off command buffer and submits it,
    /// waiting the fence. The deformed buffer is host-visible (mapped), so no copy-out is
    /// needed; a `SHADER_WRITE → HOST_READ` barrier orders the read after the dispatch.
    fn dispatch_skin(
        device: &Device,
        pipeline: &Arc<crate::Pipeline>,
        set: vk::DescriptorSet,
        vertex_count: u32,
    ) {
        use ash::vk;
        let raw = device.resources().device();
        let pool_info =
            vk::CommandPoolCreateInfo::default().queue_family_index(device.graphics_queue_family);
        // SAFETY: the ash seam. Freed at the end.
        let pool = unsafe { raw.create_command_pool(&pool_info, None) }.expect("pool");
        let alloc = vk::CommandBufferAllocateInfo::default()
            .command_pool(pool)
            .level(vk::CommandBufferLevel::PRIMARY)
            .command_buffer_count(1);
        // SAFETY: the ash seam. One buffer from the pool.
        let cmd = unsafe { raw.allocate_command_buffers(&alloc) }.expect("cmd")[0];
        // SAFETY: the ash seam. Default fence.
        let fence =
            unsafe { raw.create_fence(&vk::FenceCreateInfo::default(), None) }.expect("fence");

        let begin = vk::CommandBufferBeginInfo::default()
            .flags(vk::CommandBufferUsageFlags::ONE_TIME_SUBMIT);
        let push = SkinPush {
            vertex_count,
            joint_offset: 0,
            deformed_offset: 0,
            pad: 0,
        };
        // SAFETY: the ash seam. The PSO/set are valid; the dispatch covers the vertices;
        // the host-read barrier orders the mapped read after the compute write.
        unsafe {
            raw.begin_command_buffer(cmd, &begin).expect("begin");
            raw.cmd_bind_pipeline(cmd, vk::PipelineBindPoint::COMPUTE, pipeline.handle());
            raw.cmd_bind_descriptor_sets(
                cmd,
                vk::PipelineBindPoint::COMPUTE,
                pipeline.layout(),
                0,
                &[set],
                &[],
            );
            raw.cmd_push_constants(
                cmd,
                pipeline.layout(),
                vk::ShaderStageFlags::COMPUTE,
                0,
                bytemuck::bytes_of(&push),
            );
            raw.cmd_dispatch(cmd, vertex_count.div_ceil(64), 1, 1);
            let mem = vk::MemoryBarrier2::default()
                .src_stage_mask(vk::PipelineStageFlags2::COMPUTE_SHADER)
                .src_access_mask(vk::AccessFlags2::SHADER_STORAGE_WRITE)
                .dst_stage_mask(vk::PipelineStageFlags2::HOST)
                .dst_access_mask(vk::AccessFlags2::HOST_READ);
            let barriers = [mem];
            let dep = vk::DependencyInfo::default().memory_barriers(&barriers);
            raw.cmd_pipeline_barrier2(cmd, &dep);
            raw.end_command_buffer(cmd).expect("end");
            let cmd_info = [vk::CommandBufferSubmitInfo::default().command_buffer(cmd)];
            let submit = [vk::SubmitInfo2::default().command_buffer_infos(&cmd_info)];
            raw.queue_submit2(device.graphics_queue, &submit, fence)
                .expect("submit");
            raw.wait_for_fences(&[fence], true, u64::MAX).expect("wait");
            raw.destroy_fence(fence, None);
            raw.destroy_command_pool(pool, None);
        }
    }
}
