/// Turn a component field key into a human label: split camelCase humps and
/// snake/kebab separators, lowercase the words, capitalize the first.
/// `albedoTexture` → "Albedo texture", `emissiveStrength` → "Emissive strength".
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
  words[0] = words[0].charAt(0).toUpperCase() + words[0].slice(1);
  return words.join(" ");
}
