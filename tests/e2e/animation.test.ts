// Skeletal clip import over the control plane: a rigged+animated glTF imports, each clip
// is baked to a `.sanim` sidecar and registered as an AssetType::Animation catalog entry,
// and saving the project persists those entries. The clip sampling/IO math is covered by
// the engine's animation + geometry self-tests; this file proves the import + catalog wire.

import { afterAll, beforeAll, expect, test } from "bun:test";
import { mkdtempSync, readdirSync, readFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { Engine, REPO } from "./harness.ts";

let engine: Engine;
let appdata: string;
const FIXTURE = join(REPO, "engine", "assets", "models", "animated-strip.gltf");

beforeAll(async () => {
  appdata = mkdtempSync(join(tmpdir(), "saffron-anim-e2e-"));
  engine = await Engine.boot({ SAFFRON_AUTO_EMPTY_PROJECT: "1", SAFFRON_APPDATA_DIR: appdata });
});
afterAll(async () => {
  await engine?.shutdown();
  rmSync(appdata, { recursive: true, force: true });
});

interface Asset {
  id: string;
  name: string;
  type: string;
  duration?: number;
}

/// Files of one extension anywhere under a directory.
function findByExt(dir: string, ext: string): string[] {
  return readdirSync(dir, { recursive: true })
    .map((entry) => entry.toString())
    .filter((entry) => entry.endsWith(ext));
}

test("importing a rigged+animated glTF registers an Animation clip asset", async () => {
  await engine.call("import-model", { path: FIXTURE });
  await engine.settle();

  const { assets } = await engine.call<{ assets: Asset[] }>("list-assets");
  const clips = assets.filter((a) => a.type === "animation");
  expect(clips.length).toBe(1);
  expect(clips[0].name).toBe("Bend");
});

test("the clip is baked to a .sanim sidecar under the project assets", () => {
  const sanims = findByExt(appdata, ".sanim");
  expect(sanims.length).toBe(1);
});

test("save-project persists the Animation entry to project.json", async () => {
  await engine.call("save-project");
  const projectFile = findByExt(appdata, "project.json")[0];
  expect(projectFile).toBeDefined();
  const doc = JSON.parse(readFileSync(join(appdata, projectFile), "utf8"));
  const clips = (doc.assets ?? []).filter((a: Asset) => a.type === "animation");
  expect(clips.length).toBe(1);
  expect(clips[0].name).toBe("Bend");
  // catalogToJson carries the clip length on animation rows.
  expect(clips[0].duration).toBeCloseTo(1.0, 3);
});

test("the engine logged no validation errors", async () => {
  await engine.settle(500);
  expect(engine.validationErrors()).toEqual([]);
});
