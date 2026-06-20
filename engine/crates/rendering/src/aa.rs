//! Anti-aliasing mode selection + the motion-vector prepass the temporal modes need.
//!
//! The three AA modes are mutually exclusive: MSAA (multisampled scene targets resolved
//! into the offscreen), FXAA (scene → scratch, a compute edge-blur → offscreen), and TAA
//! (motion-vector reprojection + a compute resolve with two ping-pong history images).
//! [`Aa`] is the single selector — [`Aa::set`] enforces the exclusivity in one place
//! (MSAA wins if `samples > 1`) and clamps the requested count to what the color + depth
//! formats actually support. There is one AA state, not three independent toggles that can
//! contradict (the phase's NO-LEGACY note).
//!
//! This ports the C++ `Targets` AA cap/toggle slice (`renderer_types.cppm:1339`) + `setAa`
//! / `clampSampleCount` (`renderer_aa.cpp:67`/`:43`). The motion prepass push + recorder
//! (`MotionPush`, `recordMotion`, `renderer_drawlist.cpp:1092`) live here too, since the
//! motion vectors are the temporal AA's (and SSGI's) shared dependency.

use ash::vk;
use saffron_geometry::glam::Mat4;

use crate::draw_list::SceneDrawList;
use crate::scene_pass::record_batch_submeshes;

/// The screen-space motion-vector target format (rg16f): the per-pixel `prevUv - curUv`
/// offset TAA / SSGI reproject through. The C++ `MotionFormat`
/// (`renderer_detail.cppm:1409`).
pub const MOTION_FORMAT: vk::Format = vk::Format::R16G16_SFLOAT;

/// The TAA history exponential-moving-average weight (the history's share of the resolve).
/// The C++ `TaaHistoryWeight` (`renderer_detail.cppm:1410`).
pub const TAA_HISTORY_WEIGHT: f32 = 0.9;

/// The anti-aliasing selection: the device's supported sample counts (a fact), the chosen
/// MSAA count, and the FXAA / TAA toggles. Mutually exclusive by construction — only
/// [`Aa::set`] mutates it, and it never leaves more than one mode active.
///
/// Plain data — the GPU targets the modes drive live on [`crate::ViewTarget`]; this is the
/// selector the frame-graph build reads to branch the scene output. The C++ `Targets` AA
/// slice (`sampleCount` / `maxSampleCount` / `supportedSampleCounts` / `fxaaEnabled` /
/// `taaEnabled`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Aa {
    /// The chosen MSAA sample count (`TYPE_1` = MSAA off).
    sample_count: vk::SampleCountFlags,
    /// The largest sample count the color + depth formats both support (the device cap).
    max_sample_count: vk::SampleCountFlags,
    /// The counts the color + depth MSAA formats both accept (clamp target).
    supported: vk::SampleCountFlags,
    /// FXAA post-process (mutually exclusive with MSAA / TAA).
    fxaa: bool,
    /// TAA resolve (mutually exclusive with MSAA / FXAA).
    taa: bool,
}

impl Aa {
    /// Builds the AA selector against the device's `supported` sample counts, off (1×, no
    /// FXAA/TAA). `max_sample_count` is the largest supported count for inspection.
    pub fn new(supported: vk::SampleCountFlags) -> Self {
        Self {
            sample_count: vk::SampleCountFlags::TYPE_1,
            max_sample_count: largest_supported(supported),
            supported,
            fxaa: false,
            taa: false,
        }
    }

    /// Selects the AA mode, enforcing mutual exclusivity in one place: MSAA wins if
    /// `msaa_samples >= 2` (the count clamped to what the formats support), else FXAA if
    /// requested, else TAA if requested, else off. Returns `true` if the *MSAA sample
    /// count* changed — the caller then clears the sample-count-baked PSO cache. The C++
    /// `setAa` (`renderer_aa.cpp:67`).
    pub fn set(&mut self, msaa_samples: u32, fxaa: bool, taa: bool) -> bool {
        let requested = if msaa_samples >= 8 {
            vk::SampleCountFlags::TYPE_8
        } else if msaa_samples >= 4 {
            vk::SampleCountFlags::TYPE_4
        } else if msaa_samples >= 2 {
            vk::SampleCountFlags::TYPE_2
        } else {
            vk::SampleCountFlags::TYPE_1
        };
        let count = clamp_sample_count(self.supported, requested);
        let msaa = count != vk::SampleCountFlags::TYPE_1;
        let old_count = self.sample_count;
        // The three modes are mutually exclusive: MSAA wins if a count > 1 was requested,
        // then FXAA, then TAA. MSAA forces FXAA + TAA off; FXAA forces TAA off.
        self.sample_count = count;
        self.fxaa = !msaa && fxaa;
        self.taa = !msaa && !fxaa && taa;
        old_count != self.sample_count
    }

    /// Selects the AA mode by name (the `sa` CLI / control wire): `"off"`, `"fxaa"`,
    /// `"taa"`, `"msaa2"`, `"msaa4"`, `"msaa8"`. Returns whether the sample count changed.
    /// The C++ `setAaMode`.
    pub fn set_mode(&mut self, mode: &str) -> bool {
        let (samples, fxaa, taa) = match mode {
            "fxaa" => (1, true, false),
            "taa" => (1, false, true),
            "msaa2" => (2, false, false),
            "msaa4" => (4, false, false),
            "msaa8" => (8, false, false),
            _ => (1, false, false),
        };
        self.set(samples, fxaa, taa)
    }

    /// The current mode as a name (`"off"` / `"fxaa"` / `"taa"` / `"msaaN"`). The C++
    /// `aaMode`.
    pub fn mode(&self) -> String {
        if self.fxaa {
            return "fxaa".to_string();
        }
        if self.taa {
            return "taa".to_string();
        }
        match sample_count_value(self.sample_count) {
            n if n <= 1 => "off".to_string(),
            n => format!("msaa{n}"),
        }
    }

    /// The chosen MSAA sample count (`TYPE_1` when MSAA is off).
    pub fn sample_count(&self) -> vk::SampleCountFlags {
        self.sample_count
    }

    /// Whether MSAA is active (sample count > 1).
    pub fn msaa(&self) -> bool {
        self.sample_count != vk::SampleCountFlags::TYPE_1
    }

    /// Whether FXAA is active.
    pub fn fxaa(&self) -> bool {
        self.fxaa
    }

    /// Whether TAA is active.
    pub fn taa(&self) -> bool {
        self.taa
    }

    /// The largest MSAA count the device supports (for inspection / UI clamping).
    pub fn max_sample_count(&self) -> vk::SampleCountFlags {
        self.max_sample_count
    }
}

/// The largest MSAA sample count not exceeding `requested` that `supported` accepts (`1×`
/// if none) — a count valid as a framebuffer limit can still be unsupported for a specific
/// format, and creating an image with it is invalid. The C++ `clampSampleCount`
/// (`renderer_aa.cpp:43`).
pub fn clamp_sample_count(
    supported: vk::SampleCountFlags,
    requested: vk::SampleCountFlags,
) -> vk::SampleCountFlags {
    let want = sample_count_value(requested);
    for candidate in [
        vk::SampleCountFlags::TYPE_8,
        vk::SampleCountFlags::TYPE_4,
        vk::SampleCountFlags::TYPE_2,
    ] {
        if sample_count_value(candidate) <= want && supported.contains(candidate) {
            return candidate;
        }
    }
    vk::SampleCountFlags::TYPE_1
}

/// The largest count in a supported set (for `max_sample_count` reporting).
fn largest_supported(supported: vk::SampleCountFlags) -> vk::SampleCountFlags {
    for candidate in [
        vk::SampleCountFlags::TYPE_8,
        vk::SampleCountFlags::TYPE_4,
        vk::SampleCountFlags::TYPE_2,
    ] {
        if supported.contains(candidate) {
            return candidate;
        }
    }
    vk::SampleCountFlags::TYPE_1
}

/// The integer sample count a `SampleCountFlags` bit names (the bit value is the count:
/// `TYPE_4` == `0b100` == 4). `TYPE_1` for any unrecognized bit.
fn sample_count_value(flags: vk::SampleCountFlags) -> u32 {
    for (bit, n) in [
        (vk::SampleCountFlags::TYPE_8, 8),
        (vk::SampleCountFlags::TYPE_4, 4),
        (vk::SampleCountFlags::TYPE_2, 2),
    ] {
        if flags.contains(bit) {
            return n;
        }
    }
    1
}

/// The motion-vector prepass push: this frame's + last frame's camera viewProj. The vertex
/// shader reprojects each surface point through both to write `prevUv - curUv`. Two mat4s,
/// vertex stage, matching `motion.slang`'s `Push`.
#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
pub struct MotionPush {
    /// This frame's world → clip.
    pub cur_view_proj: Mat4,
    /// Last frame's world → clip (this view's own previous frame).
    pub prev_view_proj: Mat4,
}

const _: () = assert!(size_of::<MotionPush>() == 128);

/// The TAA resolve push: a params vec4 (`x` = history weight, `y` = 1 if history is valid
/// this frame). 16 bytes, matching `taa.slang`'s `Push`.
#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
pub struct TaaPush {
    /// `x` = history EMA weight (0..1), `y` = 1 if history is valid (else fall back to the
    /// current frame), `zw` unused.
    pub params: saffron_geometry::glam::Vec4,
}

const _: () = assert!(size_of::<TaaPush>() == 16);

/// Records the motion-vector prepass: bind the instance set (2) + the cur/prev camera
/// viewProj push, then draw every batch's submeshes with both vertex bindings pointing at
/// the same static stream (so `prevPosition == position` and object motion comes from
/// `inst.prevModel`). The skinned deform-motion path (distinct cur/prev deformed buffers)
/// lands with the skinning prepass (phase 12). The C++ `recordMotion`
/// (`renderer_drawlist.cpp:1092`), static-batch branch.
#[allow(clippy::too_many_arguments)]
pub fn record_motion(
    raw: &ash::Device,
    cmd: vk::CommandBuffer,
    list: &SceneDrawList,
    motion_pipeline: vk::Pipeline,
    motion_layout: vk::PipelineLayout,
    instance_set: vk::DescriptorSet,
    push: &MotionPush,
    deformed: Option<vk::Buffer>,
    prev_deformed: Option<vk::Buffer>,
) {
    if !list.valid || list.batches.is_empty() {
        return;
    }
    let push_bytes = bytemuck::bytes_of(push);
    // SAFETY: the ash seam. The PSO/layout/set are valid this frame; the push spans the
    // declared two-mat4 vertex range.
    unsafe {
        raw.cmd_bind_pipeline(cmd, vk::PipelineBindPoint::GRAPHICS, motion_pipeline);
        raw.cmd_bind_descriptor_sets(
            cmd,
            vk::PipelineBindPoint::GRAPHICS,
            motion_layout,
            2,
            &[instance_set],
            &[],
        );
        raw.cmd_push_constants(
            cmd,
            motion_layout,
            vk::ShaderStageFlags::VERTEX,
            0,
            push_bytes,
        );
    }
    for batch in &list.batches {
        // Binding 0 = this frame's position, binding 1 = the previous frame's. A skinned
        // batch reads the current + prev deformed buffers; a static batch binds the same
        // static stream to both (prev == cur, so object motion comes from `prev_model`).
        let (cur, prev) = match (batch.skinned, deformed, prev_deformed) {
            (true, Some(deformed), Some(prev_deformed)) => (deformed, prev_deformed),
            _ => {
                let buffer = batch.mesh.vertex_buffer();
                (buffer, buffer)
            }
        };
        // SAFETY: the ash seam. The bound streams outlive the recorded command (pinned by
        // the batch `Arc` / the frame's `Skinning`); the index buffer + draw cover the batch.
        unsafe {
            raw.cmd_bind_vertex_buffers(cmd, 0, &[cur, prev], &[0, 0]);
            raw.cmd_bind_index_buffer(cmd, batch.mesh.index_buffer(), 0, vk::IndexType::UINT32);
        }
        record_batch_submeshes(raw, cmd, batch);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// `clamp_sample_count` returns the largest supported count ≤ requested, `TYPE_1` when
    /// none. Pure logic, runs on any host. The phase-10 acceptance gate.
    #[test]
    fn clamp_sample_count_picks_largest_supported_le_requested() {
        let all = vk::SampleCountFlags::TYPE_1
            | vk::SampleCountFlags::TYPE_2
            | vk::SampleCountFlags::TYPE_4
            | vk::SampleCountFlags::TYPE_8;
        // The exact requested count when supported.
        assert_eq!(
            clamp_sample_count(all, vk::SampleCountFlags::TYPE_4),
            vk::SampleCountFlags::TYPE_4
        );
        // Requesting 8 with only ≤4 supported clamps down to 4.
        let upto4 = vk::SampleCountFlags::TYPE_1
            | vk::SampleCountFlags::TYPE_2
            | vk::SampleCountFlags::TYPE_4;
        assert_eq!(
            clamp_sample_count(upto4, vk::SampleCountFlags::TYPE_8),
            vk::SampleCountFlags::TYPE_4
        );
        // Requesting 8 with a hole at 4 falls to 2.
        let two_eight = vk::SampleCountFlags::TYPE_1
            | vk::SampleCountFlags::TYPE_2
            | vk::SampleCountFlags::TYPE_8;
        assert_eq!(
            clamp_sample_count(two_eight, vk::SampleCountFlags::TYPE_4),
            vk::SampleCountFlags::TYPE_2
        );
        // Nothing above 1× supported → 1×.
        assert_eq!(
            clamp_sample_count(vk::SampleCountFlags::TYPE_1, vk::SampleCountFlags::TYPE_8),
            vk::SampleCountFlags::TYPE_1
        );
    }

    /// Requesting MSAA + FXAA + TAA together yields MSAA only (MSAA wins); `set(0, true,
    /// true)` yields FXAA only (FXAA beats TAA when no MSAA); `set(0, false, true)` yields
    /// TAA. Mutual exclusivity in one place. The phase-10 acceptance gate.
    #[test]
    fn set_aa_enforces_mutual_exclusivity() {
        let all = vk::SampleCountFlags::TYPE_1
            | vk::SampleCountFlags::TYPE_2
            | vk::SampleCountFlags::TYPE_4
            | vk::SampleCountFlags::TYPE_8;

        // MSAA + FXAA + TAA all requested → MSAA only.
        let mut aa = Aa::new(all);
        let changed = aa.set(4, true, true);
        assert!(changed, "selecting 4× from off changes the sample count");
        assert!(aa.msaa());
        assert_eq!(aa.sample_count(), vk::SampleCountFlags::TYPE_4);
        assert!(!aa.fxaa(), "MSAA forces FXAA off");
        assert!(!aa.taa(), "MSAA forces TAA off");
        assert_eq!(aa.mode(), "msaa4");

        // FXAA + TAA both requested, no MSAA → FXAA only.
        let mut aa = Aa::new(all);
        let changed = aa.set(0, true, true);
        assert!(!changed, "staying at 1× does not change the sample count");
        assert!(!aa.msaa());
        assert!(aa.fxaa());
        assert!(!aa.taa(), "FXAA beats TAA when no MSAA");
        assert_eq!(aa.mode(), "fxaa");

        // TAA only.
        let mut aa = Aa::new(all);
        aa.set(0, false, true);
        assert!(aa.taa());
        assert!(!aa.fxaa());
        assert!(!aa.msaa());
        assert_eq!(aa.mode(), "taa");

        // Off.
        let mut aa = Aa::new(all);
        aa.set(0, false, false);
        assert_eq!(aa.mode(), "off");
        assert!(!aa.msaa() && !aa.fxaa() && !aa.taa());
    }

    /// Switching MSAA → MSAA at a different count reports a sample-count change (the caller
    /// clears the PSO cache); switching MSAA → FXAA reports a change (8× → 1×); FXAA → TAA
    /// reports none (both 1×). The cache-clear signal is exactly the sample-count delta.
    #[test]
    fn set_aa_reports_sample_count_change_for_pso_cache_clear() {
        let all = vk::SampleCountFlags::TYPE_1
            | vk::SampleCountFlags::TYPE_2
            | vk::SampleCountFlags::TYPE_4
            | vk::SampleCountFlags::TYPE_8;
        let mut aa = Aa::new(all);
        assert!(aa.set(2, false, false), "off → 2× changes the count");
        assert!(aa.set(8, false, false), "2× → 8× changes the count");
        assert!(!aa.set(8, false, false), "8× → 8× does not");
        assert!(aa.set(0, true, false), "8× → FXAA drops to 1× (a change)");
        assert!(
            !aa.set(0, false, true),
            "FXAA → TAA stays 1× (no count change)"
        );
    }

    /// The C++ `MotionFormat` (rg16f) and the std430 push sizes are pinned.
    #[test]
    fn push_layouts_match_shaders() {
        assert_eq!(MOTION_FORMAT, vk::Format::R16G16_SFLOAT);
        assert_eq!(size_of::<MotionPush>(), 128);
        assert_eq!(size_of::<TaaPush>(), 16);
    }
}
