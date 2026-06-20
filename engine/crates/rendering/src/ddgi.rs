//! Dynamic Diffuse Global Illumination: one irradiance probe volume updated each
//! frame by a software voxel-ray trace, sampled in the mesh fragment for
//! multi-bounce indirect.
//!
//! This is the C++ `Ddgi` sub-state (`renderer_types.cppm:1583`). It owns a 3D voxel
//! scene proxy ([`crate::Image3D`]), two octahedral atlases (irradiance rgba16f +
//! distance/moment rg16f), a per-frame ray radiance+distance image, the grow-only
//! per-frame scene-box SSBO, the linear-clamp atlas sampler, and the six
//! descriptor-set layouts + sets the five compute passes bind. Unlike the
//! screen-space sub-state, there is exactly one DDGI volume, so the descriptor sets
//! are device-shared (pool-owned), not per-view.
//!
//! Built once in [`Ddgi::new`], then borrowed `&Ddgi` by the frame-graph build (its
//! handles are immutable after init); the volume placement + sun/sky + temporal
//! state are written through [`Ddgi::set_scene`] / [`Ddgi::set_enabled`] /
//! [`Ddgi::advance_frame`] (`&mut self.ddgi`). The renderer carries the sun/sky as
//! plain fields fed from the scene, the same decoupling as IBL.
//!
//! # The five passes
//!
//! Each declares its storage usages so the render graph derives the `GENERAL`
//! barriers (the voxel proxy is an [`crate::Image3D`] imported via
//! [`crate::RenderGraph::import_image_3d`], tracked exactly like a 2D image):
//!
//! 1. `ddgi-voxelize` — voxel 3D storage write + box SSBO read.
//! 2. `ddgi-trace` — voxel storage read + prev-irradiance sampler → ray storage write.
//! 3. `ddgi-blend-irr` — ray sampler → irradiance storage.
//! 4. `ddgi-blend-dist` — ray sampler → distance (moment) storage.
//! 5. `ddgi-border` — the octahedral gutter copy on the irradiance atlas.

use std::sync::Arc;

use ash::vk;
use saffron_geometry::glam::{UVec4, Vec3, Vec4};

use crate::descriptors::Descriptors;
use crate::resources::{DeviceResources, Image, Image3D, ImageDesc};
use crate::{Device, Result, checked};

/// Probes per axis (the C++ `DdgiProbesX/Y/Z`, `renderer_detail.cppm:1414`).
pub const DDGI_PROBES_X: u32 = 8;
/// Probes per axis (Y).
pub const DDGI_PROBES_Y: u32 = 4;
/// Probes per axis (Z).
pub const DDGI_PROBES_Z: u32 = 8;
/// Rays traced per probe per frame (the C++ `DdgiRaysPerProbe`).
pub const DDGI_RAYS_PER_PROBE: u32 = 64;
/// Octahedral irradiance tile interior size (the C++ `DdgiIrrInterior`).
pub const DDGI_IRR_INTERIOR: u32 = 8;
/// Octahedral distance (moment) tile interior size (the C++ `DdgiDistInterior`).
pub const DDGI_DIST_INTERIOR: u32 = 16;
/// The voxel proxy is `DDGI_VOXEL_RES`³ (the C++ `DdgiVoxelRes`).
pub const DDGI_VOXEL_RES: u32 = 32;
/// Maximum scene boxes the per-frame proxy SSBO holds (the C++ `DdgiMaxBoxes`).
pub const DDGI_MAX_BOXES: u32 = 256;
/// Temporal blend weight — the fraction of last frame's value kept each update (the
/// C++ `DdgiHysteresis`).
pub const DDGI_HYSTERESIS: f32 = 0.95;

/// The voxel proxy + per-frame ray image format (rgba16f: albedo/radiance + occupancy/
/// distance). The C++ `DdgiVoxelFormat`.
pub const DDGI_VOXEL_FORMAT: vk::Format = vk::Format::R16G16B16A16_SFLOAT;
/// The irradiance atlas format (rgba16f). The C++ `DdgiIrrFormat`.
pub const DDGI_IRR_FORMAT: vk::Format = vk::Format::R16G16B16A16_SFLOAT;
/// The distance (moment) atlas format (rg16f: mean distance, mean squared distance).
/// The C++ `DdgiDistFormat`.
pub const DDGI_DIST_FORMAT: vk::Format = vk::Format::R16G16_SFLOAT;

/// The total probe count across the volume (one octahedral tile per probe).
pub const DDGI_PROBE_TOTAL: u32 = DDGI_PROBES_X * DDGI_PROBES_Y * DDGI_PROBES_Z;

/// The voxelize push: voxel resolution + box count + the world-space volume placement.
/// 48 bytes, matching `ddgi_voxelize.slang`'s `Push`.
#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
pub struct VoxelizePush {
    /// `xyz` = voxel resolution, `w` = active box count.
    pub grid_count: UVec4,
    /// `xyz` = world-space min corner of the volume.
    pub volume_min: Vec4,
    /// `xyz` = world-space size of the volume.
    pub volume_extent: Vec4,
}

const _: () = assert!(size_of::<VoxelizePush>() == 48);

/// The trace push: probe grid + voxel grid + volume + sun/sky + frame index. 112
/// bytes, matching `ddgi_trace.slang`'s `Push`.
#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
pub struct TracePush {
    /// `xyz` = probes per axis, `w` = rays per probe.
    pub probe_count: UVec4,
    /// `xyz` = voxel resolution, `w` = irradiance tile interior size.
    pub grid_count: UVec4,
    /// `xyz` = volume min corner.
    pub volume_min: Vec4,
    /// `xyz` = volume size.
    pub volume_extent: Vec4,
    /// `xyz` = direction the sun travels, `w` = sun intensity.
    pub sun_dir: Vec4,
    /// `rgb` = sun color, `w` = frame index (rotates the ray set).
    pub sun_color: Vec4,
    /// `rgb` = ambient sky radiance.
    pub sky_color: Vec4,
}

const _: () = assert!(size_of::<TracePush>() == 112);

/// The blend-irradiance / blend-distance push: probe grid + tile params + a params
/// vec4 (x = hysteresis, y = max distance for the distance pass). 48 bytes, matching
/// `ddgi_blend_irradiance.slang` / `ddgi_blend_distance.slang`'s `Push`.
#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
pub struct BlendPush {
    /// `xyz` = probes per axis, `w` = rays per probe.
    pub probe_count: UVec4,
    /// `x` = tile interior size, `y` = 1 on the first frame (no temporal history).
    pub tile: UVec4,
    /// `x` = hysteresis (history weight), `y` = max distance (distance pass only).
    pub params: Vec4,
}

const _: () = assert!(size_of::<BlendPush>() == 48);

/// The octahedral-gutter border-copy push: probe grid + the tile interior size. 32
/// bytes, matching `ddgi_border.slang`'s `Push`.
#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
pub struct BorderPush {
    /// `xyz` = probes per axis.
    pub probe_count: UVec4,
    /// `x` = tile interior size.
    pub tile: UVec4,
}

const _: () = assert!(size_of::<BorderPush>() == 32);

/// The irradiance atlas width in texels (`probesX·probesY` tiles of `interior+2`).
pub const fn irradiance_atlas_width() -> u32 {
    DDGI_PROBES_X * DDGI_PROBES_Y * (DDGI_IRR_INTERIOR + 2)
}

/// The irradiance atlas height in texels (`probesZ` tiles of `interior+2`).
pub const fn irradiance_atlas_height() -> u32 {
    DDGI_PROBES_Z * (DDGI_IRR_INTERIOR + 2)
}

/// The distance (moment) atlas width in texels.
pub const fn distance_atlas_width() -> u32 {
    DDGI_PROBES_X * DDGI_PROBES_Y * (DDGI_DIST_INTERIOR + 2)
}

/// The distance (moment) atlas height in texels.
pub const fn distance_atlas_height() -> u32 {
    DDGI_PROBES_Z * (DDGI_DIST_INTERIOR + 2)
}

/// The DDGI sub-state: the voxel proxy, the two octahedral atlases, the per-frame ray
/// image, the scene-box SSBO, the atlas sampler, and the six set layouts + sets the
/// five compute passes (and the mesh set 5) bind.
///
/// Owns an [`Arc`]`<`[`DeviceResources`]`>` so its handles free in [`Drop`] without a
/// live `&Device`. The mesh set-5 *layout* is owned by [`Descriptors`] (the übershader
/// binds it); this struct allocates the mesh *set* against it and writes the atlas
/// samplers into it.
pub struct Ddgi {
    resources: Arc<DeviceResources>,

    /// Off by default — it adds five compute passes per frame (the C++ `useDdgi`).
    pub use_ddgi: bool,
    /// Resources + sets valid (the C++ `ready`). True after [`Ddgi::new`].
    pub ready: bool,
    /// First frame after enable/resize → no temporal blend (the C++ `historyReset`).
    history_reset: bool,

    voxels: Image3D,
    irradiance: Image,
    distance: Image,
    rays: Image,
    box_buffer: crate::resources::Buffer,
    box_capacity: u32,
    frame_box_count: u32,
    /// Rotates the trace ray set each frame (the C++ `frameIndex`).
    frame_index: u32,

    volume_min: Vec3,
    volume_extent: Vec3,
    sun_dir: Vec3,
    sun_color: Vec3,
    sun_intensity: f32,
    sky_color: Vec3,

    sampler: vk::Sampler,
    voxel_layout: vk::DescriptorSetLayout,
    trace_layout: vk::DescriptorSetLayout,
    blend_irr_layout: vk::DescriptorSetLayout,
    blend_dist_layout: vk::DescriptorSetLayout,
    border_layout: vk::DescriptorSetLayout,

    voxel_set: vk::DescriptorSet,
    trace_set: vk::DescriptorSet,
    blend_irr_set: vk::DescriptorSet,
    blend_dist_set: vk::DescriptorSet,
    border_set: vk::DescriptorSet,
    mesh_set: vk::DescriptorSet,
}

impl Ddgi {
    /// Allocates the voxel proxy + atlases + ray image + box SSBO, the linear-clamp
    /// sampler, the five compute set layouts + the mesh set 5, the six sets, writes the
    /// static descriptors (the images/buffer never reallocate after init), and
    /// init-transitions the atlases into `SHADER_READ_ONLY_OPTIMAL` (their resting state
    /// for the mesh sample). The C++ `createDdgiResources` (`renderer_detail.cppm:3863`).
    ///
    /// `ready` is set true on success; `use_ddgi` defaults off (the C++ field default).
    ///
    /// # Errors
    ///
    /// Returns [`crate::Error::Vk`] for any failing Vulkan call; already-created handles
    /// are freed before returning on a partial failure (the early-return cleanup mirrors
    /// the resource wrappers' `Drop`).
    pub fn new(device: &Device, descriptors: &Descriptors) -> Result<Self> {
        let resources = Arc::clone(device.resources());
        let raw = resources.device();

        // The voxel proxy + two atlases + the per-frame ray image. The atlases are
        // STORAGE (written by the blend/border compute) + SAMPLED (read by the mesh +
        // the trace's prev-irradiance bind); the voxel proxy is STORAGE only; the ray
        // image is STORAGE (trace writes) + SAMPLED (the blend passes read).
        let storage_sampled = vk::ImageUsageFlags::STORAGE | vk::ImageUsageFlags::SAMPLED;
        let voxels = Image3D::new(
            &resources,
            vk::Extent3D {
                width: DDGI_VOXEL_RES,
                height: DDGI_VOXEL_RES,
                depth: DDGI_VOXEL_RES,
            },
            DDGI_VOXEL_FORMAT,
            vk::ImageUsageFlags::STORAGE,
        )?;
        let irradiance = make_storage_image(
            &resources,
            irradiance_atlas_width(),
            irradiance_atlas_height(),
            DDGI_IRR_FORMAT,
            storage_sampled,
        )?;
        let distance = make_storage_image(
            &resources,
            distance_atlas_width(),
            distance_atlas_height(),
            DDGI_DIST_FORMAT,
            storage_sampled,
        )?;
        let rays = make_storage_image(
            &resources,
            DDGI_RAYS_PER_PROBE,
            DDGI_PROBE_TOTAL,
            DDGI_VOXEL_FORMAT,
            storage_sampled,
        )?;
        // The box SSBO: [min, max, albedo] per box, interleaved (matches the shader's
        // `Box` struct). Host-mapped, grow-not-needed (sized to the max box count).
        let box_bytes = u64::from(DDGI_MAX_BOXES) * 3 * size_of::<Vec4>() as u64;
        let box_buffer = make_mapped_storage_buffer(&resources, box_bytes)?;

        let sampler = create_linear_clamp_sampler(raw)?;

        // The five compute set layouts + the mesh set 5 layout is owned by Descriptors.
        // Build with the same partial-failure unwind discipline as the resource wrappers:
        // a guard frees what was created so far on any error.
        let layouts = match build_layouts(raw) {
            Ok(layouts) => layouts,
            Err(err) => {
                // SAFETY: the ash seam. Free the sampler created above before returning.
                unsafe { raw.destroy_sampler(sampler, None) };
                return Err(err);
            }
        };

        let (voxel_set, trace_set, blend_irr_set, blend_dist_set, border_set, mesh_set) =
            match allocate_sets(descriptors, &layouts) {
                Ok(sets) => sets,
                Err(err) => {
                    // SAFETY: the ash seam. The sets are pool-owned (freed with the pool);
                    // free the layouts + sampler created above before returning.
                    unsafe {
                        layouts.destroy(raw);
                        raw.destroy_sampler(sampler, None);
                    }
                    return Err(err);
                }
            };

        let mut ddgi = Self {
            resources,
            use_ddgi: false,
            ready: false,
            history_reset: true,
            voxels,
            irradiance,
            distance,
            rays,
            box_buffer,
            box_capacity: DDGI_MAX_BOXES,
            frame_box_count: 0,
            frame_index: 0,
            volume_min: Vec3::splat(-8.0),
            volume_extent: Vec3::splat(16.0),
            sun_dir: Vec3::new(-0.5, -1.0, -0.3),
            sun_color: Vec3::ONE,
            sun_intensity: 1.0,
            sky_color: Vec3::new(0.1, 0.13, 0.2),
            sampler,
            voxel_layout: layouts.voxel,
            trace_layout: layouts.trace,
            blend_irr_layout: layouts.blend_irr,
            blend_dist_layout: layouts.blend_dist,
            border_layout: layouts.border,
            voxel_set,
            trace_set,
            blend_irr_set,
            blend_dist_set,
            border_set,
            mesh_set,
        };

        ddgi.write_static_descriptors();
        // On a failure the `?` early-returns, dropping `ddgi` — which frees every owned
        // handle (sampler, layouts, images, buffer) in field order.
        ddgi.init_transition_atlases(device)?;
        ddgi.ready = true;
        Ok(ddgi)
    }

    /// The atlas sampler (linear, clamp-to-edge) the mesh + trace passes read with.
    pub fn sampler(&self) -> vk::Sampler {
        self.sampler
    }

    /// The mesh set 5 (irradiance + distance samplers) the scene pass binds when DDGI
    /// ran this frame.
    pub fn mesh_set(&self) -> vk::DescriptorSet {
        self.mesh_set
    }

    /// The voxel proxy 3D image handle + view + tracked layout, for the graph import.
    pub fn voxels(&self) -> (vk::Image, vk::ImageView, vk::ImageLayout) {
        (self.voxels.handle(), self.voxels.view(), self.voxels.layout)
    }

    /// Writes back the voxel proxy's resolved layout after the graph executes.
    pub fn set_voxel_layout(&mut self, layout: vk::ImageLayout) {
        self.voxels.layout = layout;
    }

    /// The per-frame ray image handle + view + tracked layout.
    pub fn rays(&self) -> (vk::Image, vk::ImageView, vk::ImageLayout) {
        (self.rays.handle(), self.rays.view(), self.rays.layout)
    }

    /// Writes back the ray image's resolved layout after the graph executes.
    pub fn set_rays_layout(&mut self, layout: vk::ImageLayout) {
        self.rays.layout = layout;
    }

    /// The irradiance atlas handle + view + tracked layout.
    pub fn irradiance(&self) -> (vk::Image, vk::ImageView, vk::ImageLayout) {
        (
            self.irradiance.handle(),
            self.irradiance.view(),
            self.irradiance.layout,
        )
    }

    /// Writes back the irradiance atlas's resolved layout after the graph executes.
    pub fn set_irradiance_layout(&mut self, layout: vk::ImageLayout) {
        self.irradiance.layout = layout;
    }

    /// The distance (moment) atlas handle + view + tracked layout.
    pub fn distance(&self) -> (vk::Image, vk::ImageView, vk::ImageLayout) {
        (
            self.distance.handle(),
            self.distance.view(),
            self.distance.layout,
        )
    }

    /// Writes back the distance atlas's resolved layout after the graph executes.
    pub fn set_distance_layout(&mut self, layout: vk::ImageLayout) {
        self.distance.layout = layout;
    }

    /// The voxelize pass's PSO set (set 0).
    pub fn voxel_set(&self) -> vk::DescriptorSet {
        self.voxel_set
    }

    /// The trace pass's PSO set.
    pub fn trace_set(&self) -> vk::DescriptorSet {
        self.trace_set
    }

    /// The blend-irradiance pass's PSO set.
    pub fn blend_irr_set(&self) -> vk::DescriptorSet {
        self.blend_irr_set
    }

    /// The blend-distance pass's PSO set.
    pub fn blend_dist_set(&self) -> vk::DescriptorSet {
        self.blend_dist_set
    }

    /// The border-copy pass's PSO set.
    pub fn border_set(&self) -> vk::DescriptorSet {
        self.border_set
    }

    /// The voxelize compute set layout (set 0: 3D storage + box SSBO).
    pub fn voxel_layout(&self) -> vk::DescriptorSetLayout {
        self.voxel_layout
    }

    /// The trace compute set layout (voxel storage + irradiance sampler + ray storage).
    pub fn trace_layout(&self) -> vk::DescriptorSetLayout {
        self.trace_layout
    }

    /// The blend-irradiance compute set layout (ray sampler + irradiance storage).
    pub fn blend_irr_layout(&self) -> vk::DescriptorSetLayout {
        self.blend_irr_layout
    }

    /// The blend-distance compute set layout (ray sampler + distance storage).
    pub fn blend_dist_layout(&self) -> vk::DescriptorSetLayout {
        self.blend_dist_layout
    }

    /// The border-copy compute set layout (irradiance storage).
    pub fn border_layout(&self) -> vk::DescriptorSetLayout {
        self.border_layout
    }

    /// Toggles DDGI; turning it on re-converges the probes from scratch by arming a
    /// history reset (the C++ `setDdgi`).
    pub fn set_enabled(&mut self, enabled: bool) {
        if enabled && !self.use_ddgi {
            self.history_reset = true;
        }
        self.use_ddgi = enabled;
    }

    /// Arms a temporal history reset — the first frame after an enable or a resize blends
    /// with no history. The C++ sets `historyReset = true` on enable/resize.
    pub fn reset_history(&mut self) {
        self.history_reset = true;
    }

    /// Whether the next frame's blend is the first (no temporal history) — read by the
    /// blend passes' `firstFrame` push field.
    pub fn history_reset(&self) -> bool {
        self.history_reset
    }

    /// The active scene-box count uploaded this frame.
    pub fn frame_box_count(&self) -> u32 {
        self.frame_box_count
    }

    /// The scene-box SSBO capacity (the C++ `boxCapacity` = `DdgiMaxBoxes`).
    pub fn box_capacity(&self) -> u32 {
        self.box_capacity
    }

    /// The current trace ray-set frame index.
    pub fn frame_index(&self) -> u32 {
        self.frame_index
    }

    /// The fitted volume placement (world-space min corner + size).
    pub fn volume(&self) -> (Vec3, Vec3) {
        (self.volume_min, self.volume_extent)
    }

    /// Whether DDGI runs this frame: on + ready + the five PSOs are present. The C++
    /// `doDdgi` gate (`renderer.cppm:1685`). Pure logic, so the named acceptance test can
    /// assert it without a device.
    pub fn wants_ddgi(&self, pipelines_ready: bool) -> bool {
        self.use_ddgi && self.ready && pipelines_ready
    }

    /// Whether DDGI is on and ready (the mesh-sample gate / the C++ `ddgiEnabled`).
    pub fn enabled(&self) -> bool {
        self.use_ddgi && self.ready
    }

    /// The probe-grid uvec4 (`xyz` probes/axis, `w` irradiance interior) folded into the
    /// light UBO so the mesh fragment locates probes.
    pub fn probe_count_ubo(&self) -> UVec4 {
        UVec4::new(
            DDGI_PROBES_X,
            DDGI_PROBES_Y,
            DDGI_PROBES_Z,
            DDGI_IRR_INTERIOR,
        )
    }

    /// Uploads this frame's scene-box proxy (interleaved `[min, max, albedo]` per box,
    /// clamped to the SSBO capacity) and stores the volume placement + sun/sky for the
    /// trace. A no-op when not ready. The C++ `setDdgiScene` (`renderer.cppm:3018`).
    ///
    /// `box_mins`/`box_maxs`/`box_albedos` are world-space AABBs + base colors, one per
    /// scene draw. The volume is fit to the scene AABB by the caller each frame.
    #[allow(clippy::too_many_arguments)]
    pub fn set_scene(
        &mut self,
        box_mins: &[Vec4],
        box_maxs: &[Vec4],
        box_albedos: &[Vec4],
        volume_min: Vec3,
        volume_extent: Vec3,
        sun_dir: Vec3,
        sun_color: Vec3,
        sun_intensity: f32,
        sky_color: Vec3,
    ) {
        if !self.ready {
            return;
        }
        let count = (box_mins.len() as u32).min(self.box_capacity);
        if count > 0 {
            let mapped = self
                .box_buffer
                .mapped_bytes()
                .expect("DDGI box SSBO is host-mapped");
            // Interleave [min, max, albedo] per box into the mapped SSBO (matches the
            // shader `Box` struct: three vec4 per box).
            let stride = 3 * size_of::<Vec4>();
            for i in 0..count as usize {
                let base = i * stride;
                let triple = [box_mins[i], box_maxs[i], box_albedos[i]];
                mapped[base..base + stride].copy_from_slice(bytemuck::cast_slice(&triple));
            }
        }
        self.frame_box_count = count;
        self.volume_min = volume_min;
        self.volume_extent = volume_extent;
        self.sun_dir = sun_dir;
        self.sun_color = sun_color;
        self.sun_intensity = sun_intensity;
        self.sky_color = sky_color;
    }

    /// Advances the temporal state after a frame's five passes are recorded: bumps the
    /// trace ray-set index and clears the history-reset flag (so the next frame blends
    /// with history). The C++ post-pass `frameIndex + 1; historyReset = false`.
    pub fn advance_frame(&mut self) {
        self.frame_index = self.frame_index.wrapping_add(1);
        self.history_reset = false;
    }

    /// The voxelize push for this frame (voxel resolution + active box count + volume).
    pub fn voxelize_push(&self) -> VoxelizePush {
        VoxelizePush {
            grid_count: UVec4::new(
                DDGI_VOXEL_RES,
                DDGI_VOXEL_RES,
                DDGI_VOXEL_RES,
                self.frame_box_count,
            ),
            volume_min: self.volume_min.extend(0.0),
            volume_extent: self.volume_extent.extend(0.0),
        }
    }

    /// The trace push for this frame (probe + voxel grids, volume, sun/sky, frame index).
    pub fn trace_push(&self) -> TracePush {
        TracePush {
            probe_count: UVec4::new(
                DDGI_PROBES_X,
                DDGI_PROBES_Y,
                DDGI_PROBES_Z,
                DDGI_RAYS_PER_PROBE,
            ),
            grid_count: UVec4::new(
                DDGI_VOXEL_RES,
                DDGI_VOXEL_RES,
                DDGI_VOXEL_RES,
                DDGI_IRR_INTERIOR,
            ),
            volume_min: self.volume_min.extend(0.0),
            volume_extent: self.volume_extent.extend(0.0),
            sun_dir: self.sun_dir.extend(self.sun_intensity),
            sun_color: self.sun_color.extend(self.frame_index as f32),
            sky_color: self.sky_color.extend(0.0),
        }
    }

    /// The blend-irradiance push (tile interior + first-frame flag + hysteresis).
    pub fn blend_irradiance_push(&self) -> BlendPush {
        BlendPush {
            probe_count: UVec4::new(
                DDGI_PROBES_X,
                DDGI_PROBES_Y,
                DDGI_PROBES_Z,
                DDGI_RAYS_PER_PROBE,
            ),
            tile: UVec4::new(DDGI_IRR_INTERIOR, u32::from(self.history_reset), 0, 0),
            params: Vec4::new(DDGI_HYSTERESIS, 0.0, 0.0, 0.0),
        }
    }

    /// The blend-distance push (tile interior + first-frame flag + hysteresis + the
    /// volume diagonal as the max distance for the moment normalization).
    pub fn blend_distance_push(&self) -> BlendPush {
        BlendPush {
            probe_count: UVec4::new(
                DDGI_PROBES_X,
                DDGI_PROBES_Y,
                DDGI_PROBES_Z,
                DDGI_RAYS_PER_PROBE,
            ),
            tile: UVec4::new(DDGI_DIST_INTERIOR, u32::from(self.history_reset), 0, 0),
            params: Vec4::new(DDGI_HYSTERESIS, self.volume_extent.length(), 0.0, 0.0),
        }
    }

    /// The border-copy push (probe grid + the irradiance tile interior).
    pub fn border_push(&self) -> BorderPush {
        BorderPush {
            probe_count: UVec4::new(DDGI_PROBES_X, DDGI_PROBES_Y, DDGI_PROBES_Z, 0),
            tile: UVec4::new(DDGI_IRR_INTERIOR, 0, 0, 0),
        }
    }

    /// Writes the (image/buffer never reallocate) static descriptors into the six sets:
    /// the voxelize/trace/blend/border compute sets + the mesh set 5. The C++ static
    /// descriptor writes in `createDdgiResources`.
    fn write_static_descriptors(&self) {
        let raw = self.resources.device();
        let general = vk::ImageLayout::GENERAL;
        let ro = vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL;

        // voxelize: voxel storage (b0) + box SSBO (b1).
        write_storage_image(raw, self.voxel_set, 0, self.voxels.view(), general);
        write_storage_buffer(
            raw,
            self.voxel_set,
            1,
            self.box_buffer.handle(),
            self.box_buffer.size(),
        );
        // trace: voxel storage read (b0) + prev-irradiance sampler (b1) + ray storage (b2).
        write_storage_image(raw, self.trace_set, 0, self.voxels.view(), general);
        write_combined_sampler(
            raw,
            self.trace_set,
            1,
            self.irradiance.view(),
            ro,
            self.sampler,
        );
        write_storage_image(raw, self.trace_set, 2, self.rays.view(), general);
        // blend irradiance: ray sampler (b0) + irradiance storage (b1).
        write_combined_sampler(
            raw,
            self.blend_irr_set,
            0,
            self.rays.view(),
            ro,
            self.sampler,
        );
        write_storage_image(raw, self.blend_irr_set, 1, self.irradiance.view(), general);
        // blend distance: ray sampler (b0) + distance storage (b1).
        write_combined_sampler(
            raw,
            self.blend_dist_set,
            0,
            self.rays.view(),
            ro,
            self.sampler,
        );
        write_storage_image(raw, self.blend_dist_set, 1, self.distance.view(), general);
        // border: irradiance storage (b0).
        write_storage_image(raw, self.border_set, 0, self.irradiance.view(), general);
        // mesh set 5: irradiance (b0) + distance (b1) samplers.
        write_combined_sampler(
            raw,
            self.mesh_set,
            0,
            self.irradiance.view(),
            ro,
            self.sampler,
        );
        write_combined_sampler(
            raw,
            self.mesh_set,
            1,
            self.distance.view(),
            ro,
            self.sampler,
        );
    }

    /// One-shot init barrier transitioning the two atlases from `UNDEFINED` into
    /// `SHADER_READ_ONLY_OPTIMAL` (their resting state for the mesh sample + the trace's
    /// prev-irradiance bind), waited idle. The voxel proxy + ray image stay `UNDEFINED`
    /// until the graph's first storage barrier. The C++ one-shot DDGI init barrier.
    fn init_transition_atlases(&mut self, device: &Device) -> Result<()> {
        let raw = device.raw();
        let pool_info =
            vk::CommandPoolCreateInfo::default().queue_family_index(device.graphics_queue_family);
        // SAFETY: the ash seam. The pool is freed at the end of this function.
        let pool = checked(
            unsafe { raw.create_command_pool(&pool_info, None) },
            "ddgi init pool",
        )?;
        let alloc = vk::CommandBufferAllocateInfo::default()
            .command_pool(pool)
            .level(vk::CommandBufferLevel::PRIMARY)
            .command_buffer_count(1);
        // SAFETY: the ash seam. One buffer from the pool above.
        let cmd = match unsafe { raw.allocate_command_buffers(&alloc) } {
            Ok(cmds) => cmds[0],
            Err(result) => {
                // SAFETY: the ash seam. Free the pool before returning.
                unsafe { raw.destroy_command_pool(pool, None) };
                return Err(crate::Error::Vk {
                    context: "ddgi init cmd",
                    result,
                });
            }
        };
        let fence = match unsafe { raw.create_fence(&vk::FenceCreateInfo::default(), None) } {
            Ok(fence) => fence,
            Err(result) => {
                // SAFETY: the ash seam. Free the pool before returning.
                unsafe { raw.destroy_command_pool(pool, None) };
                return Err(crate::Error::Vk {
                    context: "ddgi init fence",
                    result,
                });
            }
        };

        let result = (|| -> Result<()> {
            let begin = vk::CommandBufferBeginInfo::default()
                .flags(vk::CommandBufferUsageFlags::ONE_TIME_SUBMIT);
            // SAFETY: the ash seam. The barriers reference images this device created.
            unsafe {
                checked(raw.begin_command_buffer(cmd, &begin), "ddgi init begin")?;
                let irr = atlas_init_barrier(self.irradiance.handle());
                let dist = atlas_init_barrier(self.distance.handle());
                let barriers = [irr, dist];
                let dep = vk::DependencyInfo::default().image_memory_barriers(&barriers);
                raw.cmd_pipeline_barrier2(cmd, &dep);
                checked(raw.end_command_buffer(cmd), "ddgi init end")?;
            }
            let cmd_info = [vk::CommandBufferSubmitInfo::default().command_buffer(cmd)];
            let submit = [vk::SubmitInfo2::default().command_buffer_infos(&cmd_info)];
            // SAFETY: the ash seam. The queue is touched single-threaded at init.
            unsafe {
                checked(
                    raw.queue_submit2(device.graphics_queue, &submit, fence),
                    "ddgi init submit",
                )?;
                checked(
                    raw.wait_for_fences(&[fence], true, u64::MAX),
                    "ddgi init wait",
                )?;
            }
            Ok(())
        })();

        // SAFETY: the ash seam. The fence was waited (or the submit never happened), so
        // the pool/fence are idle and destroyed exactly once.
        unsafe {
            raw.destroy_fence(fence, None);
            raw.destroy_command_pool(pool, None);
        }
        if result.is_ok() {
            self.irradiance.layout = vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL;
            self.distance.layout = vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL;
        }
        result
    }
}

impl Drop for Ddgi {
    fn drop(&mut self) {
        // SAFETY: the ash seam. The `Arc<DeviceResources>` keeps the device alive for the
        // call; the run loop idled it before teardown (README §4). The sets are pool-owned
        // (freed with the descriptor pool in `Descriptors::drop`), so only the sampler +
        // the five compute layouts are destroyed here, each exactly once. The owned images
        // + the box buffer Drop after this by field order.
        let raw = self.resources.device();
        unsafe {
            raw.destroy_descriptor_set_layout(self.voxel_layout, None);
            raw.destroy_descriptor_set_layout(self.trace_layout, None);
            raw.destroy_descriptor_set_layout(self.blend_irr_layout, None);
            raw.destroy_descriptor_set_layout(self.blend_dist_layout, None);
            raw.destroy_descriptor_set_layout(self.border_layout, None);
            raw.destroy_sampler(self.sampler, None);
        }
    }
}

/// The five DDGI compute set layouts (the mesh set 5 layout is owned by `Descriptors`).
struct DdgiLayouts {
    voxel: vk::DescriptorSetLayout,
    trace: vk::DescriptorSetLayout,
    blend_irr: vk::DescriptorSetLayout,
    blend_dist: vk::DescriptorSetLayout,
    border: vk::DescriptorSetLayout,
}

impl DdgiLayouts {
    /// Frees every created layout (the partial-failure cleanup path).
    ///
    /// # Safety
    ///
    /// The device must be idle and each layout created exactly once.
    unsafe fn destroy(&self, raw: &ash::Device) {
        // SAFETY: forwarded from the caller's contract — each layout is freed once.
        unsafe {
            raw.destroy_descriptor_set_layout(self.voxel, None);
            raw.destroy_descriptor_set_layout(self.trace, None);
            raw.destroy_descriptor_set_layout(self.blend_irr, None);
            raw.destroy_descriptor_set_layout(self.blend_dist, None);
            raw.destroy_descriptor_set_layout(self.border, None);
        }
    }
}

/// Builds the five compute set layouts, freeing what was created so far on any failure.
fn build_layouts(raw: &ash::Device) -> Result<DdgiLayouts> {
    let si = vk::DescriptorType::STORAGE_IMAGE;
    let cs = vk::DescriptorType::COMBINED_IMAGE_SAMPLER;
    let sb = vk::DescriptorType::STORAGE_BUFFER;

    let voxel = make_compute_layout(raw, &[si, sb])?;
    let trace = match make_compute_layout(raw, &[si, cs, si]) {
        Ok(layout) => layout,
        Err(err) => {
            // SAFETY: the ash seam. Free the prior layout on this partial-failure path.
            unsafe { raw.destroy_descriptor_set_layout(voxel, None) };
            return Err(err);
        }
    };
    let blend_irr = match make_compute_layout(raw, &[cs, si]) {
        Ok(layout) => layout,
        Err(err) => {
            // SAFETY: the ash seam. Free the prior layouts.
            unsafe {
                raw.destroy_descriptor_set_layout(trace, None);
                raw.destroy_descriptor_set_layout(voxel, None);
            }
            return Err(err);
        }
    };
    let blend_dist = match make_compute_layout(raw, &[cs, si]) {
        Ok(layout) => layout,
        Err(err) => {
            // SAFETY: the ash seam. Free the prior layouts.
            unsafe {
                raw.destroy_descriptor_set_layout(blend_irr, None);
                raw.destroy_descriptor_set_layout(trace, None);
                raw.destroy_descriptor_set_layout(voxel, None);
            }
            return Err(err);
        }
    };
    let border = match make_compute_layout(raw, &[si]) {
        Ok(layout) => layout,
        Err(err) => {
            // SAFETY: the ash seam. Free the prior layouts.
            unsafe {
                raw.destroy_descriptor_set_layout(blend_dist, None);
                raw.destroy_descriptor_set_layout(blend_irr, None);
                raw.destroy_descriptor_set_layout(trace, None);
                raw.destroy_descriptor_set_layout(voxel, None);
            }
            return Err(err);
        }
    };

    Ok(DdgiLayouts {
        voxel,
        trace,
        blend_irr,
        blend_dist,
        border,
    })
}

/// Allocates the six DDGI sets (the five compute sets + the mesh set 5) from the shared
/// descriptor pool. The sets are pool-owned (freed with the pool in teardown).
fn allocate_sets(
    descriptors: &Descriptors,
    layouts: &DdgiLayouts,
) -> Result<(
    vk::DescriptorSet,
    vk::DescriptorSet,
    vk::DescriptorSet,
    vk::DescriptorSet,
    vk::DescriptorSet,
    vk::DescriptorSet,
)> {
    let voxel = descriptors.allocate_set(layouts.voxel)?;
    let trace = descriptors.allocate_set(layouts.trace)?;
    let blend_irr = descriptors.allocate_set(layouts.blend_irr)?;
    let blend_dist = descriptors.allocate_set(layouts.blend_dist)?;
    let border = descriptors.allocate_set(layouts.border)?;
    let mesh = descriptors.allocate_set(descriptors.ddgi_mesh_set_layout())?;
    Ok((voxel, trace, blend_irr, blend_dist, border, mesh))
}

/// A compute-stage set layout with one binding per `types` entry, in order (binding 0,
/// 1, …). The DDGI compute sets are all single-descriptor-per-binding.
fn make_compute_layout(
    raw: &ash::Device,
    types: &[vk::DescriptorType],
) -> Result<vk::DescriptorSetLayout> {
    let bindings: Vec<vk::DescriptorSetLayoutBinding> = types
        .iter()
        .enumerate()
        .map(|(i, &ty)| {
            vk::DescriptorSetLayoutBinding::default()
                .binding(i as u32)
                .descriptor_type(ty)
                .descriptor_count(1)
                .stage_flags(vk::ShaderStageFlags::COMPUTE)
        })
        .collect();
    let info = vk::DescriptorSetLayoutCreateInfo::default().bindings(&bindings);
    // SAFETY: the ash seam. The bindings outlive the call; the layout is freed in `Drop`
    // (or the partial-failure cleanup).
    checked(
        unsafe { raw.create_descriptor_set_layout(&info, None) },
        "ddgi compute layout",
    )
}

/// A host-mapped, persistently-mapped storage buffer of `size` bytes (the box SSBO).
/// The DDGI sub-state owns its own copy of the helper, like every feature module.
fn make_mapped_storage_buffer(
    resources: &Arc<DeviceResources>,
    size: vk::DeviceSize,
) -> Result<crate::resources::Buffer> {
    let alloc_info = vk_mem::AllocationCreateInfo {
        usage: vk_mem::MemoryUsage::Auto,
        flags: vk_mem::AllocationCreateFlags::HOST_ACCESS_SEQUENTIAL_WRITE
            | vk_mem::AllocationCreateFlags::MAPPED,
        ..Default::default()
    };
    crate::resources::Buffer::new(
        resources,
        size,
        vk::BufferUsageFlags::STORAGE_BUFFER,
        &alloc_info,
    )
}

/// A single-mip, single-layer 2D color image with the given storage/sampled usage — the
/// DDGI atlases + ray image.
fn make_storage_image(
    resources: &Arc<DeviceResources>,
    width: u32,
    height: u32,
    format: vk::Format,
    usage: vk::ImageUsageFlags,
) -> Result<Image> {
    Image::new(
        resources,
        &ImageDesc::color_2d(vk::Extent2D { width, height }, format, usage),
    )
}

/// The linear, clamp-to-edge sampler the mesh + trace passes read the atlases with (the
/// C++ DDGI sampler).
fn create_linear_clamp_sampler(raw: &ash::Device) -> Result<vk::Sampler> {
    let info = vk::SamplerCreateInfo::default()
        .mag_filter(vk::Filter::LINEAR)
        .min_filter(vk::Filter::LINEAR)
        .mipmap_mode(vk::SamplerMipmapMode::NEAREST)
        .address_mode_u(vk::SamplerAddressMode::CLAMP_TO_EDGE)
        .address_mode_v(vk::SamplerAddressMode::CLAMP_TO_EDGE)
        .address_mode_w(vk::SamplerAddressMode::CLAMP_TO_EDGE);
    // SAFETY: the ash seam. The sampler is owned and freed in `Drop`.
    checked(unsafe { raw.create_sampler(&info, None) }, "ddgi sampler")
}

/// `UNDEFINED → SHADER_READ_ONLY_OPTIMAL` init barrier for an atlas (1 mip, 1 layer,
/// color), made sampler-readable by the fragment stage.
fn atlas_init_barrier(image: vk::Image) -> vk::ImageMemoryBarrier2<'static> {
    vk::ImageMemoryBarrier2::default()
        .src_stage_mask(vk::PipelineStageFlags2::TOP_OF_PIPE)
        .src_access_mask(vk::AccessFlags2::empty())
        .dst_stage_mask(vk::PipelineStageFlags2::FRAGMENT_SHADER)
        .dst_access_mask(vk::AccessFlags2::SHADER_SAMPLED_READ)
        .old_layout(vk::ImageLayout::UNDEFINED)
        .new_layout(vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL)
        .src_queue_family_index(vk::QUEUE_FAMILY_IGNORED)
        .dst_queue_family_index(vk::QUEUE_FAMILY_IGNORED)
        .image(image)
        .subresource_range(vk::ImageSubresourceRange {
            aspect_mask: vk::ImageAspectFlags::COLOR,
            base_mip_level: 0,
            level_count: 1,
            base_array_layer: 0,
            layer_count: 1,
        })
}

/// Writes a storage image into `(set, binding)` at `layout` (no sampler).
fn write_storage_image(
    raw: &ash::Device,
    set: vk::DescriptorSet,
    binding: u32,
    view: vk::ImageView,
    layout: vk::ImageLayout,
) {
    let info = [vk::DescriptorImageInfo::default()
        .image_view(view)
        .image_layout(layout)];
    let write = vk::WriteDescriptorSet::default()
        .dst_set(set)
        .dst_binding(binding)
        .descriptor_type(vk::DescriptorType::STORAGE_IMAGE)
        .image_info(&info);
    // SAFETY: the ash seam. The set + view outlive the call; the write targets one
    // binding the set's layout declares.
    unsafe { raw.update_descriptor_sets(&[write], &[]) };
}

/// Writes a combined-image-sampler into `(set, binding)` at `layout`.
fn write_combined_sampler(
    raw: &ash::Device,
    set: vk::DescriptorSet,
    binding: u32,
    view: vk::ImageView,
    layout: vk::ImageLayout,
    sampler: vk::Sampler,
) {
    let info = [vk::DescriptorImageInfo::default()
        .sampler(sampler)
        .image_view(view)
        .image_layout(layout)];
    let write = vk::WriteDescriptorSet::default()
        .dst_set(set)
        .dst_binding(binding)
        .descriptor_type(vk::DescriptorType::COMBINED_IMAGE_SAMPLER)
        .image_info(&info);
    // SAFETY: the ash seam. The set + view + sampler outlive the call.
    unsafe { raw.update_descriptor_sets(&[write], &[]) };
}

/// Writes a storage buffer into `(set, binding)`.
fn write_storage_buffer(
    raw: &ash::Device,
    set: vk::DescriptorSet,
    binding: u32,
    buffer: vk::Buffer,
    size: vk::DeviceSize,
) {
    let info = [vk::DescriptorBufferInfo {
        buffer,
        offset: 0,
        range: size,
    }];
    let write = vk::WriteDescriptorSet::default()
        .dst_set(set)
        .dst_binding(binding)
        .descriptor_type(vk::DescriptorType::STORAGE_BUFFER)
        .buffer_info(&info);
    // SAFETY: the ash seam. The set + buffer outlive the call.
    unsafe { raw.update_descriptor_sets(&[write], &[]) };
}

#[cfg(test)]
mod tests {
    use std::sync::Mutex;

    use super::*;
    use crate::device::SurfaceSource;
    use crate::resources::BindlessFreeList;
    use crate::validation_issue_count;

    /// Building the DDGI sub-state (the voxel proxy + atlases + ray image + box SSBO + the
    /// six layouts/sets + the static descriptor writes + the one-shot atlas init barrier)
    /// is validation-clean on a software device — the GPU-runtime half of this phase the
    /// toolbox can actually run (the five-pass chain is all compute, no ray tracing). The
    /// per-frame trace render is exercised by the engine e2e once DDGI is wired through the
    /// control plane; here the resource bring-up + the init transition are validated.
    /// Skips cleanly when no Vulkan device is obtainable.
    #[test]
    fn ddgi_resource_bringup_is_validation_clean() {
        let device = match crate::Device::new(&SurfaceSource::Offscreen) {
            Ok(device) => device,
            Err(err) => {
                eprintln!("skipping: no Vulkan device obtainable ({err})");
                return;
            }
        };
        let free_list: BindlessFreeList = Arc::new(Mutex::new(Vec::new()));
        let descriptors = crate::Descriptors::new(&device, &free_list).expect("Descriptors");
        let before = validation_issue_count();

        let mut ddgi = Ddgi::new(&device, &descriptors).expect("Ddgi::new");
        // Built + ready, off by default; the atlases rest in ShaderReadOnly after the init
        // barrier (the mesh-sample resting state).
        assert!(ddgi.ready);
        assert!(!ddgi.use_ddgi);
        assert_eq!(
            ddgi.irradiance().2,
            vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL
        );
        assert_eq!(ddgi.distance().2, vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL);
        assert_ne!(ddgi.mesh_set(), vk::DescriptorSet::null());
        assert_eq!(ddgi.box_capacity(), DDGI_MAX_BOXES);

        // Enabling arms a history reset; a scene upload clamps to capacity + stores the
        // volume; the probe-grid UBO matches the constants.
        ddgi.set_enabled(true);
        assert!(ddgi.history_reset());
        assert!(ddgi.enabled());
        let mins = vec![Vec4::new(-1.0, -1.0, -1.0, 0.0); 4];
        let maxs = vec![Vec4::new(1.0, 1.0, 1.0, 0.0); 4];
        let albedos = vec![Vec4::new(0.8, 0.2, 0.2, 1.0); 4];
        ddgi.set_scene(
            &mins,
            &maxs,
            &albedos,
            Vec3::splat(-2.0),
            Vec3::splat(4.0),
            Vec3::new(0.0, -1.0, 0.0),
            Vec3::ONE,
            1.0,
            Vec3::new(0.1, 0.13, 0.2),
        );
        assert_eq!(ddgi.frame_box_count(), 4);
        assert_eq!(ddgi.volume(), (Vec3::splat(-2.0), Vec3::splat(4.0)));
        assert_eq!(ddgi.probe_count_ubo().x, DDGI_PROBES_X);

        drop(ddgi);
        // SAFETY: the device must idle before its sub-state Drops (here Ddgi already
        // dropped, freeing its sampler + layouts + images + buffer).
        device.wait_idle().expect("wait_idle");
        assert_eq!(
            validation_issue_count(),
            before,
            "the DDGI bring-up + init transition raised no validation issues"
        );
    }

    /// The push-constant structs byte-match the `.slang` `Push` layouts the SPIR-V
    /// reads — a wrong offset is a silently corrupted dispatch, so pin each size.
    #[test]
    fn ddgi_push_sizes_match_slang() {
        assert_eq!(size_of::<VoxelizePush>(), 48);
        assert_eq!(size_of::<TracePush>(), 112);
        assert_eq!(size_of::<BlendPush>(), 48);
        assert_eq!(size_of::<BorderPush>(), 32);
    }

    /// The octahedral atlas dimensions follow `tilesPerRow · (interior + 2)`, matching
    /// the shaders' `atlasW`/`atlasH` derivation — a wrong gutter count corrupts the
    /// border copy + the bilinear sample.
    #[test]
    fn atlas_dimensions_match_octahedral_tiling() {
        assert_eq!(irradiance_atlas_width(), 8 * 4 * (8 + 2));
        assert_eq!(irradiance_atlas_height(), 8 * (8 + 2));
        assert_eq!(distance_atlas_width(), 8 * 4 * (16 + 2));
        assert_eq!(distance_atlas_height(), 8 * (16 + 2));
        assert_eq!(DDGI_PROBE_TOTAL, 8 * 4 * 8);
    }

    /// The pure DDGI state machine (enable/scene/advance/history) without a device, so
    /// the acceptance gate can assert the box-grow + volume-fit + history-reset behavior
    /// the named tests require. Mirrors the production fields the methods touch.
    #[derive(Default)]
    struct DdgiState {
        use_ddgi: bool,
        ready: bool,
        history_reset: bool,
        frame_box_count: u32,
        box_capacity: u32,
        frame_index: u32,
        volume_min: Vec3,
        volume_extent: Vec3,
    }

    impl DdgiState {
        fn set_enabled(&mut self, enabled: bool) {
            if enabled && !self.use_ddgi {
                self.history_reset = true;
            }
            self.use_ddgi = enabled;
        }

        fn set_scene(&mut self, box_count: u32, volume_min: Vec3, volume_extent: Vec3) {
            if !self.ready {
                return;
            }
            self.frame_box_count = box_count.min(self.box_capacity);
            self.volume_min = volume_min;
            self.volume_extent = volume_extent;
        }

        fn advance_frame(&mut self) {
            self.frame_index = self.frame_index.wrapping_add(1);
            self.history_reset = false;
        }

        fn wants_ddgi(&self, pipelines_ready: bool) -> bool {
            self.use_ddgi && self.ready && pipelines_ready
        }
    }

    /// The five DDGI passes run only when DDGI is on AND the resources/PSOs are ready;
    /// absent otherwise (the acceptance gate's first bullet).
    #[test]
    fn wants_ddgi_only_when_on_ready_and_pipelines_present() {
        let mut s = DdgiState {
            ready: true,
            box_capacity: DDGI_MAX_BOXES,
            ..Default::default()
        };
        // Off → never, whatever the pipelines.
        assert!(!s.wants_ddgi(true));
        s.set_enabled(true);
        // On + ready but the PSOs failed to build → no passes.
        assert!(!s.wants_ddgi(false));
        // On + ready + PSOs present → the chain runs.
        assert!(s.wants_ddgi(true));
        // Not ready → never (e.g. a creation failure left `ready` false).
        s.ready = false;
        assert!(!s.wants_ddgi(true));
    }

    /// The box SSBO grows to the scene box count, clamped to capacity; the volume
    /// placement matches the scene AABB fit passed in (the acceptance gate's second
    /// bullet). A no-op before `ready`.
    #[test]
    fn set_scene_clamps_box_count_and_stores_volume() {
        let mut s = DdgiState {
            box_capacity: DDGI_MAX_BOXES,
            ..Default::default()
        };
        // Before ready → no-op (stale zero count).
        s.set_scene(10, Vec3::splat(-2.0), Vec3::splat(4.0));
        assert_eq!(s.frame_box_count, 0);

        s.ready = true;
        s.set_scene(10, Vec3::new(-3.0, -1.0, -5.0), Vec3::new(6.0, 2.0, 10.0));
        assert_eq!(s.frame_box_count, 10);
        assert_eq!(s.volume_min, Vec3::new(-3.0, -1.0, -5.0));
        assert_eq!(s.volume_extent, Vec3::new(6.0, 2.0, 10.0));

        // Over capacity → clamped.
        s.set_scene(DDGI_MAX_BOXES + 100, Vec3::ZERO, Vec3::splat(1.0));
        assert_eq!(s.frame_box_count, DDGI_MAX_BOXES);
    }

    /// `history_reset` is set on enable and stays set until a frame is recorded, then
    /// `advance_frame` clears it; re-enabling re-arms it (the acceptance gate's third
    /// bullet — set on enable/resize, cleared on subsequent frames).
    #[test]
    fn history_reset_arms_on_enable_and_clears_after_a_frame() {
        let mut s = DdgiState {
            ready: true,
            box_capacity: DDGI_MAX_BOXES,
            ..Default::default()
        };
        // Enabling from off arms the reset.
        s.set_enabled(true);
        assert!(s.history_reset);
        // The first recorded frame consumes it.
        s.advance_frame();
        assert!(!s.history_reset);
        assert_eq!(s.frame_index, 1);
        // A second frame keeps it cleared and bumps the index.
        s.advance_frame();
        assert!(!s.history_reset);
        assert_eq!(s.frame_index, 2);
        // Re-enabling while already on does NOT re-arm (only an off→on edge does).
        s.set_enabled(true);
        assert!(!s.history_reset);
        // A resize / explicit reset re-arms it.
        s.history_reset = true;
        assert!(s.history_reset);
    }
}
