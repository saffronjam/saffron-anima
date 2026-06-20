//! The `sa-types.ts` emitter — reproduces `emitTs` (`gen.ts:1813`) over the `ts-rs` decls.

use std::collections::HashSet;

use super::{Decl, DtoDecls, command_type_names, selector_fields};

/// The hand-authored component-interfaces block (`gen.ts`'s `componentInterfaces` literal): the
/// 21 component shapes + `Components` + `ComponentBody`. Verbatim, so it stays byte-identical.
const COMPONENT_BLOCK: &str = include_str!("component_block.ts");

/// The hand-authored `EnvironmentDto` interface (`gen.ts:1825`): the Rust DTO is opaque
/// (`{ value: Value }`), so its wire-shaped interface is emitted verbatim, not from `ts-rs`.
const ENVIRONMENT_DTO: &str = include_str!("environment_dto.ts");

/// Build `editor/src/protocol/sa-types.ts`: header, `WireUuid` alias, the component block, the
/// command-reachable interfaces in `transitiveStructs` order, and the two command maps.
pub fn emit_sa_types(decls: &DtoDecls) -> String {
    let names = interface_order(decls);
    let interfaces = names
        .iter()
        .map(|name| emit_interface(decls, name))
        .collect::<Vec<_>>()
        .join("\n\n");

    let params_map = super::COMMANDS
        .iter()
        .map(|cmd| format!("  {:?}: {};", cmd.name, cmd.params))
        .collect::<Vec<_>>()
        .join("\n");
    let result_map = super::COMMANDS
        .iter()
        .map(|cmd| format!("  {:?}: {};", cmd.name, cmd.result))
        .collect::<Vec<_>>()
        .join("\n");

    format!(
        "/**\n * GENERATED - do not edit.\n *\n * Produced by tools/gen-control-dto/gen.ts \
         from control_dto.cppm.\n */\n\nexport type WireUuid = string;\n\n{}\n\n{}\n\nexport \
         interface CommandParamsMap {{\n{}\n}}\n\nexport interface CommandResultMap \
         {{\n{}\n}}\n",
        COMPONENT_BLOCK.trim_end_matches('\n'),
        interfaces,
        params_map,
        result_map,
    )
}

/// The interface emission order: `EntityRef` first, then `transitiveStructs` over the command
/// roots + `Vec3`/`Vec4`/`ProbeRef` (`gen.ts:1814`), deduped, struct-only.
fn interface_order(decls: &DtoDecls) -> Vec<String> {
    let mut roots = command_type_names();
    roots.extend(["Vec3", "Vec4", "ProbeRef"]);

    let mut seen = HashSet::new();
    let mut order = Vec::new();
    seen.insert("EntityRef".to_owned());
    order.push("EntityRef".to_owned());
    for root in roots {
        for dep in struct_deps(decls, root) {
            if seen.insert(dep.clone()) {
                order.push(dep);
            }
        }
    }
    order
}

/// `structDeps` (`gen.ts:1251`): a DFS pre-order of the struct types reachable from `ty`'s
/// fields (unwrapping `Array<...>` and ` | null`), itself included; non-structs return empty.
fn struct_deps(decls: &DtoDecls, ty: &str) -> Vec<String> {
    let mut out = Vec::new();
    let inner = unwrap_array(strip_nullable(ty));
    // Aliases (enums, the `Uuid` = `string` newtype) and primitives are not struct nodes.
    if let Some(Decl::Struct(fields)) = decls.get(inner) {
        out.push(inner.to_owned());
        for (_, field_ty) in fields {
            out.extend(struct_deps(decls, field_ty));
        }
    }
    out
}

/// One interface: `EnvironmentDto` is verbatim; an empty struct is `{\n\n}` (the `gen.ts`
/// empty-body shape); otherwise one `  name(?): type;` line per field in declaration order.
fn emit_interface(decls: &DtoDecls, name: &str) -> String {
    if name == "EnvironmentDto" {
        return ENVIRONMENT_DTO.trim_end_matches('\n').to_owned();
    }
    let Some(Decl::Struct(fields)) = decls.get(name) else {
        panic!("interface-order type {name} is not a struct declaration");
    };
    let body = fields
        .iter()
        .map(|(field, ty)| {
            let (mapped, optional) = ts_type(ty, name, field);
            format!("  {field}{}: {mapped};", if optional { "?" } else { "" })
        })
        .collect::<Vec<_>>()
        .join("\n");
    format!("export interface {name} {{\n{body}\n}}")
}

/// Map a `ts-rs` type token to the `gen.ts` `tsType` spelling, returning `(type, optional)`.
/// `T | null` is optional; `Array<T>` -> `T[]`; `bigint` -> `number`; `Uuid` -> `WireUuid`;
/// `JsonValue` -> a selector union when `(struct, field)` is a selector, else `unknown`; an
/// enum ident inlines its `"a" | "b"` union; structs/`Vec3`/etc. pass through.
fn ts_type(ty: &str, struct_name: &str, field: &str) -> (String, bool) {
    let (core, optional) = match ty.strip_suffix("| null") {
        Some(inner) => (inner.trim(), true),
        None => (ty.trim(), false),
    };

    // The two cross-boundary special-cases (`gen.ts:1843`/`:1846`).
    if struct_name == "InspectResult" && field == "components" {
        return ("Components".to_owned(), optional);
    }
    if struct_name == "SetComponentParams" && field == "json" {
        return ("ComponentBody".to_owned(), optional);
    }

    if let Some(item) = core
        .strip_prefix("Array<")
        .and_then(|s| s.strip_suffix('>'))
    {
        let (mapped, _) = ts_type(item, struct_name, field);
        return (format!("{mapped}[]"), optional);
    }
    match core {
        "bigint" => ("number".to_owned(), optional),
        "Uuid" => ("WireUuid".to_owned(), optional),
        "JsonValue" => {
            if is_selector_field(struct_name, field) {
                ("WireUuid | string | number".to_owned(), optional)
            } else {
                ("unknown".to_owned(), optional)
            }
        }
        other => (resolve_enum_or_passthrough(other), optional),
    }
}

/// Whether `(struct, field)` is a selector field — reusing the protocol crate's
/// [`selector_fields`] set so the TS mapping never drifts from the schema emitter.
fn is_selector_field(struct_name: &str, field: &str) -> bool {
    selector_fields()
        .iter()
        .any(|(s, f)| *s == struct_name && *f == field)
}

/// An enum ident inlines to its `"a" | "b"` union (looked up from the protocol enum decls); any
/// other name (a struct, `Vec3`, `number`, `boolean`, `string`) passes through unchanged.
fn resolve_enum_or_passthrough(name: &str) -> String {
    if let Some(union) = ENUM_UNIONS.with(|m| m.get(name).cloned()) {
        return union;
    }
    name.to_owned()
}

thread_local! {
    /// The enum ident -> `"a" | "b"` union map, parsed once from the protocol `ts-rs` decls.
    static ENUM_UNIONS: std::collections::HashMap<String, String> = enum_unions();
}

fn enum_unions() -> std::collections::HashMap<String, String> {
    let mut map = std::collections::HashMap::new();
    for (ident, decl) in saffron_protocol::ts_decls() {
        if let Decl::Alias(rhs) = super::parse_decl(&decl) {
            // The `Uuid` alias (`string`) is not an enum union; only keep `"..."`-led unions.
            if rhs.starts_with('"') {
                map.insert(ident.to_owned(), rhs);
            }
        }
    }
    map
}

/// `T | null` -> `T`; a bare `T` passes through (the nullable marker the TS walk strips before
/// resolving a dependency).
fn strip_nullable(ty: &str) -> &str {
    ty.strip_suffix("| null").map_or(ty, str::trim)
}

/// `Array<T>` -> `T` (the element type the dependency walk recurses into); a bare `T` passes
/// through.
fn unwrap_array(ty: &str) -> &str {
    ty.strip_prefix("Array<")
        .and_then(|s| s.strip_suffix('>'))
        .map_or(ty, str::trim)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn empty_struct_emits_blank_body() {
        let decls = DtoDecls::load();
        // `PingParams`/`EmptyParams` are `Record<string, never>` -> `{\n\n}`.
        assert_eq!(
            emit_interface(&decls, "PingParams"),
            "export interface PingParams {\n\n}"
        );
    }

    #[test]
    fn bigint_field_maps_to_number() {
        let (mapped, optional) = ts_type("bigint", "FrameSampleDto", "frameIndex");
        assert_eq!(mapped, "number");
        assert!(!optional);
    }

    #[test]
    fn selector_field_maps_to_union() {
        let (mapped, _) = ts_type("JsonValue", "ComponentParams", "entity");
        assert_eq!(mapped, "WireUuid | string | number");
    }

    #[test]
    fn opaque_json_field_maps_to_unknown() {
        let (mapped, _) = ts_type("JsonValue", "MaterialSetGraphParams", "graph");
        assert_eq!(mapped, "unknown");
    }

    #[test]
    fn nullable_field_is_optional() {
        let (mapped, optional) = ts_type("Vec3 | null", "SetTransformParams", "translation");
        assert_eq!(mapped, "Vec3");
        assert!(optional);
    }

    #[test]
    fn enum_field_inlines_union() {
        let (mapped, optional) = ts_type("AaModeDto", "SetAaParams", "mode");
        assert_eq!(
            mapped,
            "\"off\" | \"fxaa\" | \"taa\" | \"msaa2\" | \"msaa4\" | \"msaa8\""
        );
        assert!(!optional);
    }
}
