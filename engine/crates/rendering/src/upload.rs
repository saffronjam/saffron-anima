//! The mesh and texture upload paths — the staging-copy-to-device-local sequences
//! that produce the [`Arc`]`<`[`GpuMesh`]`>` / [`Arc`]`<`[`GpuTexture`]`>` the asset
//! layer and scene draw consume.
//!
//! These hang off an [`Uploader`] that owns the one-off command pool and a clone of
//! the externally-synchronized [`GpuQueue`] — the README §5 first shared-mutable site,
//! so the worker thread can upload off the main thread.
//!
//! # The submit mutex (README §5)
//!
//! The graphics queue is externally synchronized: the frame loop and the thumbnail
//! worker both submit on it. So the queue lives behind [`GpuQueue`] (an
//! `Arc<Mutex<vk::Queue>>`), and the one-off submit here takes the lock for the
//! submit2 only — the fence wait is outside the lock so a long upload does not stall
//! a sibling submit. A command pool is *not* thread-safe, so an [`Uploader`] owns its
//! own pool; the worker thread builds its own [`Uploader`] with the same shared queue.

use std::sync::{Arc, Mutex};

use ash::vk;
use saffron_geometry::glam::Vec3;
use saffron_geometry::{Mesh, MorphData, MorphDelta, VertexSkin};
use vk_mem::Alloc;

use crate::descriptors::Descriptors;
use crate::resources::{
    DeviceResources, GpuMesh, GpuMeshParts, GpuTexture, GpuTextureParts, MorphBuffers,
};
use crate::{Device, Error, Result, checked};

/// The externally-synchronized graphics queue, shared behind a mutex.
///
/// README §5's first `Arc<Mutex>` site: the frame loop's submit/present and the worker
/// thread's upload submits all take this lock. Cloning the `Arc` hands a second
/// thread the same queue under the same lock.
#[derive(Clone)]
pub struct GpuQueue {
    inner: Arc<Mutex<vk::Queue>>,
}

// SAFETY: a `vk::Queue` is a raw handle; the `Mutex` provides the external
// synchronization Vulkan requires for queue submission. Sharing it across threads is
// exactly the README §5 contract (the worker thread submits uploads on it).
unsafe impl Send for GpuQueue {}
// SAFETY: as above — every access goes through the `Mutex`.
unsafe impl Sync for GpuQueue {}

impl GpuQueue {
    /// Wraps the device's graphics queue for shared, externally-synchronized use.
    pub fn new(queue: vk::Queue) -> Self {
        Self {
            inner: Arc::new(Mutex::new(queue)),
        }
    }

    /// Submits `submits` on the queue under the lock, signaling `fence`. The lock is
    /// held only for the submit call.
    ///
    /// # Errors
    ///
    /// Returns [`Error::Vk`] if `vkQueueSubmit2` fails.
    fn submit2(
        &self,
        raw: &ash::Device,
        submits: &[vk::SubmitInfo2<'_>],
        fence: vk::Fence,
    ) -> Result<()> {
        let queue = *self.inner.lock().expect("gpu queue mutex");
        // SAFETY: the ash seam. The queue is externally synchronized by the mutex held
        // here; the submit-infos + fence are valid for the call.
        checked(
            unsafe { raw.queue_submit2(queue, submits, fence) },
            "queue_submit2 (one-off)",
        )
    }
}

/// The one-off upload helper: a dedicated command pool plus the shared queue.
///
/// One [`Uploader`] per thread — Vulkan command pools are not thread-safe, so the
/// thumbnail worker constructs its own with a clone of the same [`GpuQueue`]. The
/// pool's buffers are short-lived (allocated, recorded, submitted, freed per call).
/// [`Drop`] frees the pool.
pub struct Uploader {
    resources: Arc<DeviceResources>,
    queue: GpuQueue,
    command_pool: vk::CommandPool,
    /// The acceleration-structure dispatch for building a per-mesh BLAS at upload time when
    /// RT is supported. `None` on a software device —
    /// the mesh's `blas` then stays `None` and the engine renders via the shadow-map path.
    accel: Option<ash::khr::acceleration_structure::Device>,
}

// SAFETY: the pool handle is owned by this `Uploader` and used only from the thread
// that holds it (one `Uploader` per thread); the `Arc`/`GpuQueue` are `Send`.
unsafe impl Send for Uploader {}

impl Uploader {
    /// Creates an uploader with its own one-off command pool on the graphics family.
    ///
    /// # Errors
    ///
    /// Returns [`Error::Vk`] if the command pool cannot be created.
    pub fn new(device: &Device, queue: &GpuQueue) -> Result<Self> {
        let info = vk::CommandPoolCreateInfo::default()
            .flags(vk::CommandPoolCreateFlags::TRANSIENT)
            .queue_family_index(device.graphics_queue_family);
        // SAFETY: the ash seam. The create-info is valid; the pool is owned and freed
        // in `Drop`.
        let command_pool = checked(
            unsafe { device.raw().create_command_pool(&info, None) },
            "create_command_pool (uploader)",
        )?;
        Ok(Self {
            resources: Arc::clone(device.resources()),
            queue: queue.clone(),
            command_pool,
            accel: device.accel_dispatch().cloned(),
        })
    }

    /// The ash device this uploader records against.
    fn raw(&self) -> &ash::Device {
        self.resources.device()
    }

    /// The VMA allocator this uploader stages through.
    fn allocator(&self) -> &vk_mem::Allocator {
        self.resources.allocator()
    }

    /// Allocates a primary one-off command buffer, records `record` into it, submits
    /// it on the shared queue, and blocks on a fresh fence (never `device.waitIdle`,
    /// which would drain the in-flight scene frame). Frees the buffer + fence.
    fn with_one_off_commands<R>(&self, record: R) -> Result<()>
    where
        R: FnOnce(vk::CommandBuffer),
    {
        let raw = self.raw();
        let alloc_info = vk::CommandBufferAllocateInfo::default()
            .command_pool(self.command_pool)
            .level(vk::CommandBufferLevel::PRIMARY)
            .command_buffer_count(1);
        // SAFETY: the ash seam. One primary buffer from this uploader's own pool.
        let cmd = checked(
            unsafe { raw.allocate_command_buffers(&alloc_info) },
            "allocate_command_buffers (one-off)",
        )?[0];

        let begin = vk::CommandBufferBeginInfo::default()
            .flags(vk::CommandBufferUsageFlags::ONE_TIME_SUBMIT);
        let recorded = (|| -> Result<()> {
            // SAFETY: the ash seam. Begin/record/end on the freshly allocated buffer.
            checked(
                unsafe { raw.begin_command_buffer(cmd, &begin) },
                "begin_command_buffer (one-off)",
            )?;
            record(cmd);
            // SAFETY: the ash seam. Ends the recording opened above.
            checked(
                unsafe { raw.end_command_buffer(cmd) },
                "end_command_buffer (one-off)",
            )?;
            self.submit_and_wait(cmd)
        })();

        // SAFETY: the ash seam. The submit fence was waited (or never submitted), so
        // the buffer is idle and freed exactly once.
        unsafe { raw.free_command_buffers(self.command_pool, &[cmd]) };
        recorded
    }

    /// Submits one already-recorded buffer on the shared queue with a fresh fence and
    /// waits on *its* completion. The submit takes the queue mutex; the wait does not.
    fn submit_and_wait(&self, cmd: vk::CommandBuffer) -> Result<()> {
        let raw = self.raw();
        // SAFETY: the ash seam. A default (unsignaled) fence, destroyed below.
        let fence = checked(
            unsafe { raw.create_fence(&vk::FenceCreateInfo::default(), None) },
            "create_fence (one-off)",
        )?;

        let cmd_info = vk::CommandBufferSubmitInfo::default().command_buffer(cmd);
        let cmd_infos = [cmd_info];
        let submit = vk::SubmitInfo2::default().command_buffer_infos(&cmd_infos);
        let submits = [submit];

        let result = self.queue.submit2(raw, &submits, fence).and_then(|()| {
            // SAFETY: the ash seam. The fence belongs to this device; the wait blocks
            // until the one-off submit completes.
            checked(
                unsafe { raw.wait_for_fences(&[fence], true, u64::MAX) },
                "wait_for_fences (one-off)",
            )
        });

        // SAFETY: the ash seam. The fence was waited (or the submit failed before
        // signaling it), so it is idle and destroyed exactly once.
        unsafe { raw.destroy_fence(fence, None) };
        result
    }

    /// Builds the per-mesh BLAS once (a synchronous one-off submit, like the upload copy)
    /// when RT is supported, returning a shared [`crate::AccelerationStructure`]. The build
    /// scratch is held across the submit then dropped. `None` on a software device.
    fn build_mesh_blas(
        &self,
        vertex_buffer: vk::Buffer,
        vertex_count: u32,
        index_buffer: vk::Buffer,
        index_count: u32,
    ) -> Result<Option<Arc<crate::AccelerationStructure>>> {
        let Some(dispatch) = self.accel.as_ref() else {
            return Ok(None);
        };
        if index_count < 3 {
            return Ok(None);
        }
        let raw = self.raw();
        let alloc_info = vk::CommandBufferAllocateInfo::default()
            .command_pool(self.command_pool)
            .level(vk::CommandBufferLevel::PRIMARY)
            .command_buffer_count(1);
        // SAFETY: the ash seam. One primary buffer from this uploader's own pool.
        let cmd = checked(
            unsafe { raw.allocate_command_buffers(&alloc_info) },
            "allocate_command_buffers (blas)",
        )?[0];
        let begin = vk::CommandBufferBeginInfo::default()
            .flags(vk::CommandBufferUsageFlags::ONE_TIME_SUBMIT);
        let built = (|| -> Result<crate::MeshBlasBuild> {
            // SAFETY: the ash seam. Begin the freshly allocated buffer.
            checked(
                unsafe { raw.begin_command_buffer(cmd, &begin) },
                "begin_command_buffer (blas)",
            )?;
            let build = crate::record_mesh_blas_build(
                &self.resources,
                dispatch,
                cmd,
                vertex_buffer,
                vertex_count,
                index_buffer,
                index_count,
            )?;
            // SAFETY: the ash seam. Ends the recording opened above.
            checked(
                unsafe { raw.end_command_buffer(cmd) },
                "end_command_buffer (blas)",
            )?;
            self.submit_and_wait(cmd)?;
            Ok(build)
        })();
        // SAFETY: the ash seam. The submit was waited (or never happened), so the buffer is
        // idle and freed exactly once.
        unsafe { raw.free_command_buffers(self.command_pool, &[cmd]) };
        // The scratch is no longer needed once the build submit completed; drop it.
        built.map(|build| {
            drop(build.scratch);
            Some(Arc::new(build.blas))
        })
    }

    /// Uploads a mesh's vertex + index streams (and the optional [`VertexSkin`]
    /// stream) into device-local buffers, returning a shared [`GpuMesh`].
    ///
    /// One staging buffer holds `[vertices | indices | skin]`; copies fan it out to
    /// the device-local buffers. The skin stream, when present, must parallel the
    /// vertices (one [`VertexSkin`] per vertex); it carries `STORAGE` usage too (the
    /// compute skinning prepass reads it).
    ///
    /// # Errors
    ///
    /// Returns [`Error::EmptyMesh`] for an empty mesh, [`Error::SkinMismatch`] when a
    /// skin stream does not parallel the vertices, or [`Error::Vk`] for a failing
    /// Vulkan/VMA call. Resources allocated before a failure are freed before return.
    pub fn upload_mesh(
        &self,
        mesh: &Mesh,
        skin: &[VertexSkin],
        morph: Option<&MorphData>,
    ) -> Result<Arc<GpuMesh>> {
        if mesh.vertices.is_empty() || mesh.indices.is_empty() {
            return Err(Error::EmptyMesh);
        }
        if !skin.is_empty() && skin.len() != mesh.vertices.len() {
            return Err(Error::SkinMismatch {
                skin: skin.len(),
                vertices: mesh.vertices.len(),
            });
        }

        let vertex_bytes = std::mem::size_of_val(mesh.vertices.as_slice()) as vk::DeviceSize;
        let index_bytes = std::mem::size_of_val(mesh.indices.as_slice()) as vk::DeviceSize;
        let skin_bytes = std::mem::size_of_val(skin) as vk::DeviceSize;

        // One staging buffer holds the three streams concatenated.
        let mut staging =
            StagingBuffer::new(self.allocator(), vertex_bytes + index_bytes + skin_bytes)?;
        {
            let bytes = staging.mapped_slice();
            let vb = vertex_bytes as usize;
            let ib = index_bytes as usize;
            bytes[..vb].copy_from_slice(bytemuck::cast_slice(&mesh.vertices));
            bytes[vb..vb + ib].copy_from_slice(bytemuck::cast_slice(&mesh.indices));
            if !skin.is_empty() {
                bytes[vb + ib..].copy_from_slice(bytemuck::cast_slice(skin));
            }
        }
        staging.flush();

        // Compute bounds + the retained CPU copies for triangle-precise picking.
        let mut bounds_min = Vec3::splat(f32::MAX);
        let mut bounds_max = Vec3::splat(f32::MIN);
        let mut cpu_positions = Vec::with_capacity(mesh.vertices.len());
        for vertex in &mesh.vertices {
            bounds_min = bounds_min.min(vertex.position);
            bounds_max = bounds_max.max(vertex.position);
            cpu_positions.push(vertex.position);
        }

        // When RT is on, the vertex/index buffers also feed BLAS builds: they need shader
        // device address + AS-build-input usage.
        let rt_usage = if self.accel.is_some() {
            vk::BufferUsageFlags::SHADER_DEVICE_ADDRESS
                | vk::BufferUsageFlags::ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_KHR
        } else {
            vk::BufferUsageFlags::empty()
        };

        // A skinned mesh's vertex + skin streams are also read as storage buffers by
        // the compute skinning prepass, so they carry STORAGE usage too.
        let mut vertex_usage = vk::BufferUsageFlags::VERTEX_BUFFER | rt_usage;
        if !skin.is_empty() {
            vertex_usage |= vk::BufferUsageFlags::STORAGE_BUFFER;
        }

        // Allocate the device-local buffers; on a later failure free the
        // already-allocated ones (a `GpuMesh` never partially owns the set). Each
        // allocation is uniquely owned (a VMA `Allocation` is not `Copy`), so they are
        // freed directly here rather than tracked by a copied handle.
        let allocator = self.allocator();
        let vertex = make_device_buffer(allocator, vertex_bytes, vertex_usage)?;
        let index = match make_device_buffer(
            allocator,
            index_bytes,
            vk::BufferUsageFlags::INDEX_BUFFER | rt_usage,
        ) {
            Ok(buf) => buf,
            Err(err) => {
                free_one(allocator, vertex);
                return Err(err);
            }
        };
        let skin_buf = if skin.is_empty() {
            None
        } else {
            match make_device_buffer(
                allocator,
                skin_bytes,
                vk::BufferUsageFlags::VERTEX_BUFFER | vk::BufferUsageFlags::STORAGE_BUFFER,
            ) {
                Ok(buf) => Some(buf),
                Err(err) => {
                    free_one(allocator, vertex);
                    free_one(allocator, index);
                    return Err(err);
                }
            }
        };

        // Record + submit the staging copies.
        let copy = self.with_one_off_commands(|cmd| {
            // SAFETY: the ash seam. The buffers outlive the submit-wait; the staging
            // buffer is the upload source.
            unsafe {
                let raw = self.raw();
                raw.cmd_copy_buffer(
                    cmd,
                    staging.handle(),
                    vertex.0,
                    &[vk::BufferCopy::default()
                        .src_offset(0)
                        .dst_offset(0)
                        .size(vertex_bytes)],
                );
                raw.cmd_copy_buffer(
                    cmd,
                    staging.handle(),
                    index.0,
                    &[vk::BufferCopy::default()
                        .src_offset(vertex_bytes)
                        .dst_offset(0)
                        .size(index_bytes)],
                );
                if let Some((skin_buffer, _)) = skin_buf {
                    raw.cmd_copy_buffer(
                        cmd,
                        staging.handle(),
                        skin_buffer,
                        &[vk::BufferCopy::default()
                            .src_offset(vertex_bytes + index_bytes)
                            .dst_offset(0)
                            .size(skin_bytes)],
                    );
                }
            }
        });
        drop(staging);
        if let Err(err) = copy {
            free_one(allocator, vertex);
            free_one(allocator, index);
            if let Some(buf) = skin_buf {
                free_one(allocator, buf);
            }
            return Err(err);
        }

        // Build this mesh's BLAS once (the RT geometry occlusion oracle) when RT is
        // available. A failure is logged, not fatal — the mesh renders without RT shadows.
        let blas = match self.build_mesh_blas(
            vertex.0,
            mesh.vertices.len() as u32,
            index.0,
            mesh.indices.len() as u32,
        ) {
            Ok(blas) => blas,
            Err(err) => {
                tracing::warn!("BLAS build failed: {err}");
                None
            }
        };

        // Build the morph buffers (flat delta array + per-target ranges) when the mesh
        // carries blend shapes; free the already-owned buffers on a morph-upload failure.
        let morph_buffers = match morph.filter(|m| !m.targets.is_empty()) {
            Some(data) => match self.upload_morph_buffers(data) {
                Ok(buffers) => Some(buffers),
                Err(err) => {
                    free_one(allocator, vertex);
                    free_one(allocator, index);
                    if let Some(buf) = skin_buf {
                        free_one(allocator, buf);
                    }
                    return Err(err);
                }
            },
            None => None,
        };

        let parts = GpuMeshParts {
            vertex,
            index,
            skin: skin_buf,
            morph: morph_buffers,
            index_count: mesh.indices.len() as u32,
            vertex_count: mesh.vertices.len() as u32,
            submeshes: mesh.submeshes.clone(),
            bounds_min,
            bounds_max,
            cpu_positions,
            cpu_indices: mesh.indices.clone(),
            cpu_skin: skin.to_vec(),
            blas,
        };
        Ok(Arc::new(GpuMesh::from_parts(&self.resources, parts)))
    }

    /// Builds the device-local morph buffers from [`MorphData`]: the flat `MorphDelta`
    /// array (each target's deltas concatenated) and the per-target `[first_delta,
    /// delta_count]` ranges, both `STORAGE` for the morph compute pass. Frees the delta
    /// buffer if the range buffer or the copy fails.
    fn upload_morph_buffers(&self, data: &MorphData) -> Result<MorphBuffers> {
        let mut deltas: Vec<MorphDelta> = Vec::new();
        let mut ranges: Vec<[u32; 2]> = Vec::with_capacity(data.targets.len());
        for target in &data.targets {
            let first = deltas.len() as u32;
            ranges.push([first, target.deltas.len() as u32]);
            deltas.extend_from_slice(&target.deltas);
        }
        let delta_count = deltas.len() as u32;
        let target_count = ranges.len() as u32;

        // Each device buffer must be non-empty even when a count is zero (a rest-only morph
        // mesh); pad to one record. The shader never reads past the real counts.
        let delta_used = std::mem::size_of_val(deltas.as_slice());
        let range_used = std::mem::size_of_val(ranges.as_slice());
        let delta_bytes = delta_used.max(std::mem::size_of::<MorphDelta>()) as vk::DeviceSize;
        let range_bytes = range_used.max(std::mem::size_of::<[u32; 2]>()) as vk::DeviceSize;

        let mut staging = StagingBuffer::new(self.allocator(), delta_bytes + range_bytes)?;
        {
            let bytes = staging.mapped_slice();
            if delta_used > 0 {
                bytes[..delta_used].copy_from_slice(bytemuck::cast_slice(&deltas));
            }
            if range_used > 0 {
                let base = delta_bytes as usize;
                bytes[base..base + range_used].copy_from_slice(bytemuck::cast_slice(&ranges));
            }
        }
        staging.flush();

        let allocator = self.allocator();
        let deltas_buf =
            make_device_buffer(allocator, delta_bytes, vk::BufferUsageFlags::STORAGE_BUFFER)?;
        let ranges_buf = match make_device_buffer(
            allocator,
            range_bytes,
            vk::BufferUsageFlags::STORAGE_BUFFER,
        ) {
            Ok(buf) => buf,
            Err(err) => {
                free_one(allocator, deltas_buf);
                return Err(err);
            }
        };

        let copy = self.with_one_off_commands(|cmd| {
            // SAFETY: the ash seam. Both device buffers outlive the submit-wait; the staging
            // buffer is the source.
            unsafe {
                let raw = self.raw();
                raw.cmd_copy_buffer(
                    cmd,
                    staging.handle(),
                    deltas_buf.0,
                    &[vk::BufferCopy::default().size(delta_bytes)],
                );
                raw.cmd_copy_buffer(
                    cmd,
                    staging.handle(),
                    ranges_buf.0,
                    &[vk::BufferCopy::default()
                        .src_offset(delta_bytes)
                        .dst_offset(0)
                        .size(range_bytes)],
                );
            }
        });
        drop(staging);
        if let Err(err) = copy {
            free_one(allocator, deltas_buf);
            free_one(allocator, ranges_buf);
            return Err(err);
        }

        Ok(MorphBuffers {
            deltas: deltas_buf,
            ranges: ranges_buf,
            cpu_ranges: ranges,
            target_count,
            delta_count,
        })
    }

    /// Uploads tightly packed RGBA8 pixels as a sampled, mipmapped texture in the
    /// bindless array, claiming a slot in `descriptors` and writing the view into it.
    ///
    /// `srgb` selects `R8G8B8A8_SRGB` (color) vs `R8G8B8A8_UNORM` (data). A full mip
    /// chain is generated by blitting down from mip 0.
    ///
    /// # Errors
    ///
    /// Returns [`Error::ZeroSizedImage`] for a zero extent or [`Error::Vk`] for a
    /// failing Vulkan/VMA call; allocated resources are freed before return on error.
    pub fn upload_texture(
        &self,
        descriptors: &Descriptors,
        rgba: &[u8],
        width: u32,
        height: u32,
        srgb: bool,
    ) -> Result<Arc<GpuTexture>> {
        if width == 0 || height == 0 {
            return Err(Error::ZeroSizedImage);
        }
        let bytes = (width as vk::DeviceSize) * (height as vk::DeviceSize) * 4;

        let mut staging = StagingBuffer::new(self.allocator(), bytes)?;
        staging.mapped_slice()[..bytes as usize].copy_from_slice(&rgba[..bytes as usize]);
        staging.flush();

        let mip_levels = mip_count(width, height);
        let format = if srgb {
            vk::Format::R8G8B8A8_SRGB
        } else {
            vk::Format::R8G8B8A8_UNORM
        };
        let uploaded = self.create_sampled_image(width, height, mip_levels, format)?;
        let image = uploaded.image;

        // Record the upload + mip generation; on failure free the image.
        let recorded = self.with_one_off_commands(|cmd| {
            // SAFETY: the ash seam. The image/staging buffer outlive the submit-wait.
            unsafe {
                record_texture_upload(
                    self.raw(),
                    cmd,
                    image,
                    staging.handle(),
                    width,
                    height,
                    mip_levels,
                )
            };
        });
        drop(staging);
        if let Err(err) = recorded {
            self.destroy_image(uploaded.image, uploaded.allocation);
            return Err(err);
        }

        self.finish_texture(descriptors, uploaded)
    }

    /// Uploads the 1×1 white RGBA8 texture and seeds it into *every* bindless slot,
    /// returning the [`GpuTexture`] the renderer holds for its lifetime.
    ///
    /// A material with no albedo/ORM texture indexes [`crate::DEFAULT_WHITE_SLOT`], so
    /// that slot must hold a valid view or sampling it faults on lavapipe and is
    /// undefined behaviour on real hardware. The white pixel makes the missing-texture
    /// factors pass through unchanged (white × factor = factor). The upload claims slot
    /// 0 (the first claim at init) via the normal path, then [`Descriptors::seed_all_textures`]
    /// fills every remaining slot so no descriptor in the partially-bound array is ever
    /// unbound.
    ///
    /// # Errors
    ///
    /// Returns [`Error::Vk`] for a failing Vulkan/VMA call during the texture upload.
    pub fn upload_default_white(&self, descriptors: &Descriptors) -> Result<Arc<GpuTexture>> {
        let white = self.upload_texture(descriptors, &[255u8, 255, 255, 255], 1, 1, false)?;
        descriptors.seed_all_textures(white.view());
        Ok(white)
    }

    /// Uploads tightly packed linear-float RGBA (`width*height*4` floats) as an
    /// `R16G16B16A16_SFLOAT` sampled texture in the bindless array, narrowing f32→f16
    /// on the CPU before staging (HDR panoramas / env sources). Single mip.
    ///
    /// # Errors
    ///
    /// Returns [`Error::ZeroSizedImage`] for a zero extent or [`Error::Vk`] for a
    /// failing Vulkan/VMA call; allocated resources are freed before return on error.
    pub fn upload_texture_float(
        &self,
        descriptors: &Descriptors,
        rgba: &[f32],
        width: u32,
        height: u32,
    ) -> Result<Arc<GpuTexture>> {
        if width == 0 || height == 0 {
            return Err(Error::ZeroSizedImage);
        }
        let texels = (width as usize) * (height as usize) * 4;
        let half: Vec<u16> = rgba[..texels].iter().copied().map(float_to_half).collect();
        let bytes = (texels * std::mem::size_of::<u16>()) as vk::DeviceSize;

        let mut staging = StagingBuffer::new(self.allocator(), bytes)?;
        staging.mapped_slice()[..bytes as usize].copy_from_slice(bytemuck::cast_slice(&half));
        staging.flush();

        let format = vk::Format::R16G16B16A16_SFLOAT;
        let uploaded = self.create_sampled_image(width, height, 1, format)?;
        let image = uploaded.image;

        let recorded = self.with_one_off_commands(|cmd| {
            // SAFETY: the ash seam. The image/staging buffer outlive the submit-wait.
            unsafe {
                let raw = self.raw();
                transition_image(
                    raw,
                    cmd,
                    image,
                    1,
                    vk::ImageLayout::UNDEFINED,
                    vk::ImageLayout::TRANSFER_DST_OPTIMAL,
                    vk::PipelineStageFlags2::TOP_OF_PIPE,
                    vk::AccessFlags2::empty(),
                    vk::PipelineStageFlags2::COPY,
                    vk::AccessFlags2::TRANSFER_WRITE,
                );
                copy_buffer_to_image(raw, cmd, staging.handle(), image, width, height);
                transition_image(
                    raw,
                    cmd,
                    image,
                    1,
                    vk::ImageLayout::TRANSFER_DST_OPTIMAL,
                    vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL,
                    vk::PipelineStageFlags2::COPY,
                    vk::AccessFlags2::TRANSFER_WRITE,
                    vk::PipelineStageFlags2::FRAGMENT_SHADER,
                    vk::AccessFlags2::SHADER_SAMPLED_READ,
                );
            }
        });
        drop(staging);
        if let Err(err) = recorded {
            self.destroy_image(uploaded.image, uploaded.allocation);
            return Err(err);
        }

        self.finish_texture(descriptors, uploaded)
    }

    /// Creates a device-local sampled image (`TRANSFER_DST | TRANSFER_SRC | SAMPLED`,
    /// dedicated memory), the shared image shape of both texture upload paths.
    fn create_sampled_image(
        &self,
        width: u32,
        height: u32,
        mip_levels: u32,
        format: vk::Format,
    ) -> Result<UploadedImage> {
        let info = vk::ImageCreateInfo::default()
            .image_type(vk::ImageType::TYPE_2D)
            .format(format)
            .extent(vk::Extent3D {
                width,
                height,
                depth: 1,
            })
            .mip_levels(mip_levels)
            .array_layers(1)
            .samples(vk::SampleCountFlags::TYPE_1)
            .tiling(vk::ImageTiling::OPTIMAL)
            .usage(
                vk::ImageUsageFlags::TRANSFER_DST
                    | vk::ImageUsageFlags::TRANSFER_SRC
                    | vk::ImageUsageFlags::SAMPLED,
            )
            .initial_layout(vk::ImageLayout::UNDEFINED);
        let alloc_info = vk_mem::AllocationCreateInfo {
            usage: vk_mem::MemoryUsage::Auto,
            flags: vk_mem::AllocationCreateFlags::DEDICATED_MEMORY,
            ..Default::default()
        };
        // SAFETY: the VMA seam. The create-infos are valid; the image is owned and
        // freed by the returned `GpuTexture` (or `destroy_image` on a later failure).
        let (image, allocation) = checked(
            unsafe { self.allocator().create_image(&info, &alloc_info) },
            "vmaCreateImage (texture)",
        )?;
        Ok(UploadedImage {
            image,
            allocation,
            width,
            height,
            format,
            mip_levels,
        })
    }

    /// Creates the sampled view, claims a bindless slot, writes the texture into the
    /// global set, and wraps the image as a [`GpuTexture`] owning that slot.
    fn finish_texture(
        &self,
        descriptors: &Descriptors,
        uploaded: UploadedImage,
    ) -> Result<Arc<GpuTexture>> {
        let UploadedImage {
            image,
            allocation,
            width,
            height,
            format,
            mip_levels,
        } = uploaded;
        let view_info = vk::ImageViewCreateInfo::default()
            .image(image)
            .view_type(vk::ImageViewType::TYPE_2D)
            .format(format)
            .subresource_range(vk::ImageSubresourceRange {
                aspect_mask: vk::ImageAspectFlags::COLOR,
                base_mip_level: 0,
                level_count: mip_levels,
                base_array_layer: 0,
                layer_count: 1,
            });
        // SAFETY: the ash seam. The view references the image just uploaded.
        let view = match unsafe { self.raw().create_image_view(&view_info, None) } {
            Ok(view) => view,
            Err(result) => {
                self.destroy_image(image, allocation);
                return Err(Error::Vk {
                    context: "create_image_view (texture)",
                    result,
                });
            }
        };

        // Claim a bindless slot (reusing a reclaimed one) and write the texture in.
        let index = descriptors.claim_slot();
        descriptors.write_texture(view, index);

        let texture = GpuTexture::from_parts(
            &self.resources,
            GpuTextureParts {
                image,
                view,
                allocation,
                bindless_index: index,
                extent: vk::Extent2D { width, height },
                format,
            },
            descriptors.free_list(),
        );
        Ok(Arc::new(texture))
    }

    /// Frees an image + its allocation directly (the error-path cleanup before a
    /// `GpuTexture` ever takes ownership).
    fn destroy_image(&self, image: vk::Image, mut allocation: vk_mem::Allocation) {
        // SAFETY: the VMA seam. The image was created on this allocator and not yet
        // owned by a `GpuTexture`; freed exactly once on the error path.
        unsafe { self.allocator().destroy_image(image, &mut allocation) };
    }
}

impl Drop for Uploader {
    fn drop(&mut self) {
        // SAFETY: the ash seam. All one-off buffers are freed per call (none in
        // flight); the pool is destroyed exactly once. The `Arc<DeviceResources>`
        // keeps the device alive for the call.
        unsafe {
            self.resources
                .device()
                .destroy_command_pool(self.command_pool, None);
        }
    }
}

/// A freshly created device-local sampled image awaiting its view + bindless slot —
/// the handoff from [`Uploader::create_sampled_image`] to
/// [`Uploader::finish_texture`]. Owns the image until `finish_texture` wraps it in a
/// [`GpuTexture`] (or an upload-recording failure frees it directly).
struct UploadedImage {
    image: vk::Image,
    allocation: vk_mem::Allocation,
    width: u32,
    height: u32,
    format: vk::Format,
    mip_levels: u32,
}

/// A host-visible, persistently mapped staging buffer that flushes and frees itself.
struct StagingBuffer<'a> {
    allocator: &'a vk_mem::Allocator,
    buffer: vk::Buffer,
    allocation: vk_mem::Allocation,
    mapped: *mut u8,
    size: vk::DeviceSize,
}

impl<'a> StagingBuffer<'a> {
    /// Allocates a `TRANSFER_SRC`, host-sequential-write, mapped buffer of `size`.
    fn new(allocator: &'a vk_mem::Allocator, size: vk::DeviceSize) -> Result<Self> {
        let info = vk::BufferCreateInfo::default()
            .size(size)
            .usage(vk::BufferUsageFlags::TRANSFER_SRC);
        let alloc_info = vk_mem::AllocationCreateInfo {
            usage: vk_mem::MemoryUsage::Auto,
            flags: vk_mem::AllocationCreateFlags::HOST_ACCESS_SEQUENTIAL_WRITE
                | vk_mem::AllocationCreateFlags::MAPPED,
            ..Default::default()
        };
        // SAFETY: the VMA seam. The create-infos are valid; the buffer is freed in
        // `Drop`.
        let (buffer, allocation) = checked(
            unsafe { allocator.create_buffer(&info, &alloc_info) },
            "vmaCreateBuffer (staging)",
        )?;
        let mapped = allocator
            .get_allocation_info(&allocation)
            .mapped_data
            .cast::<u8>();
        Ok(Self {
            allocator,
            buffer,
            allocation,
            mapped,
            size,
        })
    }

    fn handle(&self) -> vk::Buffer {
        self.buffer
    }

    /// The mapped staging memory as a writable byte slice (the upload source).
    fn mapped_slice(&mut self) -> &mut [u8] {
        // SAFETY: the allocation is HOST_VISIBLE + MAPPED for `size` bytes; the
        // `&mut self` borrow makes the slice exclusive.
        unsafe { std::slice::from_raw_parts_mut(self.mapped, self.size as usize) }
    }

    /// Flushes the mapped writes so the GPU copy sees them.
    fn flush(&self) {
        // The map may be coherent; the flush is a no-op then. Either way it flushes
        // the whole allocation.
        let _ = self
            .allocator
            .flush_allocation(&self.allocation, 0, self.size);
    }
}

impl Drop for StagingBuffer<'_> {
    fn drop(&mut self) {
        // SAFETY: the VMA seam. The staging buffer is freed exactly once after the
        // copy completed (the one-off submit was waited before this drop).
        unsafe {
            self.allocator
                .destroy_buffer(self.buffer, &mut self.allocation);
        }
    }
}

/// Allocates a device-local buffer (`size`, `usage | TRANSFER_DST`, auto memory).
fn make_device_buffer(
    allocator: &vk_mem::Allocator,
    size: vk::DeviceSize,
    usage: vk::BufferUsageFlags,
) -> Result<(vk::Buffer, vk_mem::Allocation)> {
    let info = vk::BufferCreateInfo::default()
        .size(size)
        .usage(usage | vk::BufferUsageFlags::TRANSFER_DST);
    let alloc_info = vk_mem::AllocationCreateInfo {
        usage: vk_mem::MemoryUsage::AutoPreferDevice,
        ..Default::default()
    };
    // SAFETY: the VMA seam. The create-infos are valid; ownership of the returned
    // buffer passes to the caller (the `GpuMesh`, or freed on the error path).
    checked(
        unsafe { allocator.create_buffer(&info, &alloc_info) },
        "vmaCreateBuffer (device)",
    )
}

/// Frees one device buffer directly — the mesh-upload error-path cleanup before a
/// `GpuMesh` takes ownership of the set.
fn free_one(allocator: &vk_mem::Allocator, buffer: (vk::Buffer, vk_mem::Allocation)) {
    let (handle, mut allocation) = buffer;
    // SAFETY: the VMA seam. The buffer was created on this allocator and not yet
    // owned by a `GpuMesh`; freed exactly once on the error path.
    unsafe { allocator.destroy_buffer(handle, &mut allocation) };
}

/// Full mip-chain length for a `width × height` image (down to 1×1).
fn mip_count(width: u32, height: u32) -> u32 {
    let mut d = width.max(height);
    let mut levels = 1;
    while d > 1 {
        d >>= 1;
        levels += 1;
    }
    levels
}

/// Records the RGBA8 upload + full mip-chain generation for `image` into `cmd`: all
/// mips → `TRANSFER_DST`, copy mip 0, blit down the chain, then every mip → shader
/// read.
///
/// # Safety
///
/// `cmd` must be in the recording state; `image` (with `mip_levels` mips) and `src`
/// must outlive the submit that consumes `cmd`.
unsafe fn record_texture_upload(
    raw: &ash::Device,
    cmd: vk::CommandBuffer,
    image: vk::Image,
    src: vk::Buffer,
    width: u32,
    height: u32,
    mip_levels: u32,
) {
    // All mips start TransferDst: mip 0 receives the copy, the rest receive blits.
    let to_dst = vk::ImageMemoryBarrier2::default()
        .src_stage_mask(vk::PipelineStageFlags2::TOP_OF_PIPE)
        .dst_stage_mask(vk::PipelineStageFlags2::COPY)
        .dst_access_mask(vk::AccessFlags2::TRANSFER_WRITE)
        .old_layout(vk::ImageLayout::UNDEFINED)
        .new_layout(vk::ImageLayout::TRANSFER_DST_OPTIMAL)
        .src_queue_family_index(vk::QUEUE_FAMILY_IGNORED)
        .dst_queue_family_index(vk::QUEUE_FAMILY_IGNORED)
        .image(image)
        .subresource_range(vk::ImageSubresourceRange {
            aspect_mask: vk::ImageAspectFlags::COLOR,
            base_mip_level: 0,
            level_count: mip_levels,
            base_array_layer: 0,
            layer_count: 1,
        });
    let to_dst = [to_dst];
    let dep = vk::DependencyInfo::default().image_memory_barriers(&to_dst);
    // SAFETY: the caller's recording contract; the image outlives the submit.
    unsafe { raw.cmd_pipeline_barrier2(cmd, &dep) };

    // SAFETY: as above; mip 0 is in TRANSFER_DST per the barrier.
    unsafe { copy_buffer_to_image(raw, cmd, src, image, width, height) };

    // SAFETY: as above; generates mips 1..n and transitions every level to read.
    unsafe { record_mip_chain(raw, cmd, image, width, height, mip_levels) };
}

/// Copies the whole of `src` into mip 0 of `image` (in `TRANSFER_DST`).
///
/// # Safety
///
/// `cmd` recording; `src`/`image` outlive the submit.
unsafe fn copy_buffer_to_image(
    raw: &ash::Device,
    cmd: vk::CommandBuffer,
    src: vk::Buffer,
    image: vk::Image,
    width: u32,
    height: u32,
) {
    let region = vk::BufferImageCopy::default()
        .image_subresource(vk::ImageSubresourceLayers {
            aspect_mask: vk::ImageAspectFlags::COLOR,
            mip_level: 0,
            base_array_layer: 0,
            layer_count: 1,
        })
        .image_extent(vk::Extent3D {
            width,
            height,
            depth: 1,
        });
    // SAFETY: the caller's recording contract.
    unsafe {
        raw.cmd_copy_buffer_to_image(
            cmd,
            src,
            image,
            vk::ImageLayout::TRANSFER_DST_OPTIMAL,
            &[region],
        );
    }
}

/// Generates mips 1..`mip_levels` by blitting down from mip 0, then transitions every
/// level to `SHADER_READ_ONLY_OPTIMAL`. On entry every level is `TRANSFER_DST`.
///
/// # Safety
///
/// `cmd` recording; `image` (with `mip_levels` mips) outlives the submit.
unsafe fn record_mip_chain(
    raw: &ash::Device,
    cmd: vk::CommandBuffer,
    image: vk::Image,
    width: u32,
    height: u32,
    mip_levels: u32,
) {
    let mut mw = width as i32;
    let mut mh = height as i32;
    for i in 1..mip_levels {
        // SAFETY: the caller's recording contract.
        unsafe {
            mip_barrier(
                raw,
                cmd,
                image,
                i - 1,
                vk::ImageLayout::TRANSFER_DST_OPTIMAL,
                vk::ImageLayout::TRANSFER_SRC_OPTIMAL,
                vk::PipelineStageFlags2::COPY,
                vk::AccessFlags2::TRANSFER_WRITE,
                vk::PipelineStageFlags2::BLIT,
                vk::AccessFlags2::TRANSFER_READ,
            );
        }
        let nw = if mw > 1 { mw / 2 } else { 1 };
        let nh = if mh > 1 { mh / 2 } else { 1 };
        let blit = vk::ImageBlit::default()
            .src_subresource(vk::ImageSubresourceLayers {
                aspect_mask: vk::ImageAspectFlags::COLOR,
                mip_level: i - 1,
                base_array_layer: 0,
                layer_count: 1,
            })
            .src_offsets([
                vk::Offset3D { x: 0, y: 0, z: 0 },
                vk::Offset3D { x: mw, y: mh, z: 1 },
            ])
            .dst_subresource(vk::ImageSubresourceLayers {
                aspect_mask: vk::ImageAspectFlags::COLOR,
                mip_level: i,
                base_array_layer: 0,
                layer_count: 1,
            })
            .dst_offsets([
                vk::Offset3D { x: 0, y: 0, z: 0 },
                vk::Offset3D { x: nw, y: nh, z: 1 },
            ]);
        // SAFETY: the caller's recording contract; the blit reads mip i-1 (SRC) and
        // writes mip i (DST).
        unsafe {
            raw.cmd_blit_image(
                cmd,
                image,
                vk::ImageLayout::TRANSFER_SRC_OPTIMAL,
                image,
                vk::ImageLayout::TRANSFER_DST_OPTIMAL,
                &[blit],
                vk::Filter::LINEAR,
            );
        }
        mw = nw;
        mh = nh;
    }
    for i in 0..mip_levels {
        let last = i == mip_levels - 1;
        // The last level only received a copy/blit-dst (TRANSFER_DST); every earlier
        // level was a blit source (TRANSFER_SRC), so its source stage/access differ.
        let (from_layout, src_stage, src_access) = if last {
            (
                vk::ImageLayout::TRANSFER_DST_OPTIMAL,
                vk::PipelineStageFlags2::COPY,
                vk::AccessFlags2::TRANSFER_WRITE,
            )
        } else {
            (
                vk::ImageLayout::TRANSFER_SRC_OPTIMAL,
                vk::PipelineStageFlags2::BLIT,
                vk::AccessFlags2::TRANSFER_READ,
            )
        };
        // SAFETY: the caller's recording contract.
        unsafe {
            mip_barrier(
                raw,
                cmd,
                image,
                i,
                from_layout,
                vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL,
                src_stage,
                src_access,
                vk::PipelineStageFlags2::FRAGMENT_SHADER,
                vk::AccessFlags2::SHADER_SAMPLED_READ,
            );
        }
    }
}

/// One sync2 image-memory barrier on a single mip level.
///
/// # Safety
///
/// `cmd` recording; `image` outlives the submit.
#[allow(clippy::too_many_arguments)]
unsafe fn mip_barrier(
    raw: &ash::Device,
    cmd: vk::CommandBuffer,
    image: vk::Image,
    mip: u32,
    from: vk::ImageLayout,
    to: vk::ImageLayout,
    src_stage: vk::PipelineStageFlags2,
    src_access: vk::AccessFlags2,
    dst_stage: vk::PipelineStageFlags2,
    dst_access: vk::AccessFlags2,
) {
    let barrier = vk::ImageMemoryBarrier2::default()
        .src_stage_mask(src_stage)
        .src_access_mask(src_access)
        .dst_stage_mask(dst_stage)
        .dst_access_mask(dst_access)
        .old_layout(from)
        .new_layout(to)
        .src_queue_family_index(vk::QUEUE_FAMILY_IGNORED)
        .dst_queue_family_index(vk::QUEUE_FAMILY_IGNORED)
        .image(image)
        .subresource_range(vk::ImageSubresourceRange {
            aspect_mask: vk::ImageAspectFlags::COLOR,
            base_mip_level: mip,
            level_count: 1,
            base_array_layer: 0,
            layer_count: 1,
        });
    let barriers = [barrier];
    let dep = vk::DependencyInfo::default().image_memory_barriers(&barriers);
    // SAFETY: the caller's recording contract.
    unsafe { raw.cmd_pipeline_barrier2(cmd, &dep) };
}

/// One whole-image sync2 layout transition (single mip range), used by the float
/// (single-mip) texture path.
///
/// # Safety
///
/// `cmd` recording; `image` outlives the submit.
#[allow(clippy::too_many_arguments)]
unsafe fn transition_image(
    raw: &ash::Device,
    cmd: vk::CommandBuffer,
    image: vk::Image,
    mip_levels: u32,
    from: vk::ImageLayout,
    to: vk::ImageLayout,
    src_stage: vk::PipelineStageFlags2,
    src_access: vk::AccessFlags2,
    dst_stage: vk::PipelineStageFlags2,
    dst_access: vk::AccessFlags2,
) {
    let barrier = vk::ImageMemoryBarrier2::default()
        .src_stage_mask(src_stage)
        .src_access_mask(src_access)
        .dst_stage_mask(dst_stage)
        .dst_access_mask(dst_access)
        .old_layout(from)
        .new_layout(to)
        .src_queue_family_index(vk::QUEUE_FAMILY_IGNORED)
        .dst_queue_family_index(vk::QUEUE_FAMILY_IGNORED)
        .image(image)
        .subresource_range(vk::ImageSubresourceRange {
            aspect_mask: vk::ImageAspectFlags::COLOR,
            base_mip_level: 0,
            level_count: mip_levels,
            base_array_layer: 0,
            layer_count: 1,
        });
    let barriers = [barrier];
    let dep = vk::DependencyInfo::default().image_memory_barriers(&barriers);
    // SAFETY: the caller's recording contract.
    unsafe { raw.cmd_pipeline_barrier2(cmd, &dep) };
}

/// Narrows one finite f32 to an IEEE binary16 (round-to-nearest-even). Subnormals are
/// flushed where the source underflows; finite magnitudes above the f16 max saturate
/// to ±inf, matching what the GPU produces sampling an f16 texture.
pub(crate) fn float_to_half(value: f32) -> u16 {
    let mut bits = value.to_bits();
    let sign = ((bits >> 16) & 0x8000) as u16;
    bits &= 0x7fff_ffff;
    if bits >= 0x7f80_0000 {
        // inf / nan: keep nan non-zero so it stays nan.
        let mant: u16 = if bits > 0x7f80_0000 { 0x0200 } else { 0 };
        return sign | 0x7c00 | mant;
    }
    if bits >= 0x4780_0000 {
        return sign | 0x7c00; // overflow -> inf
    }
    if bits < 0x3880_0000 {
        // subnormal/zero in f16: round the value scaled into the denormal range.
        let mant = (bits & 0x007f_ffff) | 0x0080_0000;
        let shift = 113_i32 - (bits >> 23) as i32;
        let rounded = if shift < 24 { mant >> shift } else { 0 };
        let half = (rounded + 0x0000_0fff + ((rounded >> 13) & 1)) >> 13;
        return sign | half as u16;
    }
    let rebiased = bits.wrapping_add(0xc800_0000); // exponent rebias (127 -> 15)
    let rounded = (rebiased + 0x0000_0fff + ((rebiased >> 13) & 1)) >> 13;
    sign | rounded as u16
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::device::SurfaceSource;
    use crate::resources::BindlessFreeList;
    use crate::validation_issue_count;
    use saffron_geometry::glam::{Vec2, Vec3};
    use saffron_geometry::{Submesh, Vertex};
    use std::sync::Mutex;

    /// Builds a headless device or skips the test (no Vulkan ICD in this toolbox).
    fn device_or_skip() -> Option<Device> {
        match Device::new(&SurfaceSource::Offscreen) {
            Ok(device) => Some(device),
            Err(err) => {
                eprintln!("skipping: no Vulkan device obtainable ({err})");
                None
            }
        }
    }

    /// A single-triangle mesh, the minimal valid upload input.
    fn triangle() -> Mesh {
        let v = |x: f32, y: f32| Vertex {
            position: Vec3::new(x, y, 0.0),
            normal: Vec3::new(0.0, 0.0, 1.0),
            uv0: Vec2::ZERO,
        };
        Mesh {
            vertices: vec![v(-1.0, -1.0), v(1.0, -1.0), v(0.0, 1.0)],
            indices: vec![0, 1, 2],
            submeshes: vec![Submesh {
                first_index: 0,
                index_count: 3,
                vertex_offset: 0,
                material_slot: 0,
            }],
        }
    }

    /// Uploading a mesh with a skin stream produces a `GpuMesh` with a non-null skin
    /// buffer; uploading without one leaves it null — the phase's named skin gate. The
    /// upload runs the real staging→device-local copy on the queue, validation-clean.
    /// Skips when no Vulkan device is present.
    #[test]
    fn upload_mesh_skin_buffer_presence_tracks_the_stream() {
        let Some(device) = device_or_skip() else {
            return;
        };
        let before = validation_issue_count();
        let queue = GpuQueue::new(device.graphics_queue);
        let uploader = Uploader::new(&device, &queue).expect("Uploader::new");
        let mesh = triangle();

        let plain = uploader
            .upload_mesh(&mesh, &[], None)
            .expect("unskinned upload");
        assert_eq!(plain.index_count, 3);
        assert_eq!(plain.vertex_count, 3);
        assert!(
            plain.skin_buffer().is_none(),
            "no skin stream → null skin buffer"
        );
        assert_eq!(plain.cpu_indices, mesh.indices);
        assert_eq!(plain.cpu_positions.len(), 3);

        let skin = vec![VertexSkin::default(); mesh.vertices.len()];
        let skinned = uploader
            .upload_mesh(&mesh, &skin, None)
            .expect("skinned upload");
        assert!(
            skinned.skin_buffer().is_some(),
            "a parallel skin stream → non-null skin buffer"
        );
        assert_eq!(skinned.cpu_skin.len(), 3);

        // A mismatched skin stream is rejected before any allocation.
        let bad = uploader.upload_mesh(&mesh, &[VertexSkin::default()], None);
        assert!(matches!(bad, Err(Error::SkinMismatch { .. })));

        drop(plain);
        drop(skinned);
        drop(uploader);
        device.wait_idle().expect("idle before teardown");
        drop(device);

        let after = validation_issue_count();
        assert_eq!(
            before,
            after,
            "the mesh uploads must be validation-clean (saw {} new issue(s))",
            after.saturating_sub(before)
        );
    }

    /// Uploading an RGBA8 texture (with a full mip chain) and an HDR float texture
    /// each claim a bindless slot, write the view into the global set, and are
    /// validation-clean — the phase's GPU upload smoke. The texture drop returns its
    /// slot to the shared free-list. Skips when no Vulkan device is present.
    #[test]
    fn upload_texture_paths_are_validation_clean() {
        let Some(device) = device_or_skip() else {
            return;
        };
        let before = validation_issue_count();
        let free_list: BindlessFreeList = Arc::new(Mutex::new(Vec::new()));
        let descriptors = Descriptors::new(&device, &free_list).expect("Descriptors::new");
        let queue = GpuQueue::new(device.graphics_queue);
        let uploader = Uploader::new(&device, &queue).expect("Uploader::new");

        // A 4×4 sRGB image: mip 0 + a blitted-down chain (mip_count(4,4) == 3).
        let rgba = vec![200u8; 4 * 4 * 4];
        let tex = uploader
            .upload_texture(&descriptors, &rgba, 4, 4, true)
            .expect("rgba8 upload");
        assert_eq!(tex.extent.width, 4);
        assert_eq!(tex.format, vk::Format::R8G8B8A8_SRGB);
        // Slot 0 is the default white; the first uploaded texture takes slot 1.
        assert_eq!(tex.bindless_index(), 1);

        // A 2×2 HDR float image → R16G16B16A16_SFLOAT, single mip.
        let hdr = vec![2.0f32; 2 * 2 * 4];
        let hdr_tex = uploader
            .upload_texture_float(&descriptors, &hdr, 2, 2)
            .expect("float upload");
        assert_eq!(hdr_tex.format, vk::Format::R16G16B16A16_SFLOAT);
        assert_eq!(hdr_tex.bindless_index(), 2);

        // A zero-sized image is rejected before any allocation.
        assert!(matches!(
            uploader.upload_texture(&descriptors, &rgba, 0, 4, false),
            Err(Error::ZeroSizedImage)
        ));

        // Dropping a texture returns its slot to the shared free-list.
        let slot = hdr_tex.bindless_index();
        drop(hdr_tex);
        assert_eq!(free_list.lock().unwrap().as_slice(), &[slot]);

        drop(tex);
        drop(uploader);
        drop(descriptors);
        device.wait_idle().expect("idle before teardown");
        drop(device);

        let after = validation_issue_count();
        assert_eq!(
            before,
            after,
            "the texture uploads must be validation-clean (saw {} new issue(s))",
            after.saturating_sub(before)
        );
    }

    /// The mip-chain length: 1×1 → 1, square powers of two → log2+1, and a non-square
    /// image uses the larger dimension.
    #[test]
    fn mip_count_matches_cpp() {
        assert_eq!(mip_count(1, 1), 1);
        assert_eq!(mip_count(2, 2), 2);
        assert_eq!(mip_count(256, 256), 9);
        assert_eq!(mip_count(1024, 512), 11);
        assert_eq!(mip_count(512, 1024), 11);
        assert_eq!(mip_count(7, 1), 3); // 7 -> 3 -> 1
    }

    /// `float_to_half` reproduces the known IEEE half encodings: the exact
    /// representables, the f16-max overflow to +inf, and the sign bit. This is
    /// the load-bearing half of `upload_texture_float` (a wrong narrowing corrupts
    /// every HDR env source) and runs on any host.
    #[test]
    fn float_to_half_matches_known_encodings() {
        assert_eq!(float_to_half(0.0), 0x0000);
        assert_eq!(float_to_half(-0.0), 0x8000);
        assert_eq!(float_to_half(1.0), 0x3c00);
        assert_eq!(float_to_half(2.0), 0x4000);
        assert_eq!(float_to_half(0.5), 0x3800);
        assert_eq!(float_to_half(-1.0), 0xbc00);
        // The largest finite half (65504.0) is exactly representable.
        assert_eq!(float_to_half(65504.0), 0x7bff);
        // Above the f16 max saturates to +inf; a real inf stays inf.
        assert_eq!(float_to_half(1.0e30), 0x7c00);
        assert_eq!(float_to_half(f32::INFINITY), 0x7c00);
        assert_eq!(float_to_half(f32::NEG_INFINITY), 0xfc00);
        // NaN stays NaN (a non-zero mantissa with the inf exponent).
        let nan = float_to_half(f32::NAN);
        assert_eq!(nan & 0x7c00, 0x7c00);
        assert_ne!(nan & 0x03ff, 0, "NaN keeps a non-zero mantissa");
    }
}
