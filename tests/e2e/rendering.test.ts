// End-to-end rendering tests: drive a real headless engine over the control plane and
// assert on its responses + the engine's own validation output. Run with `bun test`
// (or `make e2e`), inside the saffron-build toolbox.

import { afterAll, beforeAll, expect, test } from "bun:test";
import { Engine } from "./harness.ts";
import type { EntityList, RenderStats } from "@saffron/protocol";


let engine: Engine;
beforeAll(async () => {
  // import-model needs a loaded project; auto-create an empty one (under the gitignored appdata/).
  engine = await Engine.boot({ SAFFRON_AUTO_EMPTY_PROJECT: "1" });
});
afterAll(async () => {
  await engine?.shutdown();
});

test("boots clean: ping answers and no validation errors at startup", async () => {
  const pong = await engine.call<{ pong: boolean }>("ping");
  expect(pong.pong).toBe(true);
  expect(engine.validationErrors()).toEqual([]);
});

test("imports a model and reports a draw", async () => {
  await engine.call("add-entity", { preset: "cube" });
  const entities = await engine.call<EntityList>("list-entities");
  expect(Array.isArray(entities.entities)).toBe(true);
  const stats = await engine.call<RenderStats>("render-stats");
  expect(stats.drawCalls).toBeGreaterThan(0);
});

// Regression: set-aa msaa2 used to create the MSAA color/depth images at a sample count
// (2x) that the R16G16B16A16_SFLOAT / D32_SFLOAT target formats reject on some GPUs
// (VUID-VkImageCreateInfo-samples-02258) — the count was clamped to the generic framebuffer
// limit, not the per-format support. setAa now clamps to the formats' actual support.
test("set-aa across every level stays Vulkan-validation-clean", async () => {
  for (const mode of ["msaa2", "msaa4", "msaa8", "msaa2", "off"]) {
    const result = await engine.call<{ aa: string }>("set-aa", { args: [mode] });
    expect(typeof result.aa).toBe("string");
    await engine.settle(200);
  }
  expect(engine.validationErrors()).toEqual([]);
});
