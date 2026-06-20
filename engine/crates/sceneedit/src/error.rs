//! The crate-root error type and `Result` alias.

/// The scene-edit crate error type.
///
/// The session surface returns `Result` where a transition can be refused (a play
/// transition from the wrong state) or a scene operation can fail. Scene-crate failures
/// compose in with `#[from]` so `?` lifts them without manual restringing.
#[derive(Debug, thiserror::Error)]
pub enum Error {
    /// A play-mode transition was rejected because the session was in the wrong state
    /// (e.g. pause while already in Edit).
    #[error("play transition rejected: {0}")]
    PlayTransition(String),
    /// A `saffron-scene` failure lifted into the scene-edit error via `?`.
    #[error(transparent)]
    Scene(#[from] saffron_scene::Error),
}

/// The crate `Result` alias bound to the typed [`Error`].
pub type Result<T> = std::result::Result<T, Error>;
