/// Canonical component order + hidden set, shared by the Inspector sections and the hierarchy's
/// component subrows so the tree leaves and the Inspector stay in lockstep (mirrors the C++
/// registry order / the `Components` schema key order). Ordering only — never a per-component
/// render switch. A regenerated schema with new components extends COMPONENT_ORDER.

export const COMPONENT_ORDER = [
  "Name",
  "Transform",
  "Mesh",
  "Camera",
  "Material",
  "MaterialSet",
  "Script",
  "DirectionalLight",
  "PointLight",
  "SpotLight",
  "ReflectionProbe",
] as const;

/// Components the Inspector/tree never render: Relationship carries the hierarchy's durable parent
/// uuid (edited through the tree / `set-parent`, never as a raw field); Bone is an empty joint tag
/// (bone-ness shows in the outliner, not as a section).
export const HIDDEN_COMPONENTS = new Set<string>(["Relationship", "Bone"]);

/// Present components in known order, then any extras in insertion order, minus the hidden set.
export function orderedComponentNames(components: Record<string, unknown>): string[] {
  const present = Object.keys(components).filter((c) => !HIDDEN_COMPONENTS.has(c));
  const known = COMPONENT_ORDER.filter((c) => present.includes(c));
  const extra = present.filter((c) => !COMPONENT_ORDER.includes(c as never));
  return [...known, ...extra];
}
