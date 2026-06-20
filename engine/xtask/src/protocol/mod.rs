//! `xtask gen-protocol`: the TS / OpenRPC / manifest emitters that replace the C++ `gen.ts`.
//!
//! The DTO crate (`saffron-protocol`) is the single source of truth: its `ts-rs` derives give
//! the field metadata (via [`saffron_protocol::ts_decls`]) and its `schemars` fragments give the
//! OpenRPC per-DTO schemas (via [`saffron_protocol::struct_fragments`]). This module assembles
//! the three editor-facing artifacts byte-equivalently to `gen.ts`:
//!
//! - `editor/src/protocol/sa-types.ts` — reproduces `emitTs` (`gen.ts:1813`): header, the
//!   `WireUuid` alias, the hand-authored component-interfaces block, the command-reachable DTO
//!   interfaces in the `transitiveStructs` order, and the `CommandParamsMap`/`CommandResultMap`.
//! - `schemas/control/openrpc.generated.json` — reproduces `emitOpenRpc` (`gen.ts:2569`): the
//!   `{ openrpc, info, methods, components.schemas }` envelope, `methods` in command-table order,
//!   `components.schemas` = the sorted per-DTO fragments + the hand-authored component block.
//! - `schemas/control/command-manifest.generated.json` — reproduces `emitManifest`
//!   (`gen.ts:2598`): the fixture/skip ledger; the one intentional byte change is `generatedBy`.
//! - `schemas/control/sa.generated.luau` — the single Luau defs file: the `sa.*` API surface
//!   ([`luau::emit_api_defs`], from the `saffron-script` binding table) followed by the
//!   `:get_component` component snapshots ([`luau::emit_component_defs`], reproducing
//!   `emitScriptComponentDefs`, `gen.ts:3361`) — the `SaLuaDefs ++ SaComponentDefs` of
//!   `assets.cppm:1211`, generated from one source (NO LEGACY: no hand-written `library/sa.lua`
//!   overlay, no `check-script-defs` tripwire — the regen-freshness diff replaces it).
//!
//! Field declaration order is load-bearing (positional-CLI / OpenRPC-`required` order); ts-rs
//! and `serde_json`'s `preserve_order` keep it, so the outputs match the committed `gen.ts`
//! artifacts byte-for-byte.

use std::collections::HashMap;
use std::collections::HashSet;
use std::path::Path;

use anyhow::{Context, Result};
use saffron_protocol::{
    COMMANDS, SELECTOR_FIELDS, component_schemas, fixture_for, skip_for, struct_fragments, ts_decls,
};
use serde_json::{Map, Value, json};

pub mod luau;
mod ts;

/// The `generatedBy` string the manifest now carries — the one intentional byte change versus
/// the committed `gen.ts` artifact (asserted as the sole diff by the byte-equivalence test).
pub const GENERATED_BY: &str = "cargo run -p xtask -- gen-protocol";

/// The emitted artifacts, relative to the repo root, paired with their content.
pub struct Artifacts {
    /// `editor/src/protocol/sa-types.ts`.
    pub sa_types: String,
    /// `schemas/control/openrpc.generated.json`.
    pub openrpc: String,
    /// `schemas/control/command-manifest.generated.json`.
    pub manifest: String,
    /// `schemas/control/sa.generated.luau` — the single Luau defs file: the `sa.*` API surface
    /// plus the typed `:get_component` component snapshots, both from the one binding source.
    pub luau_defs: String,
}

/// Assemble the artifacts from the protocol crate (no I/O).
#[must_use]
pub fn emit() -> Artifacts {
    let decls = DtoDecls::load();
    Artifacts {
        sa_types: ts::emit_sa_types(&decls),
        openrpc: emit_openrpc(),
        manifest: emit_manifest(),
        luau_defs: luau::emit_defs(),
    }
}

/// Write the artifacts under `repo_root`, returning the list of paths written.
pub fn run(repo_root: &Path) -> Result<Vec<std::path::PathBuf>> {
    let artifacts = emit();
    let targets = [
        (
            repo_root.join("editor/src/protocol/sa-types.ts"),
            artifacts.sa_types,
        ),
        (
            repo_root.join("schemas/control/openrpc.generated.json"),
            artifacts.openrpc,
        ),
        (
            repo_root.join("schemas/control/command-manifest.generated.json"),
            artifacts.manifest,
        ),
        (
            repo_root.join("schemas/control/sa.generated.luau"),
            artifacts.luau_defs,
        ),
    ];
    let mut written = Vec::new();
    for (path, content) in targets {
        let parent = path
            .parent()
            .with_context(|| format!("artifact path has no parent: {}", path.display()))?;
        std::fs::create_dir_all(parent)
            .with_context(|| format!("create dir {}", parent.display()))?;
        std::fs::write(&path, content).with_context(|| format!("write {}", path.display()))?;
        written.push(path);
    }
    Ok(written)
}

/// The parsed `ts-rs` declarations, indexed by ident, plus the parsed enum-union strings — the
/// field-metadata model the TS emitter walks. This replaces `gen.ts`'s regex `StructDef`/
/// `EnumDef` parse: `ts-rs` is the parser, this only re-models its output.
pub struct DtoDecls {
    /// `ident -> parsed declaration` for every DTO type (structs + enums + `Uuid`).
    by_ident: HashMap<String, Decl>,
}

/// One parsed DTO declaration.
enum Decl {
    /// A struct: ordered fields `(name, ts-rs type token)`.
    Struct(Vec<(String, String)>),
    /// An enum / alias: the verbatim right-hand side (e.g. `"a" | "b"` or `string`).
    Alias(String),
}

impl DtoDecls {
    /// Parse every protocol DTO's `ts-rs` `decl()` into the field-metadata model.
    fn load() -> Self {
        let mut by_ident = HashMap::new();
        for (ident, decl) in ts_decls() {
            by_ident.insert(ident.to_owned(), parse_decl(&decl));
        }
        Self { by_ident }
    }

    fn get(&self, ident: &str) -> Option<&Decl> {
        self.by_ident.get(ident)
    }
}

/// Parse a `ts-rs` `type X = ...;` declaration into a [`Decl`]. A `{ ... }` object body becomes
/// ordered struct fields; `Record<string, never>` is an empty struct; anything else (an enum
/// union or the `string` alias) is kept verbatim as an [`Decl::Alias`].
fn parse_decl(decl: &str) -> Decl {
    let rhs = decl
        .split_once('=')
        .map(|(_, r)| r.trim().trim_end_matches(';').trim())
        .unwrap_or(decl);
    if rhs == "Record<string, never>" {
        return Decl::Struct(Vec::new());
    }
    if let Some(inner) = rhs.strip_prefix('{').and_then(|s| s.strip_suffix('}')) {
        return Decl::Struct(parse_fields(inner));
    }
    Decl::Alias(rhs.to_owned())
}

/// Split a `ts-rs` object body into ordered `(field, type)` pairs, dropping the `/** ... */`
/// doc-comment blocks ts-rs interleaves (the committed wire TS carries no field docs) and
/// honoring `<>`/`{}`/`[]` nesting when splitting on the top-level commas.
fn parse_fields(inner: &str) -> Vec<(String, String)> {
    let without_docs = strip_doc_comments(inner).replace('\n', " ");
    let mut fields = Vec::new();
    let mut depth = 0_i32;
    let mut current = String::new();
    for ch in without_docs.chars() {
        match ch {
            '<' | '{' | '[' => depth += 1,
            '>' | '}' | ']' => depth -= 1,
            _ => {}
        }
        if ch == ',' && depth == 0 {
            push_field(&mut fields, &current);
            current.clear();
        } else {
            current.push(ch);
        }
    }
    push_field(&mut fields, &current);
    fields
}

fn push_field(fields: &mut Vec<(String, String)>, raw: &str) {
    let field = raw.trim();
    if field.is_empty() {
        return;
    }
    if let Some((name, ty)) = field.split_once(':') {
        fields.push((name.trim().to_owned(), ty.trim().to_owned()));
    }
}

/// Remove every `/** ... */` block from a `ts-rs` body.
fn strip_doc_comments(input: &str) -> String {
    let mut out = String::with_capacity(input.len());
    let mut rest = input;
    while let Some(start) = rest.find("/**") {
        out.push_str(&rest[..start]);
        let after = &rest[start + 3..];
        if let Some(end) = after.find("*/") {
            rest = &after[end + 2..];
        } else {
            rest = "";
        }
    }
    out.push_str(rest);
    out
}

/// The OpenRPC document, byte-equivalent to `emitOpenRpc` (`gen.ts:2569`).
fn emit_openrpc() -> String {
    let mut schemas: Map<String, Value> = Map::new();

    // `schemaNames` = every struct fragment + the three wire-helper struct shapes, sorted by
    // name (`gen.ts:2570`). The fragments come from `schemars`; the wire-helpers are hand-emitted.
    let mut named: Vec<(String, Value)> = struct_fragments()
        .into_iter()
        .map(|(name, frag)| (name.to_owned(), frag))
        .collect();
    for (name, frag) in wire_helper_fragments() {
        named.push((name, frag));
    }
    named.sort_by(|a, b| a.0.cmp(&b.0));
    for (name, frag) in named {
        schemas.insert(name, frag);
    }
    // The hand-authored component block is spread last: a key already present keeps its sorted
    // position, a new key (the aggregates + `Environment`) appends — the JS object-spread order.
    for (name, frag) in component_schemas() {
        schemas.insert(name, frag);
    }

    let methods: Vec<Value> = COMMANDS
        .iter()
        .map(|cmd| {
            json!({
                "name": cmd.name,
                "summary": cmd.summary,
                "params": [{
                    "name": "params",
                    "schema": { "$ref": format!("#/components/schemas/{}", cmd.params) },
                }],
                "result": {
                    "name": "result",
                    "schema": { "$ref": format!("#/components/schemas/{}", cmd.result) },
                },
            })
        })
        .collect();

    let doc = json!({
        "openrpc": "1.3.2",
        "info": { "title": "Saffron Anima control DTOs", "version": "0.2.0" },
        "methods": methods,
        "components": { "schemas": Value::Object(schemas) },
    });
    pretty(&doc)
}

/// The three C++ wire-helper struct fragments (`gen.ts` `schemaFor` over the `{ <type> value; }`
/// structs): `WireUuid` is `{ value: integer }`, the two selectors are `{ value: {} }`. Rust
/// models these as a `string` alias / opaque `Value`, so their object shapes are hand-emitted.
fn wire_helper_fragments() -> Vec<(String, Value)> {
    vec![
        (
            "WireUuid".to_owned(),
            json!({
                "type": "object",
                "additionalProperties": false,
                "properties": { "value": { "type": "integer" } },
                "required": ["value"],
            }),
        ),
        (
            "EntitySelector".to_owned(),
            json!({
                "type": "object",
                "additionalProperties": false,
                "properties": { "value": {} },
                "required": ["value"],
            }),
        ),
        (
            "AssetSelector".to_owned(),
            json!({
                "type": "object",
                "additionalProperties": false,
                "properties": { "value": {} },
                "required": ["value"],
            }),
        ),
    ]
}

/// The command manifest, byte-equivalent to `emitManifest` (`gen.ts:2598`) modulo `generatedBy`.
/// Each command carries exactly one of a fixture or a skip; neither is a build error.
fn emit_manifest() -> String {
    emit_manifest_with(GENERATED_BY)
}

fn emit_manifest_with(generated_by: &str) -> String {
    let commands: Vec<Value> = COMMANDS
        .iter()
        .map(|cmd| {
            let mut entry = Map::new();
            entry.insert("name".into(), json!(cmd.name));
            entry.insert("params".into(), json!(cmd.params));
            entry.insert("result".into(), json!(cmd.result));
            entry.insert("status".into(), json!("typed"));
            match (fixture_for(cmd.name), skip_for(cmd.name)) {
                (Some(fixture), _) => {
                    entry.insert("fixture".into(), json!(fixture));
                }
                (None, Some(skip)) => {
                    entry.insert("skip".into(), json!(skip));
                }
                (None, None) => unreachable!(
                    "command '{}' has neither a fixture nor a skip (the table invariant a \
                     protocol test enforces)",
                    cmd.name
                ),
            }
            Value::Object(entry)
        })
        .collect();

    let doc = json!({
        "generatedBy": generated_by,
        "commands": commands,
        "skips": [{ "name": "help", "reason": "reflective registry" }],
    });
    pretty(&doc)
}

/// `JSON.stringify(doc, null, 2) + "\n"` — 2-space indent, a trailing newline, raw non-ASCII
/// (the em-dash in command summaries stays a literal UTF-8 byte, matching `gen.ts`).
fn pretty(value: &Value) -> String {
    let mut buf = Vec::new();
    let formatter = serde_json::ser::PrettyFormatter::with_indent(b"  ");
    let mut ser = serde_json::Serializer::with_formatter(&mut buf, formatter);
    serde::Serialize::serialize(value, &mut ser).expect("json value serializes");
    let mut text = String::from_utf8(buf).expect("serde_json emits utf-8");
    text.push('\n');
    text
}

/// `commandTypeNames` (`gen.ts:1247`): the deduped `[params, result]` of every command, in
/// table order — the TS interface-walk roots.
fn command_type_names() -> Vec<&'static str> {
    let mut seen = HashSet::new();
    let mut out = Vec::new();
    for cmd in COMMANDS {
        for ty in [cmd.params, cmd.result] {
            if seen.insert(ty) {
                out.push(ty);
            }
        }
    }
    out
}

/// `(struct, field)` pairs whose `serde_json::Value` field is a selector — the TS mapping
/// reuses the protocol crate's [`SELECTOR_FIELDS`] so it never drifts from the schema emitter.
fn selector_fields() -> HashSet<(&'static str, &'static str)> {
    SELECTOR_FIELDS.iter().copied().collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The committed `gen.ts` `generatedBy`, used only to diff the two manifests modulo the one
    /// intentional change.
    const LEGACY_GENERATED_BY: &str = "tools/gen-control-dto/gen.ts";

    fn repo_root() -> std::path::PathBuf {
        // `engine/xtask/` -> repo root is three parents up.
        std::path::PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .parent()
            .and_then(Path::parent)
            .map(Path::to_path_buf)
            .expect("xtask manifest dir resolves to the repo root")
    }

    #[test]
    fn sa_types_is_byte_identical_to_committed() {
        let committed =
            std::fs::read_to_string(repo_root().join("editor/src/protocol/sa-types.ts"))
                .expect("committed sa-types.ts");
        assert_eq!(emit().sa_types, committed);
    }

    #[test]
    fn luau_defs_is_byte_identical_to_committed() {
        let committed =
            std::fs::read_to_string(repo_root().join("schemas/control/sa.generated.luau"))
                .expect("committed sa.generated.luau");
        assert_eq!(emit().luau_defs, committed);
    }

    /// NO LEGACY: with the `sa.*` defs generated from one source, the hand-written
    /// `library/sa.lua` overlay, the C++ `SaLuaDefs`/`SaComponentDefs` blobs, the stale
    /// components-only `.luau` artifact, and the `check-script-defs` drift tripwire must not
    /// exist anywhere in the Rust tree (the `engine-old/` C++ reference is exempt).
    #[test]
    fn no_legacy_overlay_or_tripwire() {
        let root = repo_root();
        for absent in [
            "tools/check-script-defs",
            "schemas/control/sa-components.generated.luau",
            "editor/library/sa.lua",
        ] {
            assert!(
                !root.join(absent).exists(),
                "legacy artifact must not exist in the Rust tree: {absent}"
            );
        }
        // No `sa.lua` overlay committed under the Rust trees (a generated `.luau` def file is
        // the only Lua-type artifact); the C++ reference under `engine-old/` is exempt.
        for tree in [
            "engine/crates",
            "engine/xtask",
            "editor/src",
            "schemas",
            "tools",
        ] {
            for entry in walk(&root.join(tree)) {
                let name = entry.file_name().and_then(|n| n.to_str()).unwrap_or("");
                assert_ne!(
                    name,
                    "sa.lua",
                    "a hand-written sa.lua overlay must not exist: {}",
                    entry.display()
                );
            }
        }
    }

    /// A small recursive file walk (no external crate), skipping `node_modules`/`target`.
    fn walk(dir: &Path) -> Vec<std::path::PathBuf> {
        let mut out = Vec::new();
        let Ok(read) = std::fs::read_dir(dir) else {
            return out;
        };
        for entry in read.flatten() {
            let path = entry.path();
            let name = path.file_name().and_then(|n| n.to_str()).unwrap_or("");
            if name == "node_modules" || name == "target" {
                continue;
            }
            if path.is_dir() {
                out.extend(walk(&path));
            } else {
                out.push(path);
            }
        }
        out
    }

    #[test]
    fn openrpc_is_byte_identical_to_committed() {
        let committed =
            std::fs::read_to_string(repo_root().join("schemas/control/openrpc.generated.json"))
                .expect("committed openrpc.generated.json");
        assert_eq!(emit().openrpc, committed);
    }

    /// Normalize whichever `generatedBy` a manifest carries to the legacy string, so a manifest
    /// emitted by the xtask and one emitted by the old `gen.ts` compare equal modulo that one
    /// field — the only intended byte change. A no-op when the legacy string is already present.
    fn normalize_generated_by(manifest: &str) -> String {
        manifest.replacen(GENERATED_BY, LEGACY_GENERATED_BY, 1)
    }

    #[test]
    fn manifest_differs_only_in_generated_by() {
        // The committed manifest is byte-identical to the live emit except for `generatedBy` —
        // robust to whether the committed file still carries the legacy `gen.ts` string or the
        // regenerated xtask string.
        let committed = std::fs::read_to_string(
            repo_root().join("schemas/control/command-manifest.generated.json"),
        )
        .expect("committed command-manifest.generated.json");
        assert_eq!(
            normalize_generated_by(&emit().manifest),
            normalize_generated_by(&committed),
            "the manifest must differ from the committed artifact only in `generatedBy`"
        );

        // The legacy-stamped emit reproduces the legacy `gen.ts` artifact byte-for-byte.
        assert_eq!(
            normalize_generated_by(&emit_manifest_with(LEGACY_GENERATED_BY)),
            normalize_generated_by(&committed),
        );
        assert!(emit().manifest.contains(GENERATED_BY));
    }
}
