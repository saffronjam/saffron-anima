// Codegen: bundle the repo's control schemas into one JSON Schema (a $defs map
// keyed by each schema's title) and compile it to a single TypeScript module.
// Re-running is deterministic: the schema file list is sorted before bundling.
//
// Run with: bun run scripts/gen-protocol.ts
import { readdir, readFile, mkdir, writeFile } from "node:fs/promises";
import { join, dirname, basename } from "node:path";
import { fileURLToPath } from "node:url";
import { compile, type JSONSchema } from "json-schema-to-typescript";

const scriptDir = dirname(fileURLToPath(import.meta.url));
const editorDir = dirname(scriptDir);
const repoRoot = dirname(editorDir);
const schemaDir = join(repoRoot, "schemas", "control");
const outFile = join(editorDir, "src", "protocol", "index.ts");

type AnySchema = Record<string, unknown>;

/// Map a schema file name (e.g. "entity-ref.schema.json") to its title by reading it.
async function loadSchemas(): Promise<Map<string, AnySchema>> {
  const files = (await readdir(schemaDir))
    .filter((f) => f.endsWith(".schema.json"))
    .sort();
  const byFile = new Map<string, AnySchema>();
  for (const file of files) {
    const text = await readFile(join(schemaDir, file), "utf8");
    byFile.set(file, JSON.parse(text) as AnySchema);
  }
  return byFile;
}

/// Recursively rewrite every `{ $ref: "X.schema.json" }` to `{ $ref: "#/$defs/<Title>" }`
/// using the file→title lookup, and strip per-file `$schema`/`$id`.
function rewrite(node: unknown, titleByFile: Map<string, string>): unknown {
  if (Array.isArray(node)) {
    return node.map((item) => rewrite(item, titleByFile));
  }
  if (node && typeof node === "object") {
    const obj = node as AnySchema;
    const out: AnySchema = {};
    for (const [key, value] of Object.entries(obj)) {
      if (key === "$schema" || key === "$id") {
        continue;
      }
      if (key === "$ref" && typeof value === "string" && value.endsWith(".schema.json")) {
        const title = titleByFile.get(value);
        if (!title) {
          throw new Error(`unresolved $ref to "${value}"`);
        }
        out[key] = `#/$defs/${title}`;
        continue;
      }
      out[key] = rewrite(value, titleByFile);
    }
    return out;
  }
  return node;
}

const bannerComment = `/**
 * GENERATED — do not edit.
 *
 * Produced by editor/scripts/gen-protocol.ts from schemas/control/*.schema.json.
 * Regenerate with \`bun run gen:protocol\`. Edit the schemas, not this file.
 */`;

const commandResultMap = `
/**
 * Maps each typed control command to the schema title of its result payload.
 * Hand-maintained alongside the engine's control surface (gen-protocol appends it).
 */
export interface CommandResultMap {
  "add-entity": EntityRef;
  "copy-entity": EntityRef;
  "create-entity": EntityRef;
  "get-selection": Selection;
  inspect: InspectResult;
  "list-entities": EntityList;
  "list-assets": AssetList;
  "get-gizmo": GizmoState;
  "set-gizmo": GizmoState;
  "get-camera": EditorCamera;
  "set-camera": EditorCamera;
  "render-stats": RenderStats;
  "get-environment": Environment;
  "set-environment": Environment;
  "get-thumbnail": Thumbnail;
  "view-asset": Thumbnail;
  "get-project": ProjectInfo;
  "new-project": ProjectInfo;
  "open-project": ProjectInfo;
  "save-project": ProjectInfo;
  "load-project": ProjectInfo;
}
`;

async function main(): Promise<void> {
  const byFile = await loadSchemas();

  const titleByFile = new Map<string, string>();
  for (const [file, schema] of byFile) {
    const title = schema.title;
    if (typeof title !== "string") {
      throw new Error(`schema "${file}" is missing a string "title"`);
    }
    titleByFile.set(file, title);
    // Some $refs spell the bare basename; tolerate both forms.
    titleByFile.set(basename(file), title);
  }

  const $defs: Record<string, AnySchema> = {};
  for (const [file, schema] of byFile) {
    const title = titleByFile.get(file)!;
    $defs[title] = rewrite(schema, titleByFile) as AnySchema;
  }

  // Root references every def so each one is emitted (declareExternallyReferenced
  // emits referenced defs; unreachableDefinitions covers any not referenced here).
  const bundle: JSONSchema = {
    $schema: "https://json-schema.org/draft/2020-12/schema",
    title: "Protocol",
    type: "object",
    additionalProperties: false,
    properties: Object.fromEntries(
      Object.keys($defs)
        .sort()
        .map((title) => [title, { $ref: `#/$defs/${title}` }]),
    ),
    $defs,
  };

  const generated = await compile(bundle, "Protocol", {
    bannerComment,
    additionalProperties: false,
    declareExternallyReferenced: true,
    unreachableDefinitions: true,
  });

  await mkdir(dirname(outFile), { recursive: true });
  await writeFile(outFile, generated + commandResultMap, "utf8");
  // eslint-disable-next-line no-console
  console.log(`wrote ${outFile} (${$defs ? Object.keys($defs).length : 0} types)`);
}

main().catch((err) => {
  // eslint-disable-next-line no-console
  console.error(err);
  process.exit(1);
});
