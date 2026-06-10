import { afterAll, beforeAll, test } from "bun:test";
import { existsSync, readFileSync, rmSync } from "node:fs";
import { join } from "node:path";
import { Engine, REPO } from "./harness.ts";

let engine: Engine;
const FIXTURE = join(REPO, "tests", "e2e", "fixtures", "skinned-strip.gltf");

beforeAll(async () => {
  engine = await Engine.boot({ SAFFRON_AUTO_EMPTY_PROJECT: "1" });
  await engine.call("set-camera", { yaw: 0, pitch: 0 });
});
afterAll(async () => {
  await engine?.shutdown();
});

async function shot(tag: string): Promise<Buffer> {
  const path = `/tmp/zzdbg-${tag}.png`;
  rmSync(path, { force: true });
  await engine.call("screenshot", { target: "viewport", path });
  const deadline = Date.now() + 10000;
  while (!existsSync(path)) {
    if (Date.now() > deadline) throw new Error("no shot");
    await engine.settle(100);
  }
  await engine.settle(200);
  return readFileSync(path);
}

test("palette vs readback", async () => {
  const imported = await engine.call<{ id: string }>("import-model", { path: FIXTURE });
  const meshId = imported.id;
  await engine.settle();
  await engine.call("focus", { entity: meshId });
  await engine.settle(400);
  const list = (await engine.call<{ entities: { id: string; name: string }[] }>("list-entities")).entities;
  const tip = list.find((e) => e.name === "TipJoint")!;
  const pal = () => engine.log.split("\n").filter((l) => l.includes("DBG palette"));
  const cap = () => engine.log.split("\n").filter((l) => l.includes("DBG capture"));

  for (let i = 0; i < 4; i++) {
    await engine.call("set-transform", { entity: tip.id, translation: { x: 2, y: 1, z: 0 } });
    await engine.settle(400);
    const np = pal().length;
    const nc = cap().length;
    const moved = await shot("moved");
    console.log(`iter ${i} after MOVED set (live tipX should -> ~2):`);
    console.log(`   last 3 palette logs: ${pal().slice(np).slice(-3).map((l) => l.split("DBG ")[1]).join(" | ")}`);
    console.log(`   capture readback:    ${(cap().slice(nc).pop() ?? "(none)").split("DBG ")[1] ?? "(none)"}`);
    console.log(`   moved size ${moved.length}`);
    // reset
    await engine.call("set-transform", { entity: tip.id, translation: { x: 0, y: 0, z: 0 } });
    await engine.settle(400);
    await shot("bind");
  }
}, 120000);
