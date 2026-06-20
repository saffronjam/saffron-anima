//! Shared `#[cfg(test)]` fixtures: an in-memory [`ControlRenderer`] stub and an
//! `EngineContext` builder, so the registry and render-command unit tests drive the
//! handlers without the un-headless-constructible concrete `Renderer`.

#![cfg(test)]

use std::path::Path;
use std::sync::Arc;

use saffron_assets::{AssetServer, GpuUploader, ThumbnailGpu, ThumbnailPng};
use saffron_geometry::{Mesh, VertexSkin};
use saffron_rendering::{
    ActiveAlarm, AlarmDrain, CaptureMode, CaptureState, FrameHistoryStats, FrameSample, GpuMesh,
    GpuTexture, PassTiming, PerfConfig, PngTransfer, ProfileCapture, ProfilerMode, ReflectionProbe,
    RenderStatsFull, SubmeshMaterial, ViewId, ViewMode,
};
use saffron_sceneedit::SceneEditContext;
use saffron_window::Window;
use serde_json::{Value, json};

use crate::registry::{ControlRenderer, EngineContext};

/// A no-op upload + thumbnail seam the stub hands to [`ControlRenderer::with_gpu_uploader`]
/// / [`ControlRenderer::with_thumbnail_gpu`]: the asset-domain unit tests resolve against an
/// empty catalog (which negative-caches before reaching the GPU), so the upload / render
/// entry points are never actually driven.
struct StubGpu;

impl GpuUploader for StubGpu {
    fn upload_mesh(
        &self,
        _mesh: &Mesh,
        _skin: &[VertexSkin],
    ) -> saffron_rendering::Result<Arc<GpuMesh>> {
        unreachable!("an empty catalog never reaches the stub uploader")
    }

    fn upload_texture(
        &self,
        _rgba: &[u8],
        _width: u32,
        _height: u32,
        _srgb: bool,
    ) -> saffron_rendering::Result<Arc<GpuTexture>> {
        unreachable!("an empty catalog never reaches the stub uploader")
    }

    fn upload_texture_float(
        &self,
        _rgba: &[f32],
        _width: u32,
        _height: u32,
    ) -> saffron_rendering::Result<Arc<GpuTexture>> {
        unreachable!("an empty catalog never reaches the stub uploader")
    }

    fn skinning_enabled(&self) -> bool {
        false
    }
}

impl ThumbnailGpu for StubGpu {
    fn bind_worker_thread(&self) {}

    fn encode_texture_thumbnail_png(
        &self,
        _texture: &Arc<GpuTexture>,
        _size: u32,
        _transfer: PngTransfer,
    ) -> saffron_rendering::Result<ThumbnailPng> {
        unreachable!("an empty catalog never reaches the stub render-to-PNG path")
    }

    fn encode_asset_thumbnail_png(
        &self,
        _mesh: &Arc<GpuMesh>,
        _size: u32,
    ) -> saffron_rendering::Result<ThumbnailPng> {
        unreachable!("an empty catalog never reaches the stub render-to-PNG path")
    }

    fn encode_model_thumbnail_png(
        &self,
        _mesh: &Arc<GpuMesh>,
        _submesh_materials: &[SubmeshMaterial],
        _size: u32,
    ) -> saffron_rendering::Result<ThumbnailPng> {
        unreachable!("an empty catalog never reaches the stub render-to-PNG path")
    }

    fn render_material_preview(
        &self,
        _material: &SubmeshMaterial,
        _size: u32,
        _shader_spv: Option<&Path>,
    ) -> saffron_rendering::Result<Arc<GpuTexture>> {
        unreachable!("an empty catalog never reaches the stub material-preview path")
    }
}

/// An in-memory renderer stub: every toggle round-trips through a plain field so a
/// handler's "echo the applied state" contract is exercised without a GPU. `rt_supported`
/// defaults `false`, matching a software device, so the RT-gated handlers take their
/// unsupported branch unless a test flips it.
pub struct StubRenderer {
    pub clustered: bool,
    pub depth_prepass: bool,
    pub shadows: bool,
    pub ibl: bool,
    pub ssao: bool,
    pub contact_shadows: bool,
    pub ssgi: bool,
    pub ddgi: bool,
    pub reflection_probes: bool,
    pub skinning: bool,
    pub rt_supported: bool,
    pub rt_shadows: bool,
    pub restir: bool,
    pub view_mode: ViewMode,
    pub aa_samples: u32,
    pub aa_fxaa: bool,
    pub aa_taa: bool,
    pub exposure_ev: f32,
    pub profiler_mode: ProfilerMode,
    pub timestamps_supported: bool,
    pub pipeline_stats_supported: bool,
    pub software_gpu: bool,
    pub perf_config: PerfConfig,
    pub capture_state: CaptureState,
    pub capture_mode: CaptureMode,
    pub capture_id: u32,
    pub width: u32,
    pub height: u32,
    pub probes: Vec<ReflectionProbe>,
    pub active_view: ViewId,
    /// Per-view desired offscreen size, indexed by [`ViewId::index`].
    pub view_sizes: [(u32, u32); saffron_rendering::VIEW_COUNT],
}

impl Default for StubRenderer {
    fn default() -> Self {
        Self {
            clustered: true,
            depth_prepass: false,
            shadows: true,
            ibl: true,
            ssao: false,
            contact_shadows: false,
            ssgi: false,
            ddgi: false,
            reflection_probes: true,
            skinning: true,
            rt_supported: false,
            rt_shadows: false,
            restir: false,
            view_mode: ViewMode::Lit,
            aa_samples: 1,
            aa_fxaa: false,
            aa_taa: false,
            exposure_ev: 0.0,
            profiler_mode: ProfilerMode::Off,
            timestamps_supported: false,
            pipeline_stats_supported: false,
            software_gpu: true,
            perf_config: PerfConfig::default(),
            capture_state: CaptureState::Idle,
            capture_mode: CaptureMode::Single,
            capture_id: 0,
            width: 1280,
            height: 720,
            probes: Vec::new(),
            active_view: ViewId::Scene,
            view_sizes: [(1280, 720); saffron_rendering::VIEW_COUNT],
        }
    }
}

impl ControlRenderer for StubRenderer {
    fn render_stats(&self) -> RenderStatsFull {
        RenderStatsFull {
            software_gpu: self.software_gpu,
            profiler_mode: self.profiler_mode,
            view_mode: self.view_mode,
            exposure_ev: self.exposure_ev,
            ..RenderStatsFull::default()
        }
    }

    fn clustered_enabled(&self) -> bool {
        self.clustered
    }
    fn set_clustered(&mut self, enabled: bool) {
        self.clustered = enabled;
    }
    fn depth_prepass_enabled(&self) -> bool {
        self.depth_prepass
    }
    fn set_depth_prepass(&mut self, enabled: bool) {
        self.depth_prepass = enabled;
    }
    fn shadows_enabled(&self) -> bool {
        self.shadows
    }
    fn set_shadows(&mut self, enabled: bool) {
        self.shadows = enabled;
    }
    fn ibl_enabled(&self) -> bool {
        self.ibl
    }
    fn set_ibl(&mut self, enabled: bool) {
        self.ibl = enabled;
    }
    fn ssao_enabled(&self) -> bool {
        self.ssao
    }
    fn set_ssao(&mut self, enabled: bool) {
        self.ssao = enabled;
    }
    fn contact_shadows_enabled(&self) -> bool {
        self.contact_shadows
    }
    fn set_contact_shadows(&mut self, enabled: bool) {
        self.contact_shadows = enabled;
    }
    fn ssgi_enabled(&self) -> bool {
        self.ssgi
    }
    fn set_ssgi(&mut self, enabled: bool) {
        self.ssgi = enabled;
    }
    fn ddgi_enabled(&self) -> bool {
        self.ddgi
    }
    fn set_ddgi(&mut self, enabled: bool) {
        self.ddgi = enabled;
    }
    fn reflection_probes_enabled(&self) -> bool {
        self.reflection_probes
    }
    fn set_reflection_probes(&mut self, enabled: bool) {
        self.reflection_probes = enabled;
    }
    fn reflection_probes(&self) -> Vec<ReflectionProbe> {
        self.probes.clone()
    }
    fn skinning_enabled(&self) -> bool {
        self.skinning
    }
    fn set_skinning(&mut self, enabled: bool) {
        self.skinning = enabled;
    }

    fn rt_supported(&self) -> bool {
        self.rt_supported
    }
    fn rt_shadows_enabled(&self) -> bool {
        self.rt_shadows
    }
    fn set_rt_shadows(&mut self, enabled: bool) {
        self.rt_shadows = enabled;
    }
    fn restir_enabled(&self) -> bool {
        self.restir
    }
    fn set_restir(&mut self, enabled: bool) {
        self.restir = enabled;
    }
    fn rt_blas_count(&self) -> u32 {
        0
    }

    fn pipeline_count(&self) -> u32 {
        0
    }
    fn bindless_texture_count(&self) -> u32 {
        0
    }
    fn bindless_free_count(&self) -> u32 {
        0
    }

    fn view_mode(&self) -> ViewMode {
        self.view_mode
    }
    fn set_view_mode(&mut self, mode: ViewMode) {
        self.view_mode = mode;
    }

    fn aa_mode(&self) -> String {
        if self.aa_samples >= 2 {
            format!("msaa{}", self.aa_samples)
        } else if self.aa_fxaa {
            "fxaa".to_owned()
        } else if self.aa_taa {
            "taa".to_owned()
        } else {
            "off".to_owned()
        }
    }
    fn set_aa(&mut self, samples: u32, fxaa: bool, taa: bool) -> Result<(), String> {
        self.aa_samples = samples;
        self.aa_fxaa = fxaa;
        self.aa_taa = taa;
        Ok(())
    }

    fn exposure_ev(&self) -> f32 {
        self.exposure_ev
    }
    fn set_exposure(&mut self, ev: f32) {
        self.exposure_ev = ev;
    }

    fn profiler_mode(&self) -> ProfilerMode {
        self.profiler_mode
    }
    fn set_profiler_mode(&mut self, mode: ProfilerMode) {
        // Mirror the renderer clamp: unsupported modes fall back to Off.
        self.profiler_mode = if mode != ProfilerMode::Off && !self.timestamps_supported {
            ProfilerMode::Off
        } else {
            mode
        };
    }
    fn profiler_timestamps_supported(&self) -> bool {
        self.timestamps_supported
    }
    fn profiler_pipeline_stats_supported(&self) -> bool {
        self.pipeline_stats_supported
    }
    fn pass_timings(&self) -> Vec<PassTiming> {
        Vec::new()
    }
    fn pass_timings_total_ms(&self) -> f32 {
        0.0
    }

    fn start_profile_capture(
        &mut self,
        mode: CaptureMode,
        _frames: u32,
        _filter: String,
        _include_cpu: bool,
        _include_stats: bool,
    ) -> u32 {
        self.capture_mode = mode;
        self.capture_state = CaptureState::Arming;
        self.capture_id += 1;
        self.capture_id
    }
    fn stop_profile_capture(&mut self) -> ProfileCapture {
        self.capture_state = CaptureState::Idle;
        ProfileCapture::default()
    }
    fn profile_capture_mode(&self) -> CaptureMode {
        self.capture_mode
    }
    fn profile_capture_state(&self) -> CaptureState {
        self.capture_state
    }
    fn profile_capture_captured_frames(&self) -> u32 {
        0
    }
    fn profile_capture_target_frames(&self) -> u32 {
        1
    }

    fn frame_history_stats(&self) -> FrameHistoryStats {
        FrameHistoryStats::default()
    }
    fn frame_samples(&self, _max_samples: u32) -> Vec<FrameSample> {
        Vec::new()
    }
    fn perf_config(&self) -> PerfConfig {
        self.perf_config
    }
    fn set_perf_config(&mut self, config: PerfConfig) {
        self.perf_config = config.clamped();
    }

    fn drain_alarms(&self, _since: u64) -> AlarmDrain {
        AlarmDrain::default()
    }
    fn active_alarms(&self) -> Vec<ActiveAlarm> {
        Vec::new()
    }

    fn viewport_width(&self) -> u32 {
        self.width
    }
    fn viewport_height(&self) -> u32 {
        self.height
    }

    fn software_gpu(&self) -> bool {
        self.software_gpu
    }

    fn wait_gpu_idle(&mut self) {}

    fn set_active_view(&mut self, view: ViewId) {
        self.active_view = view;
        let (w, h) = self.view_sizes[view.index()];
        self.width = w;
        self.height = h;
    }
    fn view_desired_size(&self, view: ViewId) -> (u32, u32) {
        self.view_sizes[view.index()]
    }
    fn set_view_desired_size(
        &mut self,
        view: ViewId,
        width: u32,
        height: u32,
    ) -> Result<(), String> {
        if width == 0 || height == 0 {
            return Ok(());
        }
        self.view_sizes[view.index()] = (width, height);
        if view == self.active_view {
            self.width = width;
            self.height = height;
        }
        Ok(())
    }

    fn capture_viewport(&mut self, _path: &Path) -> Result<(), String> {
        Ok(())
    }

    fn request_window_capture(&mut self, _path: &Path) -> Result<(), String> {
        Ok(())
    }

    fn with_gpu_uploader(&mut self, with: &mut dyn FnMut(&dyn GpuUploader)) {
        with(&StubGpu);
    }

    fn with_thumbnail_gpu(&mut self, with: &mut dyn FnMut(&dyn ThumbnailGpu)) {
        with(&StubGpu);
    }

    fn render_settings_to_json(&self) -> Value {
        json!({})
    }

    fn apply_render_settings(&mut self, _settings: &Value) {}

    fn sa_lua_defs(&self) -> String {
        String::new()
    }
}

/// Runs `body` against a fresh `EngineContext` built from the cheaply constructible
/// subsystems (a headless window, a default editor context, an asset server rooted at a
/// temp dir) and the given renderer stub.
pub fn with_stub<T>(
    renderer: &mut StubRenderer,
    body: impl FnOnce(&mut EngineContext<'_>) -> T,
) -> T {
    let mut window = Window::headless();
    let mut scene_edit = SceneEditContext::new();
    let mut assets = AssetServer::new(std::env::temp_dir().join("saffron-control-test"));
    let mut ctx = EngineContext {
        window: &mut window,
        renderer,
        scene_edit: &mut scene_edit,
        assets: &mut assets,
        physics: None,
    };
    body(&mut ctx)
}
