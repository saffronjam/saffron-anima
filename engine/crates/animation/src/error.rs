//! The crate-root error type and `Result` alias.

/// Errors raised by the animation runtime.
///
/// The only fallible surface in this crate is the clip loader (wired in a later
/// phase): the sampling and pose-algebra functions are infallible and return
/// poses, not [`Result`]. The enum and alias exist now so the public signatures
/// that *will* be fallible compile against a typed error from the start.
#[derive(Debug, thiserror::Error)]
pub enum Error {
    /// A clip asset could not be resolved by the injected loader. The payload is
    /// the loader's message.
    #[error("clip load failed: {0}")]
    ClipLoad(String),
}

/// The crate `Result` alias bound to the typed [`Error`].
pub type Result<T> = std::result::Result<T, Error>;
