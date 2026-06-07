// `set-material`/`set-transform smooth:1` animate fields toward the target over a few
// frames (the gizmo-style exponential step) and snap exactly on convergence, so a
// settled read-back must equal the target verbatim. A non-smooth write cancels any
// pending animation — the exact value always wins. Targets use f32-exact literals so
// the JSON round-trip compares with toEqual.

import { afterAll, beforeAll, expect, test } from "bun:test";
import { Engine } from "./harness.ts";

let engine: Engine;
beforeAll(async () => {
  engine = await Engine.boot();
});
afterAll(async () => {
  await engine?.shutdown();
});

interface MaterialInspect {
  components: {
    Material: {
      baseColor: { x: number; y: number; z: number; w: number };
      roughness: number;
      metallic: number;
    };
  };
}

test("smooth set-material converges exactly to the target", async () => {
  const name = "e2e-smooth-material";
  await engine.call("create-entity", { args: [name] });
  await engine.call("add-component", { entity: name, component: "Material" });

  const target = { x: 0.25, y: 0.5, z: 0.75, w: 1 };
  await engine.call("set-material", {
    entity: name,
    baseColor: target,
    roughness: 0.5,
    smooth: true,
  });
  // tau is 25ms; 400ms is ~16 time constants — converged and snapped.
  await engine.settle(400);

  const info = await engine.call<MaterialInspect>("inspect", { entity: name });
  expect(info.components.Material.baseColor).toEqual(target);
  expect(info.components.Material.roughness).toBe(0.5);
});

test("a non-smooth set-material overrides a pending smooth animation", async () => {
  const name = "e2e-smooth-cancel";
  await engine.call("create-entity", { args: [name] });
  await engine.call("add-component", { entity: name, component: "Material" });

  await engine.call("set-material", {
    entity: name,
    baseColor: { x: 1, y: 0, z: 0, w: 1 },
    smooth: true,
  });
  const exact = { x: 0, y: 0.25, z: 1, w: 0.5 };
  await engine.call("set-material", { entity: name, baseColor: exact });
  await engine.settle(400);

  const info = await engine.call<MaterialInspect>("inspect", { entity: name });
  expect(info.components.Material.baseColor).toEqual(exact);
});

interface TransformInspect {
  components: {
    Transform: {
      translation: { x: number; y: number; z: number };
      scale: { x: number; y: number; z: number };
    };
  };
}

test("smooth set-transform converges exactly to the target", async () => {
  const name = "e2e-smooth-transform";
  await engine.call("create-entity", { args: [name] }); // createEntity adds a Transform

  const translation = { x: 1.5, y: -2, z: 3.25 };
  const scale = { x: 2, y: 0.5, z: 1 };
  await engine.call("set-transform", { entity: name, translation, scale, smooth: true });
  await engine.settle(400);

  const info = await engine.call<TransformInspect>("inspect", { entity: name });
  expect(info.components.Transform.translation).toEqual(translation);
  expect(info.components.Transform.scale).toEqual(scale);
});

test("a non-smooth set-transform overrides a pending smooth animation", async () => {
  const name = "e2e-smooth-transform-cancel";
  await engine.call("create-entity", { args: [name] });

  await engine.call("set-transform", {
    entity: name,
    translation: { x: 10, y: 0, z: 0 },
    smooth: true,
  });
  const exact = { x: -1, y: 2.5, z: 0.75 };
  await engine.call("set-transform", { entity: name, translation: exact });
  await engine.settle(400);

  const info = await engine.call<TransformInspect>("inspect", { entity: name });
  expect(info.components.Transform.translation).toEqual(exact);
});

test("destroying the entity mid-smooth is harmless", async () => {
  const name = "e2e-smooth-destroyed";
  await engine.call("create-entity", { args: [name] });
  await engine.call("add-component", { entity: name, component: "Material" });
  await engine.call("set-material", {
    entity: name,
    baseColor: { x: 0, y: 1, z: 0, w: 1 },
    smooth: true,
  });
  await engine.call("destroy-entity", { entity: name });
  // The stepper must drop the orphaned entry without touching freed state; the
  // suite's validation-clean log assertion covers the rest.
  await engine.settle(200);
});
