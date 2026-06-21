//! The control-plane DTO crate: the single source of truth for the wire types, shared by
//! the engine, the `sa` CLI, and the protocol codegen (`serde`/`schemars`/`ts-rs` derives).
//!
//! The crate defines 236 structs + 17 enums + the wire-helpers, with field declaration order
//! preserved — that order is the positional-CLI-argument order and the OpenRPC `required`
//! order. There is no parser: the struct *is* the model and `derive` reads it at compile time,
//! so the serde and schema codegen carry no hand-written serialization code.
//!
//! Depends only on `saffron-core` so the `sa` CLI links the DTOs without the engine.

#![deny(unsafe_code)]

mod codegen;
mod command;
mod dto;
mod schema;
mod uuid;

pub use codegen::{struct_fragments, ts_decls};
pub use command::{
    COMMAND_FIXTURES, COMMAND_SKIPS, COMMANDS, CommandSpec, DTO_TYPE_NAMES, HELP_COMMAND,
    HELP_SKIP_REASON, fixture_for, skip_for,
};
pub use dto::*;
pub use schema::{
    COMPONENT_NAMES, SELECTOR_FIELDS, component_schemas, fragment_for, positional_field_order,
};
pub use uuid::Uuid;
