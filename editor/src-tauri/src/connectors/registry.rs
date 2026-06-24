//! The connector registry: the set of connectors the editor knows about. Phase 1 enables
//! Poly Haven by default; Phase 2 makes the enabled set per-project.

use std::sync::Arc;

use serde::Serialize;

use super::{
    AuthKind, StoreConnector, ambientcg::AmbientCg, polyhaven::PolyHaven, polypizza::PolyPizza,
    sketchfab::Sketchfab,
};

/// A connector's identity + state, surfaced to the webview.
#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ConnectorInfo {
    pub id: String,
    pub display_name: String,
    pub auth_kind: AuthKind,
    pub description: String,
    pub website: String,
    pub enabled: bool,
}

pub struct ConnectorRegistry {
    connectors: Vec<Arc<dyn StoreConnector>>,
}

impl ConnectorRegistry {
    pub fn new(http: reqwest::Client) -> Self {
        let connectors: Vec<Arc<dyn StoreConnector>> = vec![
            Arc::new(PolyHaven::new(http.clone())),
            Arc::new(AmbientCg::new(http.clone())),
            Arc::new(PolyPizza::new(http.clone())),
            Arc::new(Sketchfab::new(http)),
        ];
        Self { connectors }
    }

    /// Every available connector. Per-project enablement is applied by the caller via the
    /// `providers` scope on a search (the editor passes the project's enabled set).
    pub fn enabled(&self) -> &[Arc<dyn StoreConnector>] {
        &self.connectors
    }

    pub fn by_id(&self, id: &str) -> Option<Arc<dyn StoreConnector>> {
        self.connectors.iter().find(|c| c.id() == id).cloned()
    }

    pub fn infos(&self) -> Vec<ConnectorInfo> {
        self.connectors
            .iter()
            .map(|c| ConnectorInfo {
                id: c.id().to_owned(),
                display_name: c.display_name().to_owned(),
                auth_kind: c.auth_kind(),
                description: c.description().to_owned(),
                website: c.website().to_owned(),
                enabled: true,
            })
            .collect()
    }
}
