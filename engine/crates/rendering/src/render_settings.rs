//! The project-file `renderSettings` block: the renderer's render-panel state as JSON.
//!
//! Ports `renderSettingsToJson` / `applyRenderSettings` (`assets.cppm:922`/`:940`). The
//! save path serializes the AA mode, exposure, and the feature toggles; the load path
//! applies a saved block, leaving any missing field at its current value and applying the
//! RT toggles only where the device supports ray tracing (so a project authored on an RT
//! machine loads cleanly on a software one). One block, one schema — the project document
//! round-trips it unchanged through save/load.

use serde_json::{Value, json};

use crate::Renderer;

/// The render-panel state, gathered from the renderer's getters or parsed from a saved
/// `renderSettings` block. A field of `None` in a parse means "absent → keep current".
#[derive(Clone, Debug, Default, PartialEq)]
pub struct RenderSettings {
    /// The AA mode name (`"off"` / `"fxaa"` / `"taa"` / `"msaaN"`).
    pub aa: Option<String>,
    /// The tonemap exposure in stops.
    pub exposure_ev: Option<f32>,
    /// Clustered forward lighting on.
    pub clustered: Option<bool>,
    /// The depth pre-pass on.
    pub depth_prepass: Option<bool>,
    /// Shadow maps on.
    pub shadows: Option<bool>,
    /// Image-based lighting on.
    pub ibl: Option<bool>,
    /// Ground-truth ambient occlusion on.
    pub ssao: Option<bool>,
    /// Contact (screen-space) shadows on.
    pub contact_shadows: Option<bool>,
    /// Screen-space GI on.
    pub ssgi: Option<bool>,
    /// Dynamic diffuse GI on.
    pub ddgi: Option<bool>,
    /// Ray-traced shadows on (applied only on RT hardware).
    pub rt_shadows: Option<bool>,
    /// ReSTIR DI on (applied only on RT hardware).
    pub restir: Option<bool>,
}

/// Serializes a fully-populated [`RenderSettings`] (every field `Some`) to the project
/// `renderSettings` block. The pure half of [`Renderer::render_settings_to_json`], so the
/// frozen key schema is unit-testable without a device.
fn settings_to_json(s: &RenderSettings) -> Value {
    json!({
        "aa": s.aa,
        "exposureEv": s.exposure_ev,
        "clustered": s.clustered,
        "depthPrepass": s.depth_prepass,
        "shadows": s.shadows,
        "ibl": s.ibl,
        "ssao": s.ssao,
        "contactShadows": s.contact_shadows,
        "ssgi": s.ssgi,
        "ddgi": s.ddgi,
        "rtShadows": s.rt_shadows,
        "restir": s.restir,
    })
}

/// Parses a saved `renderSettings` block into a [`RenderSettings`] patch: a missing or
/// wrong-typed field stays `None` (→ keep current). A non-object value parses to an
/// all-`None` patch (a no-op). The pure half of [`Renderer::apply_render_settings`].
fn parse_render_settings(settings: &Value) -> RenderSettings {
    let mut patch = RenderSettings::default();
    let Some(obj) = settings.as_object() else {
        return patch;
    };
    patch.aa = obj.get("aa").and_then(Value::as_str).map(str::to_owned);
    patch.exposure_ev = obj
        .get("exposureEv")
        .and_then(Value::as_f64)
        .map(|v| v as f32);
    let b = |key: &str| obj.get(key).and_then(Value::as_bool);
    patch.clustered = b("clustered");
    patch.depth_prepass = b("depthPrepass");
    patch.shadows = b("shadows");
    patch.ibl = b("ibl");
    patch.ssao = b("ssao");
    patch.contact_shadows = b("contactShadows");
    patch.ssgi = b("ssgi");
    patch.ddgi = b("ddgi");
    patch.rt_shadows = b("rtShadows");
    patch.restir = b("restir");
    patch
}

impl Renderer {
    /// Serializes the renderer's render-panel settings as the project-file
    /// `renderSettings` block. The C++ `renderSettingsToJson`.
    pub fn render_settings_to_json(&self) -> Value {
        settings_to_json(&RenderSettings {
            aa: Some(self.aa_mode()),
            exposure_ev: Some(self.exposure_ev()),
            clustered: Some(self.clustered_enabled()),
            depth_prepass: Some(self.depth_prepass_enabled()),
            shadows: Some(self.shadows_enabled()),
            ibl: Some(self.ibl_enabled()),
            ssao: Some(self.ssao_enabled()),
            contact_shadows: Some(self.contact_shadows_enabled()),
            ssgi: Some(self.ssgi_enabled()),
            ddgi: Some(self.ddgi_enabled()),
            rt_shadows: Some(self.rt_shadows_enabled()),
            restir: Some(self.restir_enabled()),
        })
    }

    /// Applies a saved `renderSettings` block: a missing field keeps the current value,
    /// and the RT toggles apply only where the device supports ray tracing. A non-object
    /// value (or a wrong-typed field) is ignored field-by-field, so a malformed block
    /// degrades to "keep current". The C++ `applyRenderSettings`.
    pub fn apply_render_settings(&mut self, settings: &Value) {
        let patch = parse_render_settings(settings);
        if let Some(aa) = &patch.aa {
            // The AA mode setter idles + rebuilds the active view's AA targets; a bad name
            // falls back to "off" inside the setter, matching the C++ `setAaMode`.
            let _ = self.set_aa_mode(aa);
        }
        if let Some(ev) = patch.exposure_ev {
            self.set_exposure(ev);
        }
        if let Some(v) = patch.clustered {
            self.set_clustered(v);
        }
        if let Some(v) = patch.depth_prepass {
            self.set_depth_prepass(v);
        }
        if let Some(v) = patch.shadows {
            self.set_shadows(v);
        }
        if let Some(v) = patch.ibl {
            self.set_ibl(v);
        }
        if let Some(v) = patch.ssao {
            self.set_ssao(v);
        }
        if let Some(v) = patch.contact_shadows {
            self.set_contact_shadows(v);
        }
        if let Some(v) = patch.ssgi {
            self.set_ssgi(v);
        }
        if let Some(v) = patch.ddgi {
            self.set_ddgi(v);
        }
        if self.rt_supported() {
            if let Some(v) = patch.rt_shadows {
                self.set_rt_shadows(v);
            }
            if let Some(v) = patch.restir {
                self.set_restir(v);
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    /// The serialized block carries every render-panel field with the frozen project keys
    /// (matching the C++ `renderSettingsToJson` schema the editor's render panel reads).
    /// Pure logic — no device needed (a headless `Renderer::new` crashes lavapipe's WSI).
    #[test]
    fn render_settings_block_has_the_frozen_keys() {
        let settings = RenderSettings {
            aa: Some("msaa4".to_owned()),
            exposure_ev: Some(0.5),
            clustered: Some(true),
            depth_prepass: Some(false),
            shadows: Some(true),
            ibl: Some(true),
            ssao: Some(false),
            contact_shadows: Some(true),
            ssgi: Some(false),
            ddgi: Some(true),
            rt_shadows: Some(false),
            restir: Some(false),
        };
        let block = settings_to_json(&settings);
        let obj = block.as_object().expect("renderSettings is an object");
        for key in [
            "aa",
            "exposureEv",
            "clustered",
            "depthPrepass",
            "shadows",
            "ibl",
            "ssao",
            "contactShadows",
            "ssgi",
            "ddgi",
            "rtShadows",
            "restir",
        ] {
            assert!(obj.contains_key(key), "renderSettings carries '{key}'");
        }
        assert_eq!(obj["aa"], json!("msaa4"), "aa is the mode name");
        assert_eq!(obj["exposureEv"].as_f64().unwrap(), 0.5);
        assert_eq!(obj["clustered"], json!(true));
        assert_eq!(obj["shadows"], json!(true));
    }

    /// Parsing a saved block then re-serializing reproduces it — the project save/load
    /// round-trip over the pure serde halves (the `Renderer` wrappers add only the
    /// getter/setter plumbing the device-gated render tests already exercise).
    #[test]
    fn parse_then_serialize_round_trips_every_field() {
        let saved = json!({
            "aa": "fxaa",
            "exposureEv": 1.5,
            "clustered": false,
            "depthPrepass": true,
            "shadows": false,
            "ibl": false,
            "ssao": true,
            "contactShadows": true,
            "ssgi": true,
            "ddgi": true,
            "rtShadows": true,
            "restir": false,
        });
        let patch = parse_render_settings(&saved);
        // Every field parsed.
        assert_eq!(patch.aa.as_deref(), Some("fxaa"));
        assert_eq!(patch.exposure_ev, Some(1.5));
        assert_eq!(patch.clustered, Some(false));
        assert_eq!(patch.depth_prepass, Some(true));
        assert_eq!(patch.shadows, Some(false));
        assert_eq!(patch.ibl, Some(false));
        assert_eq!(patch.ssao, Some(true));
        assert_eq!(patch.contact_shadows, Some(true));
        assert_eq!(patch.ssgi, Some(true));
        assert_eq!(patch.ddgi, Some(true));
        assert_eq!(patch.rt_shadows, Some(true));
        assert_eq!(patch.restir, Some(false));

        // Re-serializing the fully-populated patch reproduces the saved block.
        assert_eq!(settings_to_json(&patch), saved);
    }

    /// A missing field parses to `None` (→ keep current); a non-object block is an
    /// all-`None` patch (a no-op); a wrong-typed field is ignored field-by-field.
    #[test]
    fn missing_and_malformed_fields_parse_to_none() {
        // A block touching only `ssao` leaves every other field `None`.
        let patch = parse_render_settings(&json!({ "ssao": true }));
        assert_eq!(patch.ssao, Some(true), "ssao parsed");
        assert_eq!(patch.shadows, None, "absent → keep current");
        assert_eq!(patch.aa, None);

        // A non-object block parses to an all-`None` patch.
        assert_eq!(
            parse_render_settings(&json!("not an object")),
            RenderSettings::default(),
            "a non-object block is a no-op"
        );

        // A wrong-typed field is ignored (a string where a bool is expected).
        let patch = parse_render_settings(&json!({ "shadows": "yes" }));
        assert_eq!(
            patch.shadows, None,
            "a string for a boolean field is ignored"
        );
    }
}
