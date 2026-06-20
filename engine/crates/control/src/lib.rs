//! The control plane: the synchronous `AF_UNIX` command server, the fn-pointer
//! command registry, the `EngineContext` borrow seam, and the wire dispatch from
//! request DTOs (`saffron-protocol`) to engine subsystems.
//!
//! The model is the C++ `Saffron.Control` one, kept verbatim: a non-blocking,
//! single-threaded socket drained once per frame from the host's main loop — no
//! tokio, no async runtime, no worker thread. A request is one newline-delimited
//! compact-JSON line; the reply echoes the request `id` and carries `ok` plus
//! exactly one of `result` / `error`. The two builtin commands `ping` and `help`
//! land here; the five domain phases register their handlers onto the registry.
//!
//! Depends on `saffron-core`, `saffron-json`, `saffron-window`,
//! `saffron-rendering`, `saffron-scene`, `saffron-sceneedit`, `saffron-assets`,
//! `saffron-physics`, `saffron-protocol`, and `rustix` for the socket syscalls.

#![deny(unsafe_code)]

mod commands_animation;
mod commands_asset;
mod commands_physics;
mod commands_render;
mod commands_scene;
mod context;
mod error;
mod registry;
mod selector;
mod server;
#[cfg(test)]
mod test_support;

pub use context::ControlContext;
pub use error::{Error, Result};
pub use registry::{
    Command, CommandRegistry, ControlRenderer, EngineContext, positional_or,
    register_builtin_commands,
};
pub use selector::{entity_ref_dto, entity_uuid, resolve_entity};
pub use server::{ControlServer, control_socket_path, start_control_server};
