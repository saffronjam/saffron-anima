//! The owned control context: the command registry plus the (optional) socket
//! server, and the once-per-frame `poll` entry the host calls.

use schemars::JsonSchema;
use serde::Serialize;
use serde::de::DeserializeOwned;

use saffron_assets::AssetServer;
use saffron_physics::World;
use saffron_sceneedit::SceneEditContext;
use saffron_window::Window;

use crate::error::Result;
use crate::registry::{CommandRegistry, ControlRenderer, EngineContext, register_builtin_commands};
use crate::server::{ControlServer, control_socket_path, start_control_server};

/// Owns the command registry and the listening socket. The registry is built
/// once at startup (it has no per-frame mutation); the `EngineContext` is rebuilt
/// each frame in [`ControlContext::poll`] — the C++ `ControlContext`.
///
/// A bind failure is non-fatal: the context is constructed with `server: None`
/// and runs inactive, so the engine still runs without a control socket (the C++
/// "control socket disabled" warning path).
pub struct ControlContext {
    registry: CommandRegistry,
    server: Option<ControlServer>,
}

impl Default for ControlContext {
    fn default() -> Self {
        Self::new()
    }
}

impl ControlContext {
    /// Registers the builtin commands and binds the control socket. If the bind
    /// fails, the context is still returned (inactive) so the host keeps running.
    #[must_use]
    pub fn new() -> Self {
        let mut registry = CommandRegistry::new();
        register_builtin_commands(&mut registry);

        let server = match start_control_server(control_socket_path()) {
            Ok(server) => {
                saffron_core::log_info!("control socket listening on {}", server.path());
                Some(server)
            }
            Err(error) => {
                saffron_core::log_warn!("control socket disabled: {error}");
                None
            }
        };

        Self { registry, server }
    }

    /// Whether the socket bound successfully and the context is serving.
    #[must_use]
    pub fn is_active(&self) -> bool {
        self.server.is_some()
    }

    /// Closes the listening socket (dropping its [`ControlServer`]) so it stops serving.
    ///
    /// The host calls this during teardown to release the socket promptly (the C++
    /// `destroyControlContext`), before the renderer is dropped; the registry stays so a
    /// late palette/manifest read still resolves. Idempotent.
    pub fn shutdown(&mut self) {
        self.server = None;
    }

    /// The command registry (for the manifest / command-palette generators).
    #[must_use]
    pub fn registry(&self) -> &CommandRegistry {
        &self.registry
    }

    /// Registers a typed command after the builtins, for the one host-owned command
    /// (`get-script-schema`) the control crate must not carry itself (it needs the Lua
    /// schema reader, and only the host may depend on `saffron-script`). Mirrors
    /// [`CommandRegistry::register`]; the wire encoding is applied there.
    pub fn register<P, R>(
        &mut self,
        name: &'static str,
        help: &'static str,
        handler: impl Fn(&mut EngineContext<'_>, P) -> Result<R> + 'static,
    ) where
        P: DeserializeOwned + JsonSchema + 'static,
        R: Serialize,
    {
        self.registry.register(name, help, handler);
    }

    /// Brings the host's project up from the editor-set environment once at startup, before
    /// the first frame (the C++ host's `config.onCreate` project block): `SAFFRON_PROJECT`
    /// opens/creates a named project, else `SAFFRON_AUTO_EMPTY_PROJECT` makes a per-shell
    /// scratch project, else a working-directory `project.json` opens; otherwise nothing
    /// loads and the host waits for the editor's picker. Runs the same project-bring-up path
    /// the lifecycle commands use, against the live subsystem borrows.
    pub fn bootstrap_project_from_env(
        &mut self,
        window: &mut Window,
        renderer: &mut dyn ControlRenderer,
        scene_edit: &mut SceneEditContext,
        assets: &mut AssetServer,
    ) {
        let mut ctx = EngineContext {
            window,
            renderer,
            scene_edit,
            assets,
            physics: None,
        };
        crate::commands_asset::bootstrap_project_from_env(&mut ctx);
    }

    /// Drains and runs any pending control requests on the calling (main) thread.
    /// Call once per frame with the live subsystem borrows (the C++
    /// `pollControl`). A no-op when the socket failed to bind.
    ///
    /// `physics` is the live play world (non-owning) or `None` in Edit.
    pub fn poll(
        &mut self,
        window: &mut Window,
        renderer: &mut dyn ControlRenderer,
        scene_edit: &mut SceneEditContext,
        assets: &mut AssetServer,
        physics: Option<&mut World>,
    ) {
        let Some(server) = self.server.as_mut() else {
            return;
        };
        let mut ctx = EngineContext {
            window,
            renderer,
            scene_edit,
            assets,
            physics,
        };
        let registry = &self.registry;
        server.drain(|line| match saffron_json::parse_json(line) {
            Ok(request) => saffron_json::dump_json(&registry.dispatch(&mut ctx, &request), -1),
            Err(_) => {
                // A non-JSON line gets the frozen invalid-request envelope, with
                // no `id` to echo (the C++ `drainControlServer` invalid path).
                r#"{"ok":false,"error":"invalid JSON request"}"#.to_owned()
            }
        });
    }
}
