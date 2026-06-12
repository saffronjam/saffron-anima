/// Graphics/UI abbreviations that render fully uppercase rather than Sentence-cased.
const ABBREVIATIONS = new Set([
  "rgba",
  "rgb",
  "srgb",
  "uv",
  "fov",
  "orm",
  "ao",
  "hdr",
  "hdri",
  "ibl",
  "gi",
  "ggx",
  "ior",
  "lod",
  "pbr",
  "id",
  "ui",
  "uuid",
  "ssao",
  "ssgi",
  "ddgi",
  "gtao",
  "taa",
  "fxaa",
  "msaa",
  "brdf",
  "lut",
]);

/// Turn a component field key into a human label: split camelCase humps and snake/kebab separators,
/// lowercase the words, capitalize the first; known abbreviations render fully uppercase.
/// `albedoTexture` → "Albedo texture", `emissiveStrength` → "Emissive strength", `rgba` → "RGBA",
/// `ormTexture` → "ORM texture".
export function humanizeFieldName(key: string): string {
  const words = key
    .replace(/([a-z0-9])([A-Z])/g, "$1 $2")
    .replace(/[_-]+/g, " ")
    .trim()
    .split(/\s+/)
    .map((word) => word.toLowerCase());
  if (words.length === 0) {
    return key;
  }
  return words
    .map((word, i) => {
      if (ABBREVIATIONS.has(word)) {
        return word.toUpperCase();
      }
      return i === 0 ? word.charAt(0).toUpperCase() + word.slice(1) : word;
    })
    .join(" ");
}
