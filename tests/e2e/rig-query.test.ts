// The rig query commands: get-rig reads a model's bone tree + clips from its .smodel container, and
// list-clips honors an asset selector. Both resolve a mesh sub-asset or a clip sub-asset to the same
// owning container, so the clip<->mesh association is intrinsic (same file). An unskinned model has no
// rig and errors with a stable message.

import { afterAll, beforeAll, expect, test } from "bun:test";
import { join } from "node:path";
import { Engine, REPO } from "./harness.ts";

let engine: Engine;
const LEG = join(REPO, "tests", "e2e", "fixtures", "leg.gltf");
const UNSKINNED = join(REPO, "tests", "e2e", "fixtures", "two-materials.gltf");

interface RigBone {
  index: number;
  name: string;
  parent: number;
  joint: boolean;
}
interface Rig {
  mesh: string;
  name: string;
  bones: RigBone[];
  clips: { id: string; name: string; duration: number }[];
}
interface ModelInfo {
  id: string;
  subAssets: { id: string; name: string; type: string }[];
}

let legModel = "";
let meshSub = "";
let clipSub = "";
let clipName = "";

beforeAll(async () => {
  engine = await Engine.boot({ SAFFRON_AUTO_EMPTY_PROJECT: "1" });
  const ref = await engine.call<{ id: string }>("import-model", { path: LEG });
  legModel = ref.id;
  await engine.settle();
  const info = await engine.call<ModelInfo>("model-info", { asset: legModel });
  meshSub = info.subAssets.find((s) => s.type === "mesh")!.id;
  const clip = info.subAssets.find((s) => s.type === "animation")!;
  clipSub = clip.id;
  clipName = clip.name;
});
afterAll(async () => {
  await engine?.shutdown();
});

test("get-rig returns the skeleton (joints + parent indices) and the container's clips", async () => {
  const rig = await engine.call<Rig>("get-rig", { asset: legModel });
  expect(rig.mesh).toBe(legModel);
  // leg.gltf: LegMesh + Hip/Knee/Ankle joints — the rig is the 3 joints, not the mesh node.
  expect(rig.bones.length).toBe(3);
  const byName = new Map(rig.bones.map((b) => [b.name, b]));
  expect(byName.get("Hip")?.joint).toBe(true);
  expect(byName.get("Hip")?.parent).toBe(-1);
  expect(byName.get("Knee")?.parent).toBe(byName.get("Hip")?.index);
  expect(byName.get("Ankle")?.parent).toBe(byName.get("Knee")?.index);
  expect(rig.clips.length).toBe(1);
  expect(rig.clips[0].id).toBe(clipSub);
  expect(rig.clips[0].name).toBe(clipName);
});

test("get-rig on a mesh sub-asset resolves to the same rig", async () => {
  const rig = await engine.call<Rig>("get-rig", { asset: meshSub });
  expect(rig.mesh).toBe(legModel);
  expect(rig.bones.length).toBe(3);
});

test("get-rig on a clip sub-asset resolves to the same rig (clip<->mesh link is intrinsic)", async () => {
  const rig = await engine.call<Rig>("get-rig", { asset: clipSub });
  expect(rig.mesh).toBe(legModel);
  expect(rig.clips.some((c) => c.id === clipSub)).toBe(true);
});

test("list-clips honors the asset selector", async () => {
  const filtered = await engine.call<{ clips: { id: string }[] }>("list-clips", { asset: legModel });
  expect(filtered.clips.length).toBe(1);
  expect(filtered.clips[0].id).toBe(clipSub);
});

test("list-assets carries rigged on a rigged model's rows and duration on clips", async () => {
  const list = await engine.call<{
    assets: { id: string; type: string; rigged?: boolean; duration?: number }[];
  }>("list-assets");
  const mesh = list.assets.find((a) => a.id === meshSub);
  expect(mesh?.rigged).toBe(true);
  const clip = list.assets.find((a) => a.id === clipSub);
  expect(clip?.duration).toBeGreaterThan(0);
});

test("get-rig on an unskinned model errors with a stable message", async () => {
  const ref = await engine.call<{ id: string }>("import-model", { path: UNSKINNED });
  await engine.settle();
  await expect(engine.call("get-rig", { asset: ref.id })).rejects.toThrow(/no rig/);
});

test("the engine logged no validation errors", async () => {
  await engine.settle();
  expect(engine.validationErrors()).toEqual([]);
});
