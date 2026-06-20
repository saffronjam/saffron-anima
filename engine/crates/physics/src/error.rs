//! The crate's typed error and `Result` alias.

/// Errors the physics layer can raise.
///
/// The fallible C++ surface (`createPhysicsWorld`, and the later `addCharacter`/`enableRagdoll`)
/// returned `Result<T>`/`Err(std::string)`; here the bare-message cases become typed variants so
/// callers can `match`. The no-op-on-null-world pattern (every mutator early-returned when
/// `impl == nullptr`) is *not* an error here — those methods take `&mut World`, so "no world" is a
/// type-level impossibility and never reaches this enum.
#[derive(Debug, thiserror::Error)]
pub enum Error {
    /// `JoltWorld` allocation failed (the C++ `make_unique` analogue handed back null).
    #[error("failed to create the Jolt physics world")]
    WorldCreate,

    /// The Jolt global init (`jolt_init`) reported failure.
    #[error("failed to initialize the Jolt globals: {0}")]
    GlobalInit(&'static str),

    /// `addCharacter` could not build the capsule shape for the character controller
    /// (`physics.cpp:948`).
    #[error("failed to create the character capsule")]
    CharacterCapsule,

    /// `enableRagdoll` was called on an entity lacking a `SkinnedMesh` + `BonePhysics` pair
    /// (`physics.cpp:1225`).
    #[error("rig has no SkinnedMesh + BonePhysics")]
    RagdollMissingComponents,

    /// The `BonePhysics` array length did not match the rig's bone count, so the parts cannot map
    /// 1:1 to the skeleton (`physics.cpp:1232`).
    #[error("BonePhysics array length {got} does not match the skeleton's {expected} bones")]
    RagdollMismatch {
        /// The skeleton's bone count.
        expected: usize,
        /// The `BonePhysics` array length supplied.
        got: usize,
    },

    /// `CreateRagdoll` returned null (the Jolt ragdoll could not be built from the settings,
    /// `physics.cpp:1300`).
    #[error("CreateRagdoll failed")]
    RagdollCreate,

    /// A blend mutator (`set_ragdoll_blend`) named a rig that has no live ragdoll this play session
    /// (`physics.cpp:1470`).
    #[error("rig has no live ragdoll")]
    NoRagdoll,

    /// `set_ragdoll_blend` was given a bone index outside the rig's bone range
    /// (`physics.cpp:1480`).
    #[error("bone index {0} out of range")]
    BoneOutOfRange(i32),

    /// A `Mesh`-shaped collider was placed on a Dynamic body. Jolt's `MeshShape` is
    /// Static/Kinematic only, so the populate walk skips the body and logs this (use a ConvexHull
    /// for a dynamic body, or make it Static/Kinematic — `physics.cpp:417`).
    #[error(
        "Mesh collider on a Dynamic body is invalid — Jolt MeshShape is Static/Kinematic only; \
         use ConvexHull for a dynamic body, or make it Static/Kinematic"
    )]
    MeshShapeOnDynamic,

    /// A ConvexHull/Mesh collider needs the `.smesh` of its `source_mesh`, but the populate walk
    /// was given no mesh-cook source. The C++ logged and skipped (`physics.cpp:392`, `:425`).
    #[error("collider shape has no mesh cook source")]
    NoCookSource,

    /// The mesh-cook closure failed to read/cook the `source_mesh` for a ConvexHull/Mesh collider
    /// (`physics.cpp:399`, `:432`). The payload is the cook's own message.
    #[error("mesh cook failed: {0}")]
    CookFailed(String),
}

/// The crate `Result` alias bound to [`Error`].
pub type Result<T> = std::result::Result<T, Error>;
