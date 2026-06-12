// Render-feature toggles, validated by READ-BACK: flip each via its set-* command, then
// query render-stats and assert the reported state actually changed — "ok:true" alone proves
// nothing. Most of these recreate GPU pipelines/targets, so the suite also asserts the engine
// stays Vulkan-validation-clean (the oracle that caught the MSAA sample-count bug).

import { afterAll, beforeAll, expect, test } from "bun:test";
import { Engine } from "./harness.ts";
import type { RenderStats } from "@saffron/protocol";


let engine: Engine;
const stats = () => engine.call<RenderStats & Record<string, unknown>>("render-stats");

beforeAll(async () => {
  // import-model needs a loaded project; auto-create an empty one (under the gitignored appdata/).
  engine = await Engine.boot({ SAFFRON_AUTO_EMPTY_PROJECT: "1" });
  await engine.call("add-entity", { preset: "cube" }); // geometry, so the passes actually run
});
afterAll(async () => {
  await engine?.shutdown();
});

// Plain on/off toggles whose state render-stats echoes back under `field`.
const BOOLEAN_TOGGLES = [
  { cmd: "set-ssao", field: "ssao" },
  { cmd: "set-shadows", field: "shadows" },
  { cmd: "set-ibl", field: "ibl" },
  { cmd: "set-clustered", field: "clustered" },
  { cmd: "set-depth-prepass", field: "depthPrepass" },
  { cmd: "set-contact-shadows", field: "contactShadows" },
  { cmd: "set-ssgi", field: "ssgi" },
];

for (const { cmd, field } of BOOLEAN_TOGGLES) {
  test(`${cmd} round-trips through render-stats`, async () => {
    await engine.call(cmd, { args: [1] });
    expect((await stats())[field]).toBe(true);
    await engine.call(cmd, { args: [0] });
    expect((await stats())[field]).toBe(false);
  });
}

test("set-gi ddgi|off round-trips through render-stats.ddgi", async () => {
  await engine.call("set-gi", { args: ["ddgi"] });
  expect((await stats()).ddgi).toBe(true);
  await engine.call("set-gi", { args: ["off"] });
  expect((await stats()).ddgi).toBe(false);
});

test("set-exposure is reflected in render-stats.exposureEv", async () => {
  const r = await engine.call<{ exposureEv: number }>("set-exposure", { args: [1.5] });
  expect(r.exposureEv).toBeCloseTo(1.5, 3);
  expect((await stats()).exposureEv).toBeCloseTo(1.5, 3);
  await engine.call("set-exposure", { args: [0] });
});

test("ray-tracing toggles round-trip when the device supports RT", async () => {
  const s = await stats();
  if (!s.rtSupported) {
    return; // llvmpipe without ray_query / real GPU without RT — nothing to assert
  }
  await engine.call("set-rt-shadows", { args: [1] });
  expect((await stats()).rtShadows).toBe(true);
  await engine.call("set-restir", { args: [1] });
  expect((await stats()).restir).toBe(true);
  await engine.call("set-rt-shadows", { args: [0] });
  await engine.call("set-restir", { args: [0] });
});

// Runs last: by now every toggle has recreated its GPU state at least once.
test("exercising every toggle left no Vulkan validation errors", async () => {
  await engine.settle(400);
  expect(engine.validationErrors()).toEqual([]);
});
