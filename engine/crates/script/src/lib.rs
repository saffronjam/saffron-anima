//! The mlua/Luau VM, the typed `sa.*` bindings, and the generated Luau type defs.
//!
//! Depends on `saffron-core`, `saffron-scene` only — exactly the C++
//! `Saffron.Script` module boundary. `mlua` confines every `lua_State`
//! unsafety internally, so this crate is `#![deny(unsafe_code)]`: the C++
//! raw-stack discipline is deleted, not ported.
//!
//! Phase 1 stands up the VM primitive: a sandboxed [`ScriptVm`] with an
//! instruction/memory budget and a typed [`Error`] carrying the Luau traceback.
//! Phase 2 adds the [`SaVec3`] value type and the declarative [`BINDINGS`] table —
//! the single source that both registers the no-scene `sa.*` surface and feeds the
//! Luau type emitter. Phase 3 adds the scoped [session guard](session) — the Rust
//! re-encoding of the C++ `currentScene` raw-pointer invariant — and the
//! [`EntityHandle`] scene-only surface (transforms, name/uuid, valid) built on it.
//! Phase 5 lands the [`ScriptHost`] lifecycle (`start_scripts`/`tick_scripts`/
//! `stop_scripts`, the class cache, the instance build with field injection +
//! overrides, pause-on-error, and the deferred destroy + relink). Phase 6 adds the
//! coroutine [scheduler](scheduler) prelude, the inter-script [`ScriptMessage`] queue
//! (`entity:send`/`sa.broadcast`, drained after the loop), the input reads (held +
//! derived edges + mouse, lent through the session guard), and the hierarchy/query
//! bindings (`parent`/`children`/`set_parent`/`spawn`/`get_entity_by_name`/
//! `find_all_by_name`/`find_by_uuid`/`primary_camera`). Phase 7 adds the
//! [`ScriptHostBridge`] (the POD seam the host implements for the physics reach — `sa.raycast`/
//! `apply_impulse`/ragdoll control + the `sa.log` sink), the pure-Scene `move_character`, and
//! [`ScriptHost::dispatch_contact`] (the contact-event ring → `on_trigger_enter`/`on_trigger_exit`/
//! `on_contact` handlers). Phase 8 adds [`read_script_schema`] (the Inspector field contract): a
//! throwaway sandboxed VM reads a script's declared `properties` and infers each
//! [`ScriptField`]'s [`ScriptFieldType`] from its default — the shape the host's
//! `get-script-schema` command maps to the `GetScriptSchemaResult` DTO.

#![deny(unsafe_code)]

mod bindings;
mod bridge;
mod convert;
mod entity;
mod error;
mod runtime;
mod scheduler;
mod schema;
mod session;
mod structural;
mod value;
mod vm;

pub use bindings::{
    Arg, BINDINGS, Binding, BindingKind, register_no_scene_globals, register_scene_globals,
    register_value_types,
};
pub use bridge::{NoopBridge, ScriptHostBridge, ScriptRagdollState, ScriptRayHit};
pub use entity::EntityHandle;
pub use error::{Error, Result};
pub use runtime::{ContactInfo, ScriptHost, ScriptRunError};
pub use schema::{ScriptField, ScriptFieldType, read_script_schema};
pub use session::{
    DeferredOps, ScopedSession, ScriptMessage, current_sender, defer_destroy, enter_session,
    queue_message, session_active, set_bridge, set_sender, take_deferred, take_messages,
    with_bridge, with_input, with_registry, with_scene, with_scene_mut,
};
pub use structural::{STRUCTURAL_COMPONENTS, is_structural_component};
pub use value::{SaVec3, lerp, look_at, vec3};
pub use vm::{DEFAULT_INSTRUCTION_BUDGET, DEFAULT_MEMORY_LIMIT, ScriptVm};
