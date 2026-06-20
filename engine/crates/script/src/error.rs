//! The crate-root error type and `Result` alias.

/// The scripting crate error type.
///
/// Replaces the C++ `Result<void>` + `Err(traceback)` stringly model: a load
/// failure (a syntax error in the chunk) is [`Error::Load`], a runtime failure
/// (a raised Lua error, a faulting handler) is [`Error::Runtime`], and a budget
/// trip (the per-call instruction limit or the VM memory limit) is
/// [`Error::Budget`]. The `Load`/`Runtime` payload carries the Luau message with
/// its stack traceback already appended — `mlua` surfaces a traceback on every
/// Lua error, so the C++ `tracebackHandler`/`luaL_traceback` raw-stack dance is
/// gone.
#[derive(Debug, thiserror::Error)]
pub enum Error {
    /// A chunk failed to load (a Luau syntax error). Carries the message.
    #[error("script load error: {0}")]
    Load(String),
    /// A chunk failed at run time (a raised error or a faulting builtin),
    /// carrying the Luau message and its stack traceback.
    #[error("script runtime error: {0}")]
    Runtime(String),
    /// A scripted call exceeded its instruction or memory budget and was
    /// aborted before it could hang the host frame.
    #[error("script budget exceeded: {0}")]
    Budget(String),
}

/// The crate `Result` alias bound to the typed [`Error`].
pub type Result<T> = std::result::Result<T, Error>;
