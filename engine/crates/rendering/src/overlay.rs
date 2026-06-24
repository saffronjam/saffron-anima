//! The final post chain: the mandatory HDR→display tonemap, the analytic ground
//! grid, and the editor overlay — the passes that run every frame after the scene +
//! AA passes and complete the offscreen color the present blit / shm publish consume.
//!
//! - **Tonemap** is mandatory: an in-place compute pass on the offscreen color
//!   (`StorageImageRwCompute`, GENERAL layout) that maps the scene's linear HDR
//!   radiance to display range. Exposure is `exp2(exposure_ev)`.
//! - **Grid** is an optional fullscreen depth-tested debug overlay drawn on the 1×
//!   resolved color after tonemap; its fragment reconstructs the world ray from a
//!   push-constant `inv_view_proj` and writes `SV_Depth` so scene geometry occludes
//!   it.
//! - **Overlay** is the editor gizmo (handles + entity billboards): a plain
//!   [`OverlayVertex`] CPU stream uploaded into a grow-only per-frame vertex buffer
//!   and drawn in two ranges — a depth-tested range (camera frustums, occluded) then
//!   an always-on-top range (handles). Composited into the post-tonemap color so the
//!   present-only blit embeds it too.
//!
//! The geometry itself is the host's native gizmo builder (PP-10); this module owns
//! the vertex contract, the per-frame upload buffer, the pushes, and the recorders.

use std::sync::Arc;

use ash::vk;
use saffron_geometry::glam::{Mat4, Vec2, Vec4};

use crate::Result;
use crate::frame::MAX_FRAMES_IN_FLIGHT;
use crate::resources::{Buffer, DeviceResources};

/// One editor-overlay vertex: a clip-space NDC position, an RGBA color, a feather
/// `edge` vector, and an NDC depth. The vertex stream the `gizmo_overlay` PSO binds.
///
/// `edge.xy` are signed coordinates per feather direction (±1 at the nominal edge),
/// `edge.zw` the matching half-extents in pixels (a non-positive half-extent disables
/// that direction — lines feather one way, filled quads both). `depth` is the NDC z
/// ([0,1]); only the depth-tested range uses it, the on-top range leaves it 0.
///
/// `#[repr(C)]` + [`bytemuck::Pod`]: the host fills this vertex with a `vec2` position +
/// a `vec4` color that aligns the color to offset 16, so [`Vec4`]'s 16-byte alignment
/// reproduces the layout exactly.
/// The attribute offsets the PSO declares come from [`std::mem::offset_of`] on this
/// struct, so the upload and the bindings are self-consistent.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, bytemuck::Pod, bytemuck::Zeroable)]
pub struct OverlayVertex {
    /// Clip-space NDC position ([-1, 1]).
    pub position: Vec2,
    /// Padding to align `color` to offset 16.
    _pad: [f32; 2],
    /// RGBA color (straight alpha; the pass alpha-blends).
    pub color: Vec4,
    /// `xy` signed edge coords per feather direction, `zw` the pixel half-extents.
    pub edge: Vec4,
    /// NDC z ([0, 1]); 0 = on the near plane (on top). Only the depth-tested range
    /// reads it.
    pub depth: f32,
    /// Tail padding to the 16-byte struct alignment.
    _pad_tail: [f32; 3],
}

const _: () = assert!(size_of::<OverlayVertex>() == 64);
const _: () = assert!(std::mem::offset_of!(OverlayVertex, position) == 0);
const _: () = assert!(std::mem::offset_of!(OverlayVertex, color) == 16);
const _: () = assert!(std::mem::offset_of!(OverlayVertex, edge) == 32);
const _: () = assert!(std::mem::offset_of!(OverlayVertex, depth) == 48);

impl OverlayVertex {
    /// A vertex with the given clip-space position, color, feather edge, and NDC depth
    /// — the constructor the host's gizmo builder uses (the padding fields are zeroed).
    pub fn new(position: Vec2, color: Vec4, edge: Vec4, depth: f32) -> Self {
        Self {
            position,
            _pad: [0.0; 2],
            color,
            edge,
            depth,
            _pad_tail: [0.0; 3],
        }
    }
}

/// The tonemap compute push: the linear exposure multiplier + the operator mode, matching
/// `tonemap.slang`'s `Push`.
#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
pub struct TonemapPush {
    /// The linear exposure multiplier (`exp2(exposure_ev)`).
    pub exposure: f32,
    /// The tonemap operator ([`TonemapMode`] as `u32`).
    pub mode: u32,
}

const _: () = assert!(size_of::<TonemapPush>() == 8);

impl TonemapPush {
    /// The tonemap push for `exposure_ev` stops + operator `mode`.
    pub fn new(exposure_ev: f32, mode: TonemapMode) -> Self {
        Self {
            exposure: exposure_ev.exp2(),
            mode: mode as u32,
        }
    }
}

/// The selectable tonemap operator. Wire-encoded by the kebab-case name pair below.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
#[repr(u32)]
pub enum TonemapMode {
    /// Simple Reinhard (flat; for A/B).
    Reinhard = 0,
    /// ACES filmic (Narkowicz) — the engine default.
    #[default]
    Aces = 1,
    /// AgX (graceful highlight compression, hue-preserving).
    Agx = 2,
    /// Khronos PBR Neutral (material-color-preserving; good for thumbnails).
    PbrNeutral = 3,
}

impl TonemapMode {
    /// The wire / CLI name.
    #[must_use]
    pub fn as_str(self) -> &'static str {
        match self {
            TonemapMode::Reinhard => "reinhard",
            TonemapMode::Aces => "aces",
            TonemapMode::Agx => "agx",
            TonemapMode::PbrNeutral => "pbr-neutral",
        }
    }

    /// Parses a tonemap-operator name, `None` on an unknown value.
    #[must_use]
    pub fn from_name(name: &str) -> Option<TonemapMode> {
        match name {
            "reinhard" => Some(TonemapMode::Reinhard),
            "aces" => Some(TonemapMode::Aces),
            "agx" => Some(TonemapMode::Agx),
            "pbr-neutral" => Some(TonemapMode::PbrNeutral),
            _ => None,
        }
    }
}

/// The grid push: the camera world→clip + its inverse, both vertex+fragment stage.
/// Two mat4s (128 bytes), matching `grid.slang`'s `GridPush`. The fragment
/// reconstructs the world view ray from `inv_view_proj` and reprojects the ground
/// point through `view_proj` for `SV_Depth`.
#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
pub struct GridPush {
    /// World → clip (this frame's camera) — for the fragment `SV_Depth`.
    pub view_proj: Mat4,
    /// Clip → world (the inverse) — for the view-ray reconstruction.
    pub inv_view_proj: Mat4,
}

const _: () = assert!(size_of::<GridPush>() == 128);

impl GridPush {
    /// The grid push for `view_proj`, computing the inverse for the view-ray
    /// reconstruction.
    pub fn new(view_proj: Mat4) -> Self {
        Self {
            view_proj,
            inv_view_proj: view_proj.inverse(),
        }
    }
}

/// Per-frame editor-overlay geometry: the vertex list (depth-tested range first, then
/// the always-on-top range) and a grow-only mapped vertex buffer per frame-in-flight.
///
/// The pass body cannot hold `&mut Renderer`, so the buffer is prepared (grown +
/// uploaded) *before* the graph build via [`OverlayState::prepare`]; the pass then
/// captures only the resolved [`vk::Buffer`] handle + the counts (README §2).
/// Single-thread state — only the render thread touches it.
pub struct OverlayState {
    resources: Arc<DeviceResources>,
    /// The combined vertex list: `[0, depth_tested_count)` then the on-top range.
    vertices: Vec<OverlayVertex>,
    /// How many leading vertices are the depth-tested (occluded) range.
    depth_tested_count: u32,
    /// Grow-only mapped vertex buffer per frame-in-flight.
    buffers: [Option<Buffer>; MAX_FRAMES_IN_FLIGHT],
    /// Current capacity (in vertices) of each per-frame buffer.
    capacity: [u32; MAX_FRAMES_IN_FLIGHT],
}

/// What [`OverlayState::prepare`] hands the graph build: the resolved per-frame vertex
/// buffer plus the two draw-range counts. Captured by the overlay pass body.
#[derive(Clone, Copy)]
pub struct OverlayDraw {
    /// The per-frame vertex buffer the pass binds (this frame's uploaded geometry).
    pub buffer: vk::Buffer,
    /// Total vertices in the buffer (depth-tested + on-top).
    pub vertex_count: u32,
    /// Leading vertices in the depth-tested (occluded) range.
    pub depth_tested_count: u32,
}

impl OverlayState {
    /// An empty overlay state owning a clone of the device resources (for the
    /// per-frame buffers' `Drop`). No buffers are allocated until the first non-empty
    /// frame.
    pub fn new(resources: &Arc<DeviceResources>) -> Self {
        Self {
            resources: Arc::clone(resources),
            vertices: Vec::new(),
            depth_tested_count: 0,
            buffers: [const { None }; MAX_FRAMES_IN_FLIGHT],
            capacity: [0; MAX_FRAMES_IN_FLIGHT],
        }
    }

    /// Replaces this frame's overlay geometry: the `depth_tested` range (occluded by
    /// scene geometry) followed by the `on_top` range (always drawn). The
    /// `depth_tested_count` is recorded so the pass draws each range with its own PSO.
    pub fn submit(&mut self, mut depth_tested: Vec<OverlayVertex>, on_top: Vec<OverlayVertex>) {
        self.depth_tested_count = depth_tested.len() as u32;
        depth_tested.extend(on_top);
        self.vertices = depth_tested;
    }

    /// Whether any overlay geometry is queued this frame (the gate for arming the
    /// overlay pass).
    pub fn has_geometry(&self) -> bool {
        !self.vertices.is_empty()
    }

    /// Grows the `frame` slot's vertex buffer to fit the queued geometry and uploads
    /// it, returning the resolved draw info the graph captures — or `None` when no
    /// geometry is queued (the pass is skipped). Done before the graph build so the
    /// pass body captures only the handle (README §2).
    ///
    /// # Errors
    ///
    /// Returns [`crate::Error::Vk`] if the (re)allocation of the per-frame buffer
    /// fails; the prior buffer is left in place on failure.
    pub fn prepare(&mut self, frame: usize) -> Result<Option<OverlayDraw>> {
        let vertex_count = self.vertices.len() as u32;
        if vertex_count == 0 {
            return Ok(None);
        }
        if self.capacity[frame] < vertex_count {
            let bytes = u64::from(vertex_count) * size_of::<OverlayVertex>() as u64;
            // Drop the prior buffer before allocating the replacement so the old
            // allocation frees first (the slot's prior frame already completed).
            self.buffers[frame] = None;
            let buffer = Buffer::new(
                &self.resources,
                bytes,
                vk::BufferUsageFlags::VERTEX_BUFFER,
                &vk_mem::AllocationCreateInfo {
                    usage: vk_mem::MemoryUsage::AutoPreferHost,
                    flags: vk_mem::AllocationCreateFlags::HOST_ACCESS_SEQUENTIAL_WRITE
                        | vk_mem::AllocationCreateFlags::MAPPED,
                    ..Default::default()
                },
            )?;
            self.buffers[frame] = Some(buffer);
            self.capacity[frame] = vertex_count;
        }
        let buffer = self.buffers[frame]
            .as_mut()
            .expect("the buffer was just ensured");
        let src = bytemuck::cast_slice::<OverlayVertex, u8>(&self.vertices);
        let dst = buffer.mapped_bytes().expect("overlay buffer is MAPPED");
        dst[..src.len()].copy_from_slice(src);
        Ok(Some(OverlayDraw {
            buffer: buffer.handle(),
            vertex_count,
            depth_tested_count: self.depth_tested_count.min(vertex_count),
        }))
    }
}

/// The final-post-chain pass names that arm this frame, in graph order. The tonemap is
/// mandatory (always present unless its PSO build failed); the grid arms only when shown,
/// the overlay only when geometry is queued. Pure gate logic so the phase's
/// "tonemap-always / grid-overlay-conditional" acceptance test runs without a device —
/// it mirrors the `if let Some(..)` guards in [`crate::Renderer::add_tonemap_pass`] /
/// [`crate::Renderer::add_grid_overlay_passes`].
#[cfg(test)]
pub(crate) fn final_post_pass_names(
    tonemap_built: bool,
    show_grid: bool,
    grid_built: bool,
    has_overlay: bool,
    overlay_built: bool,
) -> Vec<&'static str> {
    let mut names = Vec::new();
    if tonemap_built {
        names.push("tonemap");
    }
    if show_grid && grid_built {
        names.push("grid");
    }
    if has_overlay && overlay_built {
        names.push("editor-overlay");
    }
    names
}

/// Records the ground-grid draw: bind the grid PSO + the `view_proj`/`inv_view_proj`
/// push, draw the fullscreen triangle (no vertex buffer). The graph opened the
/// rendering scope + set the viewport/scissor.
pub fn record_grid(
    raw: &ash::Device,
    cmd: vk::CommandBuffer,
    pipeline: vk::Pipeline,
    layout: vk::PipelineLayout,
    push: &GridPush,
) {
    // SAFETY: the ash seam. The PSO/layout are valid this frame; the push spans the
    // declared two-mat4 vertex+fragment range; the fullscreen triangle needs no buffer.
    unsafe {
        raw.cmd_bind_pipeline(cmd, vk::PipelineBindPoint::GRAPHICS, pipeline);
        raw.cmd_push_constants(
            cmd,
            layout,
            vk::ShaderStageFlags::VERTEX | vk::ShaderStageFlags::FRAGMENT,
            0,
            bytemuck::bytes_of(push),
        );
        raw.cmd_draw(cmd, 3, 1, 0, 0);
    }
}

/// Records the editor overlay: bind the uploaded vertex buffer, then draw the
/// depth-tested range (occluded, `overlay_depth` PSO) followed by the on-top range
/// (always drawn, `overlay` PSO). The graph opened the rendering scope + set the
/// viewport/scissor.
pub fn record_overlay(
    raw: &ash::Device,
    cmd: vk::CommandBuffer,
    draw: &OverlayDraw,
    overlay: vk::Pipeline,
    overlay_depth: vk::Pipeline,
) {
    // SAFETY: the ash seam. The buffer holds this frame's uploaded vertices; the two
    // ranges partition `[0, vertex_count)`; the PSOs are valid this frame.
    unsafe {
        raw.cmd_bind_vertex_buffers(cmd, 0, &[draw.buffer], &[0]);
        if draw.depth_tested_count > 0 {
            raw.cmd_bind_pipeline(cmd, vk::PipelineBindPoint::GRAPHICS, overlay_depth);
            raw.cmd_draw(cmd, draw.depth_tested_count, 1, 0, 0);
        }
        if draw.vertex_count > draw.depth_tested_count {
            raw.cmd_bind_pipeline(cmd, vk::PipelineBindPoint::GRAPHICS, overlay);
            raw.cmd_draw(
                cmd,
                draw.vertex_count - draw.depth_tested_count,
                1,
                draw.depth_tested_count,
                0,
            );
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The `OverlayVertex` byte layout the host fills: position@0, color@16,
    /// edge@32, depth@48, size 64. A wrong offset is a
    /// silently corrupted vertex stream, so pin each. Pure layout logic, any host.
    #[test]
    fn overlay_vertex_layout_matches_the_host_contract() {
        use std::mem::offset_of;
        assert_eq!(size_of::<OverlayVertex>(), 64);
        assert_eq!(offset_of!(OverlayVertex, position), 0);
        assert_eq!(offset_of!(OverlayVertex, color), 16);
        assert_eq!(offset_of!(OverlayVertex, edge), 32);
        assert_eq!(offset_of!(OverlayVertex, depth), 48);
    }

    /// The tonemap push is `exp2(ev)`: 0 EV → 1.0×, +1 EV → 2.0×, -1 EV → 0.5×. Pure math,
    /// any host.
    #[test]
    fn tonemap_push_is_exp2_of_the_ev() {
        let m = TonemapMode::Aces;
        assert!((TonemapPush::new(0.0, m).exposure - 1.0).abs() < 1e-6);
        assert!((TonemapPush::new(1.0, m).exposure - 2.0).abs() < 1e-6);
        assert!((TonemapPush::new(-1.0, m).exposure - 0.5).abs() < 1e-6);
        assert!((TonemapPush::new(2.0, m).exposure - 4.0).abs() < 1e-6);
        assert_eq!(TonemapPush::new(0.0, TonemapMode::Agx).mode, 2);
        assert_eq!(size_of::<TonemapPush>(), 8);
    }

    /// `GridPush::new` records `view_proj` and its mathematical inverse — round-tripping
    /// through both is the identity (within float tolerance). The grid fragment relies
    /// on this for the view-ray reconstruction. Pure math, any host.
    #[test]
    fn grid_push_records_view_proj_and_its_inverse() {
        let view_proj = Mat4::perspective_rh(1.0, 1.6, 0.1, 100.0)
            * Mat4::look_at_rh(
                saffron_geometry::glam::Vec3::new(3.0, 4.0, 5.0),
                saffron_geometry::glam::Vec3::ZERO,
                saffron_geometry::glam::Vec3::Y,
            );
        let push = GridPush::new(view_proj);
        let identity = push.view_proj * push.inv_view_proj;
        let diff = (identity - Mat4::IDENTITY).to_cols_array();
        assert!(
            diff.iter().all(|c| c.abs() < 1e-3),
            "view_proj * inv_view_proj should be the identity, got {identity:?}"
        );
        assert_eq!(size_of::<GridPush>(), 128);
    }

    /// The tonemap is always in the graph (mandatory); the grid arms only when shown +
    /// built; the overlay only when geometry is queued + built — in that graph order. Pure
    /// gate logic, any host.
    #[test]
    fn final_post_chain_arms_tonemap_always_grid_and_overlay_conditionally() {
        // Tonemap only: nothing else armed.
        assert_eq!(
            final_post_pass_names(true, false, false, false, false),
            vec!["tonemap"]
        );
        // Grid shown + built arms it after the tonemap.
        assert_eq!(
            final_post_pass_names(true, true, true, false, false),
            vec!["tonemap", "grid"]
        );
        // Grid shown but its PSO failed to build → not armed (degrades, no panic).
        assert_eq!(
            final_post_pass_names(true, true, false, false, false),
            vec!["tonemap"]
        );
        // Overlay geometry queued + built arms it last.
        assert_eq!(
            final_post_pass_names(true, false, false, true, true),
            vec!["tonemap", "editor-overlay"]
        );
        // All three armed, in graph order.
        assert_eq!(
            final_post_pass_names(true, true, true, true, true),
            vec!["tonemap", "grid", "editor-overlay"]
        );
        // Overlay geometry present but no PSO → not armed.
        assert_eq!(
            final_post_pass_names(true, false, false, true, false),
            vec!["tonemap"]
        );
    }

    /// `submit` lays the depth-tested range first then the on-top range and records the
    /// split count, so one buffer drives both draws, and [`OverlayState::prepare`] grows +
    /// uploads the per-frame buffer. Needs a device (the per-frame buffer is
    /// VMA-allocated); skips when none is present.
    #[test]
    fn submit_lays_depth_tested_first_then_uploads() {
        let device = match crate::device::Device::new(&crate::device::SurfaceSource::Offscreen) {
            Ok(device) => device,
            Err(err) => {
                eprintln!("skipping: no Vulkan device ({err})");
                return;
            }
        };
        let before = crate::validation_issue_count();
        let mut state = OverlayState::new(device.resources());
        assert!(!state.has_geometry());
        assert!(
            state.prepare(0).expect("empty prepare").is_none(),
            "no geometry → no draw"
        );

        let depth = vec![OverlayVertex::default(); 3];
        let on_top = vec![OverlayVertex::default(); 6];
        state.submit(depth, on_top);
        assert!(state.has_geometry());
        assert_eq!(state.depth_tested_count, 3);
        assert_eq!(state.vertices.len(), 9);

        // Prepare grows + uploads the per-frame buffer and reports the two draw ranges.
        let draw = state.prepare(0).expect("prepare").expect("draw");
        assert_eq!(draw.vertex_count, 9);
        assert_eq!(draw.depth_tested_count, 3);

        // An empty submit clears the geometry (the gate disarms the pass).
        state.submit(Vec::new(), Vec::new());
        assert!(!state.has_geometry());

        // Drop the state (freeing its buffer) before the device, then idle + teardown.
        drop(state);
        device.wait_idle().expect("idle");
        drop(device);

        let after = crate::validation_issue_count();
        assert_eq!(
            before,
            after,
            "overlay buffer alloc + teardown must be validation-clean (saw {} new)",
            after.saturating_sub(before)
        );
    }
}
