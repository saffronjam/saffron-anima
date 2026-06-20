//! The control-plane error type and `Result` alias.

/// Failures raised while standing up the socket server, framing requests, or
/// running a command handler.
///
/// The C++ control layer threaded `Result<T> = std::expected<T, std::string>`
/// through every command; the port keeps the same shape but types the variants
/// so callers can `match` on the failure kind. A command handler's *business*
/// failure (a bad selector, an unknown view) is carried as
/// [`Error::Command`] — that message is what crosses the wire as the envelope's
/// `error` string, byte-for-byte the C++ behavior.
#[derive(Debug, thiserror::Error)]
pub enum Error {
    /// A socket syscall (socket/bind/listen/accept) failed while standing up the
    /// server. The payload names the syscall and the OS error.
    #[error("{0}")]
    Socket(String),
    /// The resolved socket path does not fit in `sockaddr_un.sun_path`.
    #[error("socket path too long: {0}")]
    PathTooLong(String),
    /// A command handler reported a business failure; this message is the
    /// envelope `error` string on the wire.
    #[error("{0}")]
    Command(String),
    /// A request param failed to deserialize into the handler's typed DTO.
    #[error("{0}")]
    Params(String),
}

impl Error {
    /// Builds a [`Error::Command`] from anything that renders as a string — the
    /// idiomatic replacement for the C++ `Err(std::format(...))` at a handler's
    /// failure points.
    pub fn command(message: impl Into<String>) -> Self {
        Self::Command(message.into())
    }
}

/// The crate `Result` alias bound to the typed [`Error`].
pub type Result<T> = std::result::Result<T, Error>;
