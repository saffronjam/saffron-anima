//! The crate-root error type and `Result` alias.

/// The foundation error type.
///
/// `saffron-core` has almost no fallible functions of its own; the value of a
/// typed root error is that downstream crates compose against it with `#[from]`
/// and propagate with `?`.
#[derive(Debug, thiserror::Error)]
pub enum Error {
    /// A failure whose underlying cause genuinely has no further structure.
    #[error("{0}")]
    Message(String),
}

/// The crate `Result` alias bound to the typed [`Error`].
///
/// Every Saffron library crate exports its own `Result<T>` alias over its own
/// `Error`; this is `saffron-core`'s.
pub type Result<T> = core::result::Result<T, Error>;
