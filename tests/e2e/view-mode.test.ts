// View mode: the debug render-output selector (set-view-mode {lit|wireframe}). Drives the control
// command (echo), proves the value reads back through render-stats (no get-view-mode), proves
// wireframe actually changes the render (llvmpipe supports fillModeNonSolid), and asserts the run
// stays validation-clean.

import { afterAll, beforeAll, expect, test } from "bun:test";
import { existsSync, readFileSync, rmSync } from "node:fs";
import { Engine } from "./harness.ts";

let engine: Engine;
const shots: string[] = [];

interface Ref {
  id: string;
  name: string;
}

interface ViewModeResult {
  viewMode: string;
}

interface RenderStats {
  viewMode: string;
}

beforeAll(async () => {
  engine = await Engine.boot({ SAFFRON_AUTO_EMPTY_PROJECT: "1" });
  await engine.call("set-camera", { yaw: 0, pitch: 0 });
  const cube = await engine.call<Ref>("add-entity", { args: ["cube"] });
  await engine.call("focus", { entity: cube.id });
  await engine.settle();
});
afterAll(async () => {
  await engine?.shutdown();
  for (const shot of shots) {
    rmSync(shot, { force: true });
  }
});

async function screenshot(tag: string): Promise<Buffer> {
  const path = `/tmp/saffron-e2e-viewmode-${process.pid}-${tag}.png`;
  shots.push(path);
  await engine.call("screenshot", { target: "viewport", path });
  const deadline = Date.now() + 10_000;
  while (!existsSync(path)) {
    if (Date.now() > deadline) {
      throw new Error(`screenshot ${tag} never landed at ${path}`);
    }
    await engine.settle(100);
  }
  await engine.settle(200);
  return readFileSync(path);
}

test("the default view mode is lit", async () => {
  const stats = await engine.call<RenderStats>("render-stats", {});
  expect(stats.viewMode).toBe("lit");
});

test("set-view-mode echoes the mode and reads back through render-stats", async () => {
  const set = await engine.call<ViewModeResult>("set-view-mode", { mode: "wireframe" });
  expect(set.viewMode).toBe("wireframe");
  const stats = await engine.call<RenderStats>("render-stats", {});
  expect(stats.viewMode).toBe("wireframe");
});

test("wireframe changes the render", async () => {
  await engine.call("set-view-mode", { mode: "lit" });
  await engine.settle(300);
  const lit = await screenshot("lit");
  await engine.call("set-view-mode", { mode: "wireframe" });
  await engine.settle(300);
  const wire = await screenshot("wire");
  expect(wire.equals(lit)).toBe(false);
});

test("a buffer channel (albedo) round-trips and changes the render", async () => {
  const set = await engine.call<ViewModeResult>("set-view-mode", { mode: "albedo" });
  expect(set.viewMode).toBe("albedo");
  await engine.call("set-view-mode", { mode: "lit" });
  await engine.settle(300);
  const lit = await screenshot("lit2");
  await engine.call("set-view-mode", { mode: "albedo" });
  await engine.settle(300);
  const albedo = await screenshot("albedo");
  expect(albedo.equals(lit)).toBe(false);
});

test("the engine logged no validation errors", () => {
  expect(engine.validationErrors()).toEqual([]);
});
