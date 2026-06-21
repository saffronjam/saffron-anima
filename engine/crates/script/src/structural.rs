//! The structural-component write gate: the fixed set of cache/asset-backed
//! components a mid-play script may not `set`/`add`.
//!
//! These components are cooked at play start — the Jolt world
//! (`Collider`/`Rigidbody`/`KinematicBones`), the rig caches
//! (`SkinnedMesh`/`Bone`/`FootIk`/`BonePhysics`), and the hierarchy link
//! (`Relationship`). A script `set`/`add` of one mid-play would desync the live state
//! (the registry's `deserialize` auto-adds, which is correct only for scene load), so
//! the write bindings refuse them with a logged `false`. `remove_component` is gated
//! separately by the registry's `removable` flag, not by this set.

/// The component names the script write bindings refuse. Keyed on the registered name
/// — verified against the scene registry's built-in names by the unit test below.
pub const STRUCTURAL_COMPONENTS: &[&str] = &[
    "Relationship",
    "SkinnedMesh",
    "Bone",
    "FootIk",
    "BonePhysics",
    "Collider",
    "Rigidbody",
    "KinematicBones",
];

/// Whether `name` is a structural component the script write bindings refuse.
#[must_use]
pub fn is_structural_component(name: &str) -> bool {
    STRUCTURAL_COMPONENTS.contains(&name)
}

#[cfg(test)]
mod tests {
    use super::*;
    use saffron_scene::register_builtin_components;

    #[test]
    fn the_gate_membership_is_exact() {
        assert!(is_structural_component("Collider"));
        assert!(is_structural_component("Rigidbody"));
        assert!(is_structural_component("Relationship"));
        assert!(!is_structural_component("Transform"));
        assert!(!is_structural_component("PointLight"));
        assert!(!is_structural_component("NotARealComponent"));
    }

    /// Every gated name must name a real registered component — a typo here would
    /// silently leave a structural component writable.
    #[test]
    fn every_gated_name_resolves_a_registry_row() {
        let registry = register_builtin_components();
        for &name in STRUCTURAL_COMPONENTS {
            assert!(
                registry.find_by_name(name).is_some(),
                "structural component '{name}' must name a registered component"
            );
        }
    }
}
