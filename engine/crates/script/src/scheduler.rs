//! The Roblox-task-style coroutine scheduler: verbatim Luau installed onto the `sa`
//! table after the bindings are bound.
//!
//! Ports the C++ `SchedulerPrelude` (`script_runtime.cpp:865`–904) unchanged. The
//! scheduler is pure Luau over the enabled coroutine library: `sa.spawn_task` creates
//! and resumes a coroutine, `sa.wait` yields it (a no-op outside a coroutine, never a
//! tick error), `sa.delay` is wait + call, and the global `_sa_advance(dt)` resumes
//! ready coroutines timed off accumulated `dt` — deterministic, never `os.clock` (the
//! sandbox omits `os` anyway). The runtime calls `_sa_advance(dt)` once per tick (after
//! the message dispatch) through [`crate::ScriptHost`]; a faulting coroutine logs via
//! `sa.log` and never crashes the VM.
//!
//! The prelude keeps the C++ `rawset(sa, …)` form (the C++ `sa` namespace had a read-only
//! `__newindex`; in Rust `sa` is a plain table, so `rawset` is a plain assignment). It
//! diverges from the C++ in one place — `sa.wait`'s "am I inside a scheduler task?" guard.
//! The C++ relied on `coroutine.running()`'s `ismain` being `true` on the main thread, but
//! mlua's Luau backend runs every `Function::call` (including a script's `on_update`) on an
//! auxiliary Lua thread, so `ismain` reads `false` even in a bare `on_update`. The prelude
//! instead tracks the scheduler coroutine it is currently resuming (`_sa_active`) and
//! yields only from *that* coroutine; a bare-`on_update` `sa.wait` falls through to the
//! documented ignored no-op, never the "yield across a C-call boundary" error.

use mlua::Lua;

use crate::error::{Error, Result};

/// The scheduler prelude, byte-identical to the C++ `SchedulerPrelude`.
const SCHEDULER_PRELUDE: &str = r#"
local _tasks, _accum, _sa_active = {}, 0, nil
rawset(sa, "spawn_task", function(fn, ...)
  local co = coroutine.create(fn)
  local prev = _sa_active
  _sa_active = co
  local ok, waitFor = coroutine.resume(co, ...)
  _sa_active = prev
  if not ok then sa.log("sa: task error: " .. tostring(waitFor))
  elseif coroutine.status(co) ~= "dead" then
    _tasks[#_tasks + 1] = { co = co, wake = _accum + (type(waitFor) == "number" and waitFor or 0) }
  end
  return co
end)
rawset(sa, "wait", function(seconds)
  local running = coroutine.running()
  if running ~= _sa_active or _sa_active == nil then
    sa.log("sa.wait called outside a coroutine is ignored")
    return
  end
  return coroutine.yield(seconds or 0)
end)
rawset(sa, "delay", function(seconds, fn)
  return sa.spawn_task(function() sa.wait(seconds) fn() end)
end)
function _sa_advance(dt)
  _accum = _accum + dt
  local ready, keep = {}, {}
  for _, t in ipairs(_tasks) do
    if t.wake <= _accum then ready[#ready + 1] = t else keep[#keep + 1] = t end
  end
  _tasks = keep
  for _, t in ipairs(ready) do
    local prev = _sa_active
    _sa_active = t.co
    local ok, waitFor = coroutine.resume(t.co)
    _sa_active = prev
    if not ok then sa.log("sa: coroutine error: " .. tostring(waitFor))
    elseif coroutine.status(t.co) ~= "dead" then
      _tasks[#_tasks + 1] = { co = t.co, wake = _accum + (type(waitFor) == "number" and waitFor or 0) }
    end
  end
end
"#;

/// Installs the scheduler prelude (`sa.spawn_task`/`sa.wait`/`sa.delay` + the global
/// `_sa_advance`) onto the already-bound `sa` table.
///
/// Run once per session after [`crate::register_no_scene_globals`] and the scene
/// bindings, so the prelude's `rawset(sa, …)` lands on the live `sa` table. A failure
/// (a missing `sa` global, a Luau error) is surfaced as [`Error::Runtime`] for the
/// caller to log — the C++ logged it and continued.
pub fn install(lua: &Lua) -> Result<()> {
    lua.load(SCHEDULER_PRELUDE)
        .set_name("sa:scheduler")
        .exec()
        .map_err(|e| Error::Runtime(e.to_string()))
}
