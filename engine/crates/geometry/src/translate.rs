//! The model-format dispatch on file extension.
//!
//! `.gltf`/`.glb` route to the glTF importer, `.obj` to the OBJ importer; any other
//! extension is rejected.

use std::path::Path;

use crate::error::{Error, Result};
use crate::gltf_import::import_gltf_model;
use crate::obj_import::import_obj_model;
use crate::types::ImportedModel;

/// Translate a source model (`.gltf`/`.glb`/`.obj`) into the in-memory import graph,
/// dispatching on the file extension (case-insensitive).
pub fn translate_model(source: impl AsRef<Path>) -> Result<ImportedModel> {
    let source = source.as_ref();
    let ext = source
        .extension()
        .and_then(|e| e.to_str())
        .map(str::to_ascii_lowercase)
        .unwrap_or_default();
    match ext.as_str() {
        "gltf" | "glb" => import_gltf_model(source),
        "obj" => import_obj_model(source),
        _ => Err(Error::Import(format!(
            "unsupported model format: '{}' (expected .gltf/.glb/.obj)",
            source.display()
        ))),
    }
}
