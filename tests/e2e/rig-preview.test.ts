// The rig preview scene: enter-rig-preview spawns a model's rig into an isolated scene routed through
// activeScene (the play-mode pattern), so animation commands drive it unchanged, and exit-rig-preview
// restores the authored scene + camera. The keystone invariant: a full enter -> scrub -> exit round-trip
// leaves project.json byte-identical. Mutual exclusion with play and authored-scene mutations is enforced.

import { afterAll, beforeAll, expect, test } from "bun:test";
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { Engine, REPO } from "./harness.ts";

let engine: Engine;
const LEG = join(REPO, "tests", "e2e", "fixtures", "leg.gltf");
const UNSKINNED = join(REPO, "tests", "e2e", "fixtures", "two-materials.gltf");

interface RigBoneEntity {
  index: number;
  entity: string;
}
interface EnterResult {
  rigEntity: string;
  bones: RigBoneEntity[];
}
interface PlayStateResult {
  state: string;
  previewAsset: string;
}
interface AnimState {
  time: number;
  playing: boolean;
  clip: string;
}

let legModel = "";
let projectPath = "";

beforeAll(async () => {
  engine = await Engine.boot({ SAFFRON_AUTO_EMPTY_PROJECT: "1" });
  const ref = await engine.call<{ id: string }>("import-model", { path: LEG });
  legModel = ref.id;
  await engine.settle();
  const proj = await engine.call<{ path: string }>("get-project");
  projectPath = proj.path;
});
afterAll(async () => {
  await engine?.shutdown();
});

async function listEntityIds(): Promise<string[]> {
  const list = await engine.call<{ entities: { id: string }[] }>("list-entities");
  return list.entities.map((e) => e.id).sort();
}

test("enter-rig-preview spawns the rig and reports the bone table", async () => {
  await engine.call("exit-rig-preview");
  const res = await engine.call<EnterResult>("enter-rig-preview", { asset: legModel });
  expect(res.rigEntity).not.toBe("0");
  expect(res.bones.length).toBe(3); // Hip/Knee/Ankle joints map to spawned entities
  for (const b of res.bones) {
    expect(b.entity).not.toBe("0");
  }
  const ps = await engine.call<PlayStateResult>("get-play-state");
  expect(ps.state).toBe("edit"); // preview stays in Edit
  expect(ps.previewAsset).toBe(legModel);
  await engine.call("exit-rig-preview");
});

test("seek advances the preview rig's animation state", async () => {
  await engine.call("exit-rig-preview");
  const res = await engine.call<EnterResult>("enter-rig-preview", { asset: legModel });
  const rig = res.rigEntity;
  const s0 = await engine.call<AnimState>("seek-animation", { entity: rig, time: 0.0 });
  await engine.call("seek-animation", { entity: rig, time: 0.4 });
  const state = await engine.call<AnimState>("get-animation-state", { entity: rig });
  expect(state.time).toBeGreaterThan(s0.time);
  expect(state.time).toBeCloseTo(0.4, 2);
  await engine.call("exit-rig-preview");
});

test("play during preview is rejected; enter during play is rejected", async () => {
  await engine.call("exit-rig-preview");
  await engine.call("enter-rig-preview", { asset: legModel });
  await expect(engine.call("play")).rejects.toThrow(/rig preview/);
  await engine.call("exit-rig-preview");

  await engine.call("play");
  await expect(engine.call("enter-rig-preview", { asset: legModel })).rejects.toThrow(/stop play/);
  await engine.call("stop");
});

test("project + asset mutations are rejected while previewing", async () => {
  await engine.call("exit-rig-preview");
  await engine.call("enter-rig-preview", { asset: legModel });
  await expect(engine.call("import-model", { path: LEG })).rejects.toThrow(/rig preview/);
  await expect(engine.call("reload-project")).rejects.toThrow(/rig preview/);
  await engine.call("exit-rig-preview");
});

test("entering an unskinned model errors with a stable message", async () => {
  await engine.call("exit-rig-preview");
  const ref = await engine.call<{ id: string }>("import-model", { path: UNSKINNED });
  await engine.settle();
  await expect(engine.call("enter-rig-preview", { asset: ref.id })).rejects.toThrow(/no rig/);
});

test("a preview round-trip leaves project.json byte-identical", async () => {
  await engine.call("exit-rig-preview");
  await engine.call("save-project");
  const before = readFileSync(projectPath, "utf8");
  const entitiesBefore = await listEntityIds();

  await engine.call("enter-rig-preview", { asset: legModel });
  // Re-enter the same rig is a swap (drop + respawn); exit must still land cleanly.
  const res = await engine.call<EnterResult>("enter-rig-preview", { asset: legModel });
  await engine.call("seek-animation", { entity: res.rigEntity, time: 0.3 });
  await engine.call("exit-rig-preview");

  await engine.call("save-project");
  const after = readFileSync(projectPath, "utf8");
  expect(after).toBe(before); // includes the editorCamera block: the engine-side camera restore holds
  expect(await listEntityIds()).toEqual(entitiesBefore);

  const ps = await engine.call<PlayStateResult>("get-play-state");
  expect(ps.state).toBe("edit");
  expect(ps.previewAsset).toBe("0");
});

test("scrubbing the preview rig moves its bones (the pose follows the playhead)", async () => {
  await engine.call("exit-rig-preview");
  const entered = await engine.call<EnterResult>("enter-rig-preview", { asset: legModel });
  // The first seek arms previewInEdit, so the evaluator poses the rig at the playhead.
  await engine.call("seek-animation", { entity: entered.rigEntity, time: 0 });
  await engine.settle(150);
  type WorldXform = { translation: { x: number; y: number; z: number } };
  const before = await Promise.all(
    entered.bones.map((b) => engine.call<WorldXform>("get-world-transform", { entity: b.entity })),
  );
  await engine.call("seek-animation", { entity: entered.rigEntity, time: 0.6 });
  await engine.settle(150);
  const after = await Promise.all(
    entered.bones.map((b) => engine.call<WorldXform>("get-world-transform", { entity: b.entity })),
  );
  const moved = before.some((b, i) => {
    const a = after[i].translation;
    return (
      Math.abs(a.x - b.translation.x) > 1e-4 ||
      Math.abs(a.y - b.translation.y) > 1e-4 ||
      Math.abs(a.z - b.translation.z) > 1e-4
    );
  });
  expect(moved).toBe(true); // KneeBend deforms the chain, so a joint's world position tracks the seek
  // A burst of seeks stays clean and the final state matches the last seek.
  for (const t of [0.1, 0.3, 0.5, 0.2, 0.45]) {
    await engine.call("seek-animation", { entity: entered.rigEntity, time: t });
  }
  const final = await engine.call<{ time: number }>("get-animation-state", { entity: entered.rigEntity });
  expect(final.time).toBeCloseTo(0.45, 2);
  await engine.call("exit-rig-preview");
});

test("set-skeleton-highlight tints a joint without moving scene selection", async () => {
  await engine.call("exit-rig-preview");
  const res = await engine.call<EnterResult>("enter-rig-preview", { asset: legModel });
  const rig = await engine.call<{ bones: { index: number; joint: boolean }[] }>("get-rig", {
    asset: legModel,
  });
  const joint = rig.bones.find((b) => b.joint)!;
  const overlay = await engine.call<{ highlightJoint: number; show: boolean }>("set-skeleton-highlight", {
    joint: joint.index,
  });
  expect(overlay.highlightJoint).toBe(joint.index);
  expect(overlay.show).toBe(true); // preview defaults the overlay on
  // Selection stayed on the rig (the highlight uses a dedicated channel, not scene selection), so the
  // selection-keyed animation state the timeline reads is still resolvable.
  const state = await engine.call<{ time: number }>("get-animation-state", { entity: res.rigEntity });
  expect(state).toBeDefined();
  await engine.call("set-skeleton-highlight", { joint: -1 });
  await engine.call("exit-rig-preview");
});

test("set-rig-preview-options toggles the floor slab live", async () => {
  await engine.call("exit-rig-preview");
  await engine.call("enter-rig-preview", { asset: legModel });
  const withFloor = (await engine.call<{ entities: unknown[] }>("list-entities")).entities.length;
  const off = await engine.call<{ floor: boolean }>("set-rig-preview-options", { floor: false });
  expect(off.floor).toBe(false);
  const withoutFloor = (await engine.call<{ entities: unknown[] }>("list-entities")).entities.length;
  expect(withoutFloor).toBe(withFloor - 1);
  const on = await engine.call<{ floor: boolean }>("set-rig-preview-options", { floor: true });
  expect(on.floor).toBe(true);
  expect((await engine.call<{ entities: unknown[] }>("list-entities")).entities.length).toBe(withFloor);
  await engine.call("exit-rig-preview");
});

test("the engine logged no validation errors", async () => {
  await engine.settle();
  expect(engine.validationErrors()).toEqual([]);
});
