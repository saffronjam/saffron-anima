//! The scoped session guard: the scene and registry are lent to a scripted call only
//! while it is on the stack.
//!
//! A `&mut Scene` lifetime cannot live inside the `'static` userdata an
//! [`crate::entity::EntityHandle`] becomes, so the borrow is re-supplied per call by a
//! **scoped guard** instead of cached in the handle. An entity handle kept past its
//! session degrades to a logged no-op, never a dangling deref.
//!
//! Because the VM is single-threaded and `!Send`, the guard is a thread-local slot that
//! the scene is *moved into* for the duration of a scripted call and *moved back out of*
//! on scope exit (`mem::take` + restore). The borrow never escapes into the VM, so the
//! borrow checker is satisfied with no `unsafe`: the handle's accessors reach the live
//! scene only through [`with_scene`] / [`with_scene_mut`], which see the moved-in value,
//! and resolve to the documented no-op default whenever no session is open.

use std::cell::RefCell;
use std::rc::Rc;
use std::sync::Arc;

use mlua::RegistryKey;

use saffron_core::Uuid;
use saffron_scene::{ComponentRegistry, Scene, ScriptInputState};

use crate::bridge::ScriptHostBridge;

thread_local! {
    /// The scene lent for the active scripted call, or `None` between calls.
    ///
    /// `RefCell<Option<Scene>>` (single-thread shared-mutable): the VM is `!Send`, so no
    /// `Mutex` is needed. `Some` exactly while a [`ScopedSession`] is alive.
    static SESSION: RefCell<Option<Scene>> = const { RefCell::new(None) };

    /// The component registry lent alongside the scene, or `None` between calls.
    ///
    /// The registry drives the type-erased component bridge
    /// (`get/set/add/remove/has_component`). It is read-only during a session, so it
    /// crosses as a shared `Arc` clone rather than a moved value — there is nothing to
    /// move back out.
    static REGISTRY: RefCell<Option<Arc<ComponentRegistry>>> = const { RefCell::new(None) };

    /// The deferred structural ops accumulated during the active call's instance loop.
    ///
    /// `entity:destroy()` queues a uuid here and the handle stays valid for the rest
    /// of the handler; the runtime drains it with [`take_deferred`] after the loop and
    /// runs `destroy_entity` + one `relink_hierarchy` if dirty — never mid-loop. The slot
    /// is reset on each [`enter_session`] and lives for the call's duration.
    static DEFERRED: RefCell<DeferredOps> = const { RefCell::new(DeferredOps::new()) };

    /// The gameplay-input snapshot lent for the active call, or `None` when the call
    /// runs without input (a `None` input → every input binding returns its default).
    ///
    /// Read-only during a session (the edges are derived by the host *before* the
    /// tick), so the bindings only ever read it; it crosses by move like the scene
    /// (the host owns it between ticks) so there is no per-tick clone.
    static INPUT: RefCell<Option<ScriptInputState>> = const { RefCell::new(None) };

    /// The uuid of the instance whose handler is currently running, so a queued
    /// message records its sender. `Uuid(0)` (no sender) outside an instance handler —
    /// set per instance, cleared after the loop.
    static SENDER: RefCell<Uuid> = const { RefCell::new(Uuid(0)) };

    /// The inter-script messages queued during the active call's instance loop, drained
    /// by the runtime after the loop.
    ///
    /// `entity:send` queues a targeted message; `sa.broadcast` queues with `target =
    /// Uuid(0)` (every instance). The payload is a registry ref so it survives the
    /// queue and is released after dispatch — never carried mid-loop (the instance
    /// vector is iterated by reference).
    static MESSAGES: RefCell<Vec<ScriptMessage>> = const { RefCell::new(Vec::new()) };

    /// The host-callback bridge lent for the active call, or `None` between calls.
    ///
    /// The physics-reaching bindings (`sa.raycast`/`apply_impulse`/ragdoll/`sa.log`'s
    /// sink) reach it through [`with_bridge`]; the runtime clones the host's
    /// `Rc<dyn ScriptHostBridge>` into this slot when it opens a session and clears it on
    /// scope exit. `None` (no bridge lent) means the documented no-op path. Read-only
    /// during a session, so it crosses as a shared `Rc` clone.
    static BRIDGE: RefCell<Option<Rc<dyn ScriptHostBridge>>> = const { RefCell::new(None) };
}

/// One queued inter-script message, dispatched after the instance loop.
///
/// `entity:send(handler, payload)` queues `target = <that entity's uuid>`;
/// `sa.broadcast(handler, payload)` queues `target = Uuid(0)` (every instance). The
/// payload rides as an [`mlua::RegistryKey`], released after the message is dispatched.
pub struct ScriptMessage {
    /// The uuid of the target instance, or `Uuid(0)` for a broadcast to every instance.
    pub target: Uuid,
    /// The uuid of the sending instance (`Uuid(0)` when sent outside a handler).
    pub sender: Uuid,
    /// The handler method name invoked as `self:<handler>(sender, payload)`.
    pub handler: String,
    /// The registry ref to the payload table, or `None` when the payload was `nil`.
    pub payload: Option<RegistryKey>,
}

/// The structural ops a scripted call defers to its post-loop flush.
///
/// Only `destroy` is queued; `set_parent`/`spawn` run inline (they touch components,
/// not the instance vector). Reset per [`enter_session`].
#[derive(Default)]
pub struct DeferredOps {
    /// The uuids queued by `entity:destroy()`, drained after the instance loop.
    pub pending_destroy: Vec<Uuid>,
    /// Set whenever a queued structural op changes the hierarchy, so the flush runs
    /// `relink_hierarchy` exactly once.
    pub hierarchy_dirty: bool,
}

impl DeferredOps {
    /// An empty op set — the between-calls state.
    const fn new() -> Self {
        Self {
            pending_destroy: Vec::new(),
            hierarchy_dirty: false,
        }
    }

    /// Whether anything is queued (a fast skip for the flush).
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.pending_destroy.is_empty() && !self.hierarchy_dirty
    }
}

/// Queues `uuid` for deferred destruction and marks the hierarchy dirty. A no-op when
/// no session is open — the deferred slot is only meaningful within a call.
pub fn defer_destroy(uuid: Uuid) {
    if !session_active() {
        return;
    }
    DEFERRED.with(|slot| {
        let mut ops = slot.borrow_mut();
        ops.pending_destroy.push(uuid);
        ops.hierarchy_dirty = true;
    });
}

/// Takes and clears the deferred ops accumulated during the call, for the runtime's
/// post-loop flush. Returns the empty set when no session is open.
pub fn take_deferred() -> DeferredOps {
    DEFERRED.with(|slot| std::mem::take(&mut *slot.borrow_mut()))
}

/// Runs `f` against the lent input snapshot, or returns `None` when no input is lent
/// (the input bindings' no-input path → the documented default).
pub fn with_input<R>(f: impl FnOnce(&ScriptInputState) -> R) -> Option<R> {
    INPUT.with(|slot| slot.borrow().as_ref().map(f))
}

/// Sets the uuid of the instance whose handler is about to run, so a message queued
/// from it records the right sender. Cleared to `Uuid(0)` after the loop.
pub fn set_sender(uuid: Uuid) {
    SENDER.with(|slot| *slot.borrow_mut() = uuid);
}

/// The uuid of the instance whose handler is currently running (`Uuid(0)` outside a
/// handler), for the message sender and the host log-sink tag.
#[must_use]
pub fn current_sender() -> Uuid {
    SENDER.with(|slot| *slot.borrow())
}

/// Queues an inter-script message, drained by the runtime after the instance loop.
/// A no-op when no session is open — a message outside a call has no loop to dispatch
/// into.
pub fn queue_message(message: ScriptMessage) {
    if !session_active() {
        return;
    }
    MESSAGES.with(|slot| slot.borrow_mut().push(message));
}

/// Takes and clears the queued messages, for the runtime's post-loop dispatch. Returns
/// the empty queue when no session is open.
pub fn take_messages() -> Vec<ScriptMessage> {
    MESSAGES.with(|slot| std::mem::take(&mut *slot.borrow_mut()))
}

/// Whether a scripted call is currently on the stack (a session is open).
///
/// The first check every accessor runs.
#[must_use]
pub fn session_active() -> bool {
    SESSION.with(|slot| slot.borrow().is_some())
}

/// Lends `bridge` to the active call so the physics-reaching bindings reach the host's
/// callbacks. The runtime sets it right after [`enter_session`] (cloning the host's
/// `Rc<dyn ScriptHostBridge>`); [`ScopedSession`]'s drop clears it. A no-op outside a
/// session — a bridge call has no live world to reach.
pub fn set_bridge(bridge: Rc<dyn ScriptHostBridge>) {
    if !session_active() {
        return;
    }
    BRIDGE.with(|slot| *slot.borrow_mut() = Some(bridge));
}

/// Runs `f` against the lent host-callback bridge, or returns `None` when no bridge is
/// lent (no session open, or the host installed none) — the documented no-op path for the
/// physics-reaching bindings.
pub fn with_bridge<R>(f: impl FnOnce(&dyn ScriptHostBridge) -> R) -> Option<R> {
    BRIDGE.with(|slot| slot.borrow().as_ref().map(|b| f(b.as_ref())))
}

/// Opens a session, lending `scene` and `registry` to the thread-local slots, and
/// returns a guard.
///
/// While the returned [`ScopedSession`] is alive, [`session_active`] is `true`,
/// [`with_scene`] / [`with_scene_mut`] see `scene`'s contents, and [`with_registry`]
/// sees `registry`; dropping the guard moves the scene back into `*scene` and clears
/// the registry. The guard borrows `scene` for its whole lifetime, so the caller
/// cannot touch `scene` until the session ends — the compiler enforces the "scene is
/// lent, not aliased" contract via `mem::take`/restore around the instance loop.
///
/// `input` is the gameplay-input snapshot lent for the call (the host's snapshot,
/// moved in and restored on drop like the scene); pass `None` for a call that runs
/// without input (`on_create`/`on_destroy`, the schema probe, the tests), so the
/// input bindings read their documented default.
///
/// A re-entrant call (a session already open on this thread) is a programming error —
/// the invariant is single-call-on-the-stack — so it panics rather than silently
/// aliasing.
pub fn enter_session<'a>(
    scene: &'a mut Scene,
    registry: Arc<ComponentRegistry>,
    mut input: Option<&'a mut ScriptInputState>,
) -> ScopedSession<'a> {
    SESSION.with(|slot| {
        assert!(
            slot.borrow().is_none(),
            "script: a session is already active on this thread (re-entrant tick)"
        );
        let moved = std::mem::take(scene);
        *slot.borrow_mut() = Some(moved);
    });
    REGISTRY.with(|slot| {
        *slot.borrow_mut() = Some(registry);
    });
    // Move the caller's input into the slot (no clone), restored on drop like the scene.
    INPUT.with(|slot| {
        *slot.borrow_mut() = input.as_deref_mut().map(std::mem::take);
    });
    DEFERRED.with(|slot| {
        *slot.borrow_mut() = DeferredOps::new();
    });
    MESSAGES.with(|slot| slot.borrow_mut().clear());
    SENDER.with(|slot| *slot.borrow_mut() = Uuid(0));
    BRIDGE.with(|slot| {
        slot.borrow_mut().take();
    });
    ScopedSession { scene, input }
}

/// The live session: a guard that holds the caller's `&mut Scene` (and optional `&mut
/// ScriptInputState`) and restores them on drop. The lent contents live in the
/// thread-local slots while this is alive.
#[must_use = "the session ends when the guard is dropped; hold it for the call's duration"]
pub struct ScopedSession<'a> {
    scene: &'a mut Scene,
    input: Option<&'a mut ScriptInputState>,
}

impl Drop for ScopedSession<'_> {
    fn drop(&mut self) {
        SESSION.with(|slot| {
            if let Some(scene) = slot.borrow_mut().take() {
                *self.scene = scene;
            }
        });
        INPUT.with(|slot| {
            if let (Some(restored), Some(target)) = (slot.borrow_mut().take(), self.input.as_mut())
            {
                **target = restored;
            }
        });
        REGISTRY.with(|slot| {
            slot.borrow_mut().take();
        });
        DEFERRED.with(|slot| {
            *slot.borrow_mut() = DeferredOps::new();
        });
        MESSAGES.with(|slot| slot.borrow_mut().clear());
        SENDER.with(|slot| *slot.borrow_mut() = Uuid(0));
        BRIDGE.with(|slot| {
            slot.borrow_mut().take();
        });
    }
}

/// Runs `f` with a shared reference to the lent scene, or returns `None` when no
/// session is open (the "outside a script callback" path).
///
/// The handle accessors call this for reads (`get_position`, `name`, `uuid`, …):
/// `None` means session-inactive → the caller logs and returns the documented
/// default.
pub fn with_scene<R>(f: impl FnOnce(&Scene) -> R) -> Option<R> {
    SESSION.with(|slot| slot.borrow().as_ref().map(f))
}

/// Runs `f` with a mutable reference to the lent scene, or returns `None` when no
/// session is open. The write counterpart of [`with_scene`] (`set_position`, …).
pub fn with_scene_mut<R>(f: impl FnOnce(&mut Scene) -> R) -> Option<R> {
    SESSION.with(|slot| slot.borrow_mut().as_mut().map(f))
}

/// Runs `f` with the lent component registry, or returns `None` when no session is
/// open. The registry drives the type-erased component bridge (`get/set/add/remove/
/// has_component`).
pub fn with_registry<R>(f: impl FnOnce(&ComponentRegistry) -> R) -> Option<R> {
    REGISTRY.with(|slot| slot.borrow().as_ref().map(|r| f(r)))
}

#[cfg(test)]
mod tests {
    use super::*;
    use saffron_scene::register_builtin_components;

    fn registry() -> Arc<ComponentRegistry> {
        Arc::new(register_builtin_components())
    }

    #[test]
    fn no_session_open_by_default() {
        assert!(!session_active());
        assert!(with_scene(|_| ()).is_none());
        assert!(with_scene_mut(|_| ()).is_none());
        assert!(with_registry(|_| ()).is_none());
    }

    #[test]
    fn session_lends_scene_and_restores_on_drop() {
        let mut scene = Scene::new();
        let e = scene.create_entity("probe");
        {
            let _guard = enter_session(&mut scene, registry(), None);
            assert!(session_active());
            let found = with_scene(|s| s.valid(e)).expect("session open");
            assert!(found, "the lent scene carries the entity");
            let has_transform_row =
                with_registry(|r| r.find_by_name("Transform").is_some()).expect("registry lent");
            assert!(has_transform_row, "the lent registry resolves a row");
        }
        assert!(!session_active(), "the session closed on drop");
        assert!(
            with_registry(|_| ()).is_none(),
            "the registry was cleared on drop"
        );
        assert!(scene.valid(e), "the scene came back intact");
    }

    #[test]
    fn mutations_through_the_session_survive() {
        let mut scene = Scene::new();
        let e = scene.create_entity("probe");
        {
            let _guard = enter_session(&mut scene, registry(), None);
            with_scene_mut(|s| {
                s.add_component(
                    e,
                    saffron_scene::Name {
                        name: "renamed".to_owned(),
                    },
                )
                .expect("rename");
            })
            .expect("session open");
        }
        let name = scene
            .with_component::<saffron_scene::Name, _>(e, |n| n.name.clone())
            .expect("name present");
        assert_eq!(name, "renamed");
    }

    #[test]
    #[should_panic(expected = "already active")]
    fn re_entrant_session_panics() {
        let mut scene = Scene::new();
        let _outer = enter_session(&mut scene, registry(), None);
        let mut other = Scene::new();
        let _inner = enter_session(&mut other, registry(), None);
    }
}
