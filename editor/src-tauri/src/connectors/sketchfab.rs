//! The Sketchfab connector — `AuthKind::OauthLoopback`, the largest catalog. Data API v3
//! search plus the Download API, authorized by an implicit-flow token from the loopback
//! capability. A 401 (the monthly token expiry) surfaces as "not configured" so the editor
//! re-runs the login.
//!
//! Sketchfab is now an Epic/Fab property folding into Fab, so the standalone API's longevity
//! is a known risk.

use std::path::PathBuf;

use async_trait::async_trait;
use serde_json::Value;

use super::{
    AuthKind, ConnectorError, Credentials, OAuthLoopbackConfig, SearchPage, SearchQuery,
    StoreConnector, StoreCursor, StoreImportDescriptor, StoreKind, StoreLicense, StoreRef,
    StoreResult, store_cache_dir, user_agent,
};

const API_BASE: &str = "https://api.sketchfab.com/v3";

pub struct Sketchfab {
    http: reqwest::Client,
}

impl Sketchfab {
    pub fn new(http: reqwest::Client) -> Self {
        Self { http }
    }

    fn token(&self) -> Result<String, ConnectorError> {
        Credentials::global()
            .get_secret(self.id())
            .filter(|t| !t.is_empty())
            .ok_or_else(|| ConnectorError::NotConfigured(self.id().to_owned()))
    }

    fn to_result(&self, item: &Value) -> Option<StoreResult> {
        let uid = item.get("uid").and_then(Value::as_str)?;
        let name = item
            .get("name")
            .and_then(Value::as_str)
            .unwrap_or(uid)
            .to_owned();
        let author = item
            .get("user")
            .and_then(|u| u.get("displayName").or_else(|| u.get("username")))
            .and_then(Value::as_str)
            .unwrap_or_default()
            .to_owned();
        let thumbnail = item
            .get("thumbnails")
            .and_then(|t| t.get("images"))
            .and_then(Value::as_array)
            .and_then(|imgs| imgs.first())
            .and_then(|img| img.get("url"))
            .and_then(Value::as_str)
            .unwrap_or_default()
            .to_owned();
        let license = license_from(item.get("license"));
        Some(StoreResult {
            id: uid.to_owned(),
            store: StoreRef {
                id: self.id().to_owned(),
                display_name: self.display_name().to_owned(),
            },
            kind: StoreKind::Model,
            name,
            author,
            thumbnail_url: thumbnail,
            source_url: item
                .get("viewerUrl")
                .and_then(Value::as_str)
                .map(str::to_owned)
                .unwrap_or_else(|| format!("https://sketchfab.com/3d-models/{uid}")),
            license,
            import_descriptor: StoreImportDescriptor {
                format: "auto".to_owned(),
                ref_: uid.to_owned(),
                resolution: None,
            },
            has_parts: false,
            supports_resolution: false,
            published_at: item
                .get("publishedAt")
                .and_then(Value::as_str)
                .map(str::to_owned),
            updated_at: None,
            tri_count: item.get("faceCount").and_then(Value::as_u64),
            file_size: None,
            resolution: None,
            tags: None,
        })
    }
}

#[async_trait]
impl StoreConnector for Sketchfab {
    fn id(&self) -> &'static str {
        "sketchfab"
    }

    fn display_name(&self) -> &'static str {
        "Sketchfab"
    }

    fn auth_kind(&self) -> AuthKind {
        AuthKind::OauthLoopback
    }

    fn description(&self) -> &'static str {
        "700k+ downloadable Creative Commons models. Sign in with your Sketchfab account."
    }

    fn website(&self) -> &'static str {
        "https://sketchfab.com"
    }

    fn oauth_config(&self) -> Option<OAuthLoopbackConfig> {
        Some(OAuthLoopbackConfig {
            authorize_url: "https://sketchfab.com/oauth2/authorize/".to_owned(),
            token_url: "https://sketchfab.com/oauth2/token/".to_owned(),
            client_id: std::env::var("SAFFRON_SKETCHFAB_CLIENT_ID").unwrap_or_default(),
            scope: String::new(),
            response_type: "token".to_owned(),
            keyring_key: self.id().to_owned(),
        })
    }

    async fn search(
        &self,
        query: &SearchQuery,
        cursor: Option<StoreCursor>,
    ) -> Result<SearchPage, ConnectorError> {
        if matches!(query.kind, Some(k) if k != StoreKind::Model) {
            return Ok(SearchPage {
                results: Vec::new(),
                next_cursor: None,
                exhausted: true,
            });
        }
        let token = self.token()?;
        let mut req = self
            .http
            .get(format!("{API_BASE}/search"))
            .query(&[
                ("type", "models"),
                ("downloadable", "true"),
                ("q", query.text.trim()),
                ("count", "24"),
            ])
            .header(reqwest::header::AUTHORIZATION, format!("Bearer {token}"))
            .header(reqwest::header::USER_AGENT, user_agent());
        // The v3 cursor is an opaque offset token echoed back as `cursor`.
        if let Some(StoreCursor(c)) = &cursor {
            req = req.query(&[("cursor", c.as_str())]);
        }
        let resp = req
            .send()
            .await
            .map_err(|e| ConnectorError::Http(e.to_string()))?;
        if resp.status() == reqwest::StatusCode::UNAUTHORIZED {
            return Err(ConnectorError::NotConfigured(self.id().to_owned()));
        }
        if !resp.status().is_success() {
            return Err(ConnectorError::Http(format!("{}", resp.status())));
        }
        let body: Value = resp
            .json()
            .await
            .map_err(|e| ConnectorError::Decode(e.to_string()))?;
        let results: Vec<StoreResult> = body
            .get("results")
            .and_then(Value::as_array)
            .map(|items| items.iter().filter_map(|it| self.to_result(it)).collect())
            .unwrap_or_default();
        let next = body
            .get("cursors")
            .and_then(|c| c.get("next"))
            .and_then(Value::as_str)
            .filter(|s| !s.is_empty())
            .map(|s| StoreCursor(s.to_owned()));
        Ok(SearchPage {
            exhausted: next.is_none(),
            next_cursor: next,
            results,
        })
    }

    async fn download(
        &self,
        descriptor: &StoreImportDescriptor,
        progress: &super::ProgressFn,
    ) -> Result<PathBuf, ConnectorError> {
        let token = self.token()?;
        let uid = &descriptor.ref_;
        let resp = self
            .http
            .get(format!("{API_BASE}/models/{uid}/download"))
            .header(reqwest::header::AUTHORIZATION, format!("Bearer {token}"))
            .header(reqwest::header::USER_AGENT, user_agent())
            .send()
            .await
            .map_err(|e| ConnectorError::Download(e.to_string()))?;
        if resp.status() == reqwest::StatusCode::UNAUTHORIZED {
            return Err(ConnectorError::NotConfigured(self.id().to_owned()));
        }
        if !resp.status().is_success() {
            return Err(ConnectorError::Download(format!("{}", resp.status())));
        }
        let body: Value = resp
            .json()
            .await
            .map_err(|e| ConnectorError::Decode(e.to_string()))?;
        // Prefer a self-contained GLB; fall back to glTF, then USDZ.
        let (ext, url) = ["glb", "gltf", "usdz"]
            .iter()
            .find_map(|fmt| {
                body.get(fmt)
                    .and_then(|v| v.get("url"))
                    .and_then(Value::as_str)
                    .map(|u| (*fmt, u.to_owned()))
            })
            .ok_or_else(|| ConnectorError::UnsupportedFormat("no deliverable".to_owned()))?;

        let bytes = super::stream_get(&self.http, &url, |done, total| {
            if let Some(t) = total.filter(|t| *t > 0) {
                progress(done as f64 / t as f64);
            }
        })
        .await?;
        let dir = store_cache_dir().join("sketchfab");
        std::fs::create_dir_all(&dir).map_err(|e| ConnectorError::Download(e.to_string()))?;
        let dest = dir.join(format!("{uid}.{ext}"));
        std::fs::write(&dest, &bytes).map_err(|e| ConnectorError::Download(e.to_string()))?;
        Ok(dest)
    }
}

/// Maps a Sketchfab license object to the canonical structured license.
fn license_from(license: Option<&Value>) -> StoreLicense {
    let slug = license
        .and_then(|l| l.get("slug"))
        .and_then(Value::as_str)
        .unwrap_or("")
        .to_lowercase();
    let url = license
        .and_then(|l| l.get("url"))
        .and_then(Value::as_str)
        .unwrap_or("")
        .to_owned();
    if slug == "cc0" {
        StoreLicense {
            id: "cc0".to_owned(),
            requires_attribution: false,
            url: if url.is_empty() {
                "https://creativecommons.org/publicdomain/zero/1.0/".to_owned()
            } else {
                url
            },
        }
    } else {
        StoreLicense {
            id: if slug.is_empty() {
                "cc-by".to_owned()
            } else {
                slug
            },
            requires_attribution: true,
            url,
        }
    }
}
