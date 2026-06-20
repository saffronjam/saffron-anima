//! Edit-time script schema: the declared `properties` of a script, read in a
//! throwaway sandboxed VM with no gameplay run.
//!
//! Ports the C++ `readScriptSchema`/`inferField`/`scriptFieldTypeName`
//! (`script_runtime.cpp` 1512–1617). A fresh sandboxed VM with the value types
//! registered (so a `sa.vec3(...)` default resolves) loads + runs the chunk to get
//! the class table, reads its `properties` table, infers each field's edit-time type
//! from its declared default, and returns the fields sorted by name. The chunk only
//! builds tables — no `on_create`/`on_update` runs, so the declaration must be
//! side-effect-free. This feeds the editor Inspector via the host's
//! `get-script-schema` command and the `GetScriptSchemaResult` DTO.

use std::path::Path;

use mlua::{Table, Value as LuaValue};

use saffron_core::log_info;

use crate::error::{Error, Result};
use crate::value::SaVec3;
use crate::vm::ScriptVm;

/// The inferred edit-time type of a script-declared property.
///
/// Ports the C++ `ScriptFieldType` (`script.cppm:178`): a number, a boolean, a
/// string, or a vec3 (a `sa.Vec3` default, captured as a 3-number JSON array — the
/// shape the Inspector + override storage round-trip).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ScriptFieldType {
    /// A Luau number default.
    Number,
    /// A Luau boolean default.
    Bool,
    /// A Luau string default.
    String,
    /// An `sa.Vec3` default — serialized as a 3-number JSON array.
    Vec3,
}

impl ScriptFieldType {
    /// The wire/Inspector name of the type (`"number"|"bool"|"string"|"vec3"`).
    ///
    /// Ports `scriptFieldTypeName` (`script_runtime.cpp:1512`).
    #[must_use]
    pub fn wire_name(self) -> &'static str {
        match self {
            ScriptFieldType::Number => "number",
            ScriptFieldType::Bool => "bool",
            ScriptFieldType::String => "string",
            ScriptFieldType::Vec3 => "vec3",
        }
    }
}

/// One script-declared editable field: the `properties` key, the type inferred from
/// its default, and the default itself.
///
/// Ports the C++ `ScriptField` (`script.cppm:190`). The `default_value` rides as an
/// opaque [`serde_json::Value`] — a scalar (number/bool/string) or, for a vec3, a
/// 3-number array — matching the C++ `nlohmann::json defaultValue` and the
/// `ScriptSlot.overrides` shape `inject_fields` consumes.
#[derive(Clone, Debug, PartialEq)]
pub struct ScriptField {
    /// The declared `properties` key.
    pub name: String,
    /// The edit-time type inferred from the default.
    pub field_type: ScriptFieldType,
    /// The declared default value (a scalar, or a 3-number array for a vec3).
    pub default_value: serde_json::Value,
}

/// Infers a [`ScriptField`] from a declared `properties` default.
///
/// Ports `inferField` (`script_runtime.cpp:1533`): a number/bool/string maps 1:1; an
/// `sa.Vec3` userdata is a vec3 captured as a 3-number JSON array; anything else (a
/// table, a function, `nil`) is not a field. Returns `None` for an uninferable
/// default — the caller logs and skips it.
fn infer_field(name: String, default: &LuaValue) -> Option<ScriptField> {
    match default {
        LuaValue::Integer(i) => Some(ScriptField {
            name,
            field_type: ScriptFieldType::Number,
            default_value: serde_json::json!(*i as f64),
        }),
        LuaValue::Number(n) => Some(ScriptField {
            name,
            field_type: ScriptFieldType::Number,
            default_value: serde_json::json!(*n),
        }),
        LuaValue::Boolean(b) => Some(ScriptField {
            name,
            field_type: ScriptFieldType::Bool,
            default_value: serde_json::json!(*b),
        }),
        LuaValue::String(s) => s.to_str().ok().map(|text| ScriptField {
            name,
            field_type: ScriptFieldType::String,
            default_value: serde_json::json!(text.to_owned()),
        }),
        LuaValue::UserData(ud) => ud.borrow::<SaVec3>().ok().map(|v| ScriptField {
            name,
            field_type: ScriptFieldType::Vec3,
            default_value: serde_json::json!([v.0.x, v.0.y, v.0.z]),
        }),
        _ => None,
    }
}

/// Reads a script's declared `properties` at edit time in a throwaway sandboxed VM.
///
/// No gameplay runs: a fresh sandboxed VM with the value types + no-scene `sa.*`
/// globals registered (so a `sa.vec3(...)` default resolves) loads and runs the chunk
/// to get the class table, then reads only its `properties` — the declaration must
/// build tables, not run gameplay. Each entry's type is inferred from its default;
/// an entry whose default cannot be inferred is skipped with a logged note. Fields
/// come back sorted by name. Returns [`Error::Load`]/[`Error::Runtime`] on a
/// load/run failure, and [`Error::Load`] if the chunk does not return a table.
///
/// Ports `readScriptSchema` (`script_runtime.cpp:1565`).
pub fn read_script_schema(path: &Path) -> Result<Vec<ScriptField>> {
    let vm = ScriptVm::new()?;
    vm.register_no_scene_globals()?;

    let source = std::fs::read_to_string(path)
        .map_err(|e| Error::Load(format!("{}: {e}", path.display())))?;

    vm.reset_budget();
    let lua = vm.lua();
    let chunk_name = path.display().to_string();
    let function = lua
        .load(&source)
        .set_name(chunk_name)
        .into_function()
        .map_err(|e| Error::Load(e.to_string()))?;
    let returned: LuaValue = function.call(()).map_err(|e| vm.classify_run_error(&e))?;

    let LuaValue::Table(class) = returned else {
        return Err(Error::Load(format!(
            "'{}' must return a class table",
            path.display()
        )));
    };

    let properties: LuaValue = class
        .get("properties")
        .map_err(|e| vm.classify_run_error(&e))?;
    let LuaValue::Table(properties) = properties else {
        return Ok(Vec::new());
    };

    let mut fields = read_property_fields(&properties, &path.display().to_string())?;
    fields.sort_by(|a, b| a.name.cmp(&b.name));
    Ok(fields)
}

/// Walks a `properties` table, inferring a [`ScriptField`] per string-keyed entry and
/// logging + skipping any whose default cannot be inferred. Non-string keys are
/// ignored (the C++ `lua_type(L, -2) == LUA_TSTRING` guard).
fn read_property_fields(properties: &Table, source: &str) -> Result<Vec<ScriptField>> {
    let mut fields = Vec::new();
    for pair in properties.clone().pairs::<LuaValue, LuaValue>() {
        let (key, default) = pair.map_err(|e| Error::Runtime(e.to_string()))?;
        let LuaValue::String(key) = key else {
            continue;
        };
        let Ok(name) = key.to_str() else {
            continue;
        };
        let name = name.to_owned();
        match infer_field(name.clone(), &default) {
            Some(field) => fields.push(field),
            None => log_info!("script schema '{source}': skipping '{name}' (uninferable default)"),
        }
    }
    Ok(fields)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write;

    /// Writes `source` to a uniquely-named `.luau` file under the OS temp dir and
    /// returns its path; the file is left for the OS to reap (the schema reader only
    /// reads it).
    fn write_temp_script(source: &str) -> std::path::PathBuf {
        use std::sync::atomic::{AtomicU64, Ordering};
        static COUNTER: AtomicU64 = AtomicU64::new(0);
        let n = COUNTER.fetch_add(1, Ordering::Relaxed);
        let mut dir = std::env::temp_dir();
        dir.push(format!(
            "saffron-script-schema-{}-{}.luau",
            std::process::id(),
            n
        ));
        let mut file = std::fs::File::create(&dir).expect("create temp script");
        file.write_all(source.as_bytes())
            .expect("write temp script");
        dir
    }

    #[test]
    fn infers_each_field_type_and_sorts_by_name() {
        let path = write_temp_script(
            r#"
            local M = {}
            M.properties = {
                speed = 5,
                name = "x",
                on = true,
                offset = sa.vec3(0, 1, 0),
            }
            function M.on_update(self, dt) end
            return M
            "#,
        );
        let fields = read_script_schema(&path).expect("schema read");

        assert_eq!(fields.len(), 4, "every inferable field is present");
        // Sorted by name: name, offset, on, speed.
        assert_eq!(fields[0].name, "name");
        assert_eq!(fields[0].field_type, ScriptFieldType::String);
        assert_eq!(fields[0].default_value, serde_json::json!("x"));

        assert_eq!(fields[1].name, "offset");
        assert_eq!(fields[1].field_type, ScriptFieldType::Vec3);
        assert_eq!(fields[1].default_value, serde_json::json!([0.0, 1.0, 0.0]));

        assert_eq!(fields[2].name, "on");
        assert_eq!(fields[2].field_type, ScriptFieldType::Bool);
        assert_eq!(fields[2].default_value, serde_json::json!(true));

        assert_eq!(fields[3].name, "speed");
        assert_eq!(fields[3].field_type, ScriptFieldType::Number);
        assert_eq!(fields[3].default_value, serde_json::json!(5.0));
    }

    #[test]
    fn skips_uninferable_defaults() {
        let path = write_temp_script(
            r#"
            local M = {}
            M.properties = {
                speed = 2,
                handler = function() end,
                bag = { 1, 2, 3 },
            }
            return M
            "#,
        );
        let fields = read_script_schema(&path).expect("schema read");
        // A function and a table default are not fields; only `speed` survives.
        assert_eq!(fields.len(), 1);
        assert_eq!(fields[0].name, "speed");
        assert_eq!(fields[0].field_type, ScriptFieldType::Number);
    }

    #[test]
    fn no_properties_table_yields_empty_fields() {
        let path = write_temp_script(
            r#"
            local M = {}
            function M.on_update(self, dt) end
            return M
            "#,
        );
        let fields = read_script_schema(&path).expect("schema read");
        assert!(fields.is_empty(), "no properties means no fields");
    }

    #[test]
    fn load_failure_returns_typed_err() {
        let path = write_temp_script("this is not lua ===");
        let err = read_script_schema(&path).expect_err("a syntax error should fail");
        assert!(
            matches!(err, Error::Load(_)),
            "expected a load error, got {err:?}"
        );
    }

    #[test]
    fn run_failure_returns_typed_err() {
        let path = write_temp_script("error('declaration blew up')\nreturn {}");
        let err = read_script_schema(&path).expect_err("a raised error should fail");
        assert!(
            matches!(err, Error::Runtime(_)),
            "expected a runtime error, got {err:?}"
        );
    }

    #[test]
    fn non_table_return_is_a_load_error() {
        let path = write_temp_script("return 42");
        let err = read_script_schema(&path).expect_err("a non-table return should fail");
        let Error::Load(message) = &err else {
            panic!("expected a load error, got {err:?}");
        };
        assert!(
            message.contains("must return a class table"),
            "message should explain the contract: {message}"
        );
    }

    #[test]
    fn schema_vm_never_runs_gameplay_handlers() {
        // The schema VM must read `properties` without running `on_create`/`on_update`.
        // A handler that errors would surface as a run failure if it were called; the
        // module body itself only builds tables, so the read must succeed and the
        // declared field must be present.
        let path = write_temp_script(
            r#"
            local M = {}
            M.properties = { speed = 9 }
            function M.on_create(self) error("on_create must not run at schema time") end
            function M.on_update(self, dt) error("on_update must not run at schema time") end
            return M
            "#,
        );
        let fields = read_script_schema(&path).expect("schema read with gameplay handlers present");
        assert_eq!(fields.len(), 1);
        assert_eq!(fields[0].name, "speed");
    }

    #[test]
    fn fields_map_to_the_inspector_dto_shape() {
        // Each ScriptField maps to the (name, type-string, default-Value) the host's
        // get-script-schema command emits; a vec3 default is a 3-number JSON array —
        // the shape the Inspector + ScriptSlot.overrides round-trip.
        let path = write_temp_script(
            r#"
            local M = {}
            M.properties = { offset = sa.vec3(1, 2, 3), count = 7 }
            return M
            "#,
        );
        let fields = read_script_schema(&path).expect("schema read");

        let offset = fields.iter().find(|f| f.name == "offset").unwrap();
        assert_eq!(offset.field_type.wire_name(), "vec3");
        assert_eq!(offset.default_value, serde_json::json!([1.0, 2.0, 3.0]));
        assert!(
            offset
                .default_value
                .as_array()
                .is_some_and(|a| a.len() == 3),
            "a vec3 default is a 3-number array"
        );

        let count = fields.iter().find(|f| f.name == "count").unwrap();
        assert_eq!(count.field_type.wire_name(), "number");
        assert_eq!(count.default_value, serde_json::json!(7.0));
    }

    #[test]
    fn wire_names_cover_every_variant() {
        assert_eq!(ScriptFieldType::Number.wire_name(), "number");
        assert_eq!(ScriptFieldType::Bool.wire_name(), "bool");
        assert_eq!(ScriptFieldType::String.wire_name(), "string");
        assert_eq!(ScriptFieldType::Vec3.wire_name(), "vec3");
    }
}
