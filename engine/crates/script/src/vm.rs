//! The Luau VM: an owned `mlua::Lua` with the sandbox and the per-call
//! instruction/memory budget installed.
//!
//! `mlua::Lua` owns the state and frees it on `Drop`. The VM is `!Send` and
//! single-thread-owned by value, per the host's ownership model.

use std::cell::Cell;
use std::rc::Rc;

use mlua::{Lua, LuaOptions, StdLib, VmState};

use crate::error::{Error, Result};

/// The sandboxed standard-library set: base, coroutine, string, math, table, utf8.
/// `os`/`io`/`debug`/`package` are never loaded, so they read as `nil` in script.
/// `new_with` refuses any unsafe library, so this set stays safe with
/// `#![deny(unsafe_code)]`.
fn sandbox_libs() -> StdLib {
    StdLib::COROUTINE | StdLib::STRING | StdLib::MATH | StdLib::TABLE | StdLib::UTF8
}

/// How many interrupt callbacks a single scripted call may take before it is
/// aborted. Luau fires the interrupt periodically (not once per instruction), so
/// this bounds a runaway loop to a finite number of host cycles rather than an
/// exact instruction count — enough to abort a tight `while true do end` without
/// hanging the frame, generous enough that no real gameplay tick trips it.
pub const DEFAULT_INSTRUCTION_BUDGET: u64 = 1_000_000;

/// The VM memory ceiling in bytes. A chunk that allocates past this is aborted
/// with a memory error (mapped to [`Error::Budget`]).
pub const DEFAULT_MEMORY_LIMIT: usize = 256 * 1024 * 1024;

/// The shared budget state read and written by the interrupt callback.
///
/// `Rc<Cell<…>>` because the interrupt closure is `'static` and runs on the same
/// thread as the VM — no `Send` is needed, so this is the cheap single-thread
/// shared-mutable idiom, not `Arc<Mutex>`.
#[derive(Clone, Default)]
struct Budget {
    /// Interrupt callbacks taken since the last [`Budget::reset`].
    ticks: Rc<Cell<u64>>,
    /// The per-call ceiling on `ticks`; 0 disables the instruction guard.
    limit: Rc<Cell<u64>>,
    /// Set by the interrupt when `ticks` crosses `limit`, so a caller can tell a
    /// budget abort from an ordinary runtime error after a run returns.
    tripped: Rc<Cell<bool>>,
}

impl Budget {
    /// Clears the tick counter and the trip flag at the start of a scripted call.
    fn reset(&self) {
        self.ticks.set(0);
        self.tripped.set(false);
    }
}

/// One Luau VM under the engine sandbox with the instruction/memory budget armed.
///
/// Owns the `mlua::Lua`; dropping the `ScriptVm` frees the VM. Not `Send`.
pub struct ScriptVm {
    lua: Lua,
    budget: Budget,
}

impl ScriptVm {
    /// Creates a sandboxed VM with the default budget.
    ///
    /// Only the [`sandbox_libs`] set is loaded, so `io`/`os`/`debug`/`package`
    /// read as `nil`; Luau's `sandbox(true)` then freezes the base tables and
    /// makes the VM deterministic. The instruction interrupt and the memory limit
    /// are installed up front; each scripted run resets the per-call tick counter
    /// via [`ScriptVm::run_string`].
    pub fn new() -> Result<Self> {
        Self::with_limits(DEFAULT_INSTRUCTION_BUDGET, DEFAULT_MEMORY_LIMIT)
    }

    /// Creates a sandboxed VM with explicit instruction and memory limits.
    ///
    /// An `instruction_budget` of 0 disables the instruction guard; tests use a
    /// small budget to trip a runaway loop quickly.
    pub fn with_limits(instruction_budget: u64, memory_limit: usize) -> Result<Self> {
        let budget = Budget::default();
        let lua = Lua::new_with(sandbox_libs(), LuaOptions::default())
            .map_err(|e| map_lua_error(&e, &budget))?;

        budget.limit.set(instruction_budget);

        let ticks = Rc::clone(&budget.ticks);
        let limit = Rc::clone(&budget.limit);
        let tripped = Rc::clone(&budget.tripped);
        lua.set_interrupt(move |_| {
            let cap = limit.get();
            if cap == 0 {
                return Ok(VmState::Continue);
            }
            let next = ticks.get().saturating_add(1);
            ticks.set(next);
            if next > cap {
                tripped.set(true);
                return Err(mlua::Error::runtime("script instruction budget exceeded"));
            }
            Ok(VmState::Continue)
        });

        lua.set_memory_limit(memory_limit)
            .map_err(|e| map_lua_error(&e, &budget))?;

        // Sandbox last: it freezes the globals after the interrupt/limit wiring,
        // which touch VM internals, not the script-visible global table.
        lua.sandbox(true).map_err(|e| map_lua_error(&e, &budget))?;

        Ok(Self { lua, budget })
    }

    /// Borrows the underlying VM (for binding registration).
    pub fn lua(&self) -> &Lua {
        &self.lua
    }

    /// Registers the value types (the `sa.Vec3` userdata) into this VM.
    pub fn register_value_types(&self) -> Result<()> {
        crate::bindings::register_value_types(&self.lua)
    }

    /// Registers the value types and the no-scene `sa.*` globals (`sa.vec3`,
    /// `sa.lerp`, `sa.look_at`, `sa.log`) into this VM.
    ///
    /// The runtime VM and the throwaway schema VM both call this, so a `properties`
    /// default of `sa.vec3(0, 1, 0)` resolves at edit time too.
    pub fn register_no_scene_globals(&self) -> Result<()> {
        crate::bindings::register_no_scene_globals(&self.lua)
    }

    /// Registers the scene-dependent `sa.*` free functions (input reads, hierarchy
    /// queries, `sa.broadcast`) onto the already-installed `sa` table.
    ///
    /// The runtime VM calls this after [`ScriptVm::register_no_scene_globals`]; the
    /// throwaway schema VM does not (the schema probe has no scene to read).
    pub fn register_scene_globals(&self) -> Result<()> {
        crate::bindings::register_scene_globals(&self.lua)
    }

    /// The per-call instruction budget; 0 means the instruction guard is off.
    pub fn instruction_budget(&self) -> u64 {
        self.budget.limit.get()
    }

    /// Clears the per-call instruction/trip budget at the start of a scripted call
    /// that the runtime drives directly through `mlua` (an instance method call),
    /// rather than through [`ScriptVm::run_string`]. The runtime resets before every
    /// `on_create`/`on_update`/`on_destroy` so a tight loop in one handler cannot
    /// starve the next.
    pub(crate) fn reset_budget(&self) {
        self.budget.reset();
    }

    /// Maps a run-time `mlua::Error` from a directly-driven call to the crate error,
    /// consulting the budget trip flag — the same classification
    /// [`ScriptVm::run_string`] applies, exposed for the runtime's instance-method
    /// calls.
    pub(crate) fn classify_run_error(&self, err: &mlua::Error) -> Error {
        map_lua_error(err, &self.budget)
    }

    /// Loads and runs a source chunk under `chunk_name`, resetting the per-call
    /// budget first.
    ///
    /// A syntax error maps to [`Error::Load`]; a raised/faulting runtime error to
    /// [`Error::Runtime`] (with the Luau traceback in the message); a budget or
    /// memory-limit abort to [`Error::Budget`].
    pub fn run_string(&self, source: &str, chunk_name: &str) -> Result<()> {
        self.budget.reset();
        let chunk = self.lua.load(source).set_name(chunk_name);
        let function = chunk.into_function().map_err(|e| classify_load(&e))?;
        function
            .call::<()>(())
            .map_err(|e| map_lua_error(&e, &self.budget))
    }
}

/// Maps a load-time `mlua::Error` to the crate error.
///
/// A syntax error is the load failure; anything else this early (e.g. the VM
/// rejecting the chunk) is treated as a load error too, since no script code ran.
fn classify_load(err: &mlua::Error) -> Error {
    Error::Load(err.to_string())
}

/// Maps a run-time `mlua::Error` to the crate error, consulting the budget trip
/// flag so an instruction-budget abort surfaces as [`Error::Budget`] rather than
/// the generic runtime error its interrupt `Err` would otherwise become.
fn map_lua_error(err: &mlua::Error, budget: &Budget) -> Error {
    if budget.tripped.get() {
        return Error::Budget("instruction budget exceeded".to_owned());
    }
    match err {
        mlua::Error::SyntaxError { message, .. } => Error::Load(message.clone()),
        mlua::Error::MemoryError(message) => Error::Budget(message.clone()),
        other => Error::Runtime(other.to_string()),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn good_chunk_runs_ok() {
        let vm = ScriptVm::new().expect("create vm");
        vm.run_string("assert(1 + 1 == 2)", "selftest")
            .expect("good chunk should run");
    }

    #[test]
    fn broken_chunk_returns_runtime_error_with_traceback() {
        let vm = ScriptVm::new().expect("create vm");
        let err = vm
            .run_string("error('deliberate')", "selftest-broken")
            .expect_err("broken chunk should fail");
        let Error::Runtime(message) = &err else {
            panic!("expected a runtime error, got {err:?}");
        };
        assert!(
            message.contains("deliberate"),
            "runtime error should carry the raised message: {message}"
        );
        assert!(
            message.contains("stack traceback") || message.contains("stack backtrace"),
            "runtime error should carry a traceback: {message}"
        );
    }

    #[test]
    fn nested_call_error_carries_traceback() {
        let vm = ScriptVm::new().expect("create vm");
        let source =
            "local function inner() error('boom') end\nlocal function outer() inner() end\nouter()";
        let err = vm
            .run_string(source, "selftest-nested")
            .expect_err("nested error should fail");
        let Error::Runtime(message) = &err else {
            panic!("expected a runtime error, got {err:?}");
        };
        assert!(message.contains("boom"), "message: {message}");
        assert!(
            message.contains("traceback") || message.contains("backtrace"),
            "message should include a traceback: {message}"
        );
    }

    #[test]
    fn sandbox_omits_unsafe_libraries() {
        let vm = ScriptVm::new().expect("create vm");
        vm.run_string(
            "assert(io == nil and os == nil and debug == nil and package == nil)",
            "selftest-sandbox",
        )
        .expect("sandbox probe should pass");
    }

    #[test]
    fn sandbox_keeps_allowed_libraries() {
        let vm = ScriptVm::new().expect("create vm");
        vm.run_string(
            "assert(type(string) == 'table' and type(math) == 'table' and type(table) == 'table')",
            "selftest-allowed",
        )
        .expect("allowed libraries should be present");
        vm.run_string("assert(math.floor(2.7) == 2)", "selftest-math")
            .expect("math should work");
    }

    #[test]
    fn syntax_error_maps_to_load_variant() {
        let vm = ScriptVm::new().expect("create vm");
        let err = vm
            .run_string("this is not lua ===", "selftest-syntax")
            .expect_err("syntax error should fail");
        assert!(
            matches!(err, Error::Load(_)),
            "expected a load error, got {err:?}"
        );
    }

    #[test]
    fn runaway_loop_trips_instruction_budget() {
        let vm = ScriptVm::with_limits(10_000, DEFAULT_MEMORY_LIMIT).expect("create vm");
        let err = vm
            .run_string("while true do end", "selftest-runaway")
            .expect_err("runaway loop should be aborted");
        assert!(
            matches!(err, Error::Budget(_)),
            "expected a budget error, got {err:?}"
        );
    }

    #[test]
    fn budget_resets_between_runs() {
        let vm = ScriptVm::with_limits(100_000, DEFAULT_MEMORY_LIMIT).expect("create vm");
        // A bounded loop under budget runs fine, repeatedly, because each run
        // resets the per-call tick counter.
        for _ in 0..3 {
            vm.run_string(
                "local s = 0 for i = 1, 1000 do s = s + i end assert(s == 500500)",
                "selftest-bounded",
            )
            .expect("bounded loop should run within budget");
        }
    }

    #[test]
    fn memory_allocation_past_limit_errors() {
        // A tight 8 MiB ceiling: a chunk growing a table without bound must fail
        // before exhausting host memory. The interrupt budget is disabled so the
        // failure is the memory limit, not the instruction guard.
        let vm = ScriptVm::with_limits(0, 8 * 1024 * 1024).expect("create vm");
        let err = vm
            .run_string(
                "local t = {} local i = 1 while true do t[i] = string.rep('x', 4096) i = i + 1 end",
                "selftest-oom",
            )
            .expect_err("unbounded allocation should be aborted");
        assert!(
            matches!(err, Error::Budget(_)),
            "expected a budget error from the memory limit, got {err:?}"
        );
    }
}
