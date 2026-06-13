// Undo/redo is implemented ENTIRELY in the editor (the engine has no undo concept). This
// e2e proves only the engine-side PRIMITIVE the React history depends on: replaying a
// command with a prior value restores that prior state, observable via inspect. It does
// NOT exercise the editor's history bookkeeping (the per-tab stacks, gesture grouping,
// truncate-on-create-undo) — those are the editor/src/lib/undo.test.ts bun unit tests.
// The e2e suite drives the engine over the wire, so an editor-only feature reaches only
// this far here.

import { afterAll, beforeAll, expect, test } from "bun:test";
import { Engine } from "./harness.ts";

let engine: Engine;
beforeAll(async () => {
  engine = await Engine.boot();
});
afterAll(async () => {
  await engine?.shutdown();
});

interface Vec3 {
  x: number;
  y: number;
  z: number;
}
type Inspected = {
  components: { Transform: { translation: Vec3 }; Name: { name: string } };
};

const translationOf = async (entity: string): Promise<Vec3> =>
  (await engine.call<Inspected>("inspect", { entity })).components.Transform.translation;

test("set-transform inverse restores the prior translation (the undo primitive)", async () => {
  const name = "e2e-undo-transform";
  await engine.call("create-entity", { args: [name] }); // adds a Transform at the origin
  const prior = await translationOf(name); // the value the editor would capture before editing

  // An edit to A is observable...
  await engine.call("set-transform", { entity: name, translation: { x: 4, y: -1, z: 2.5 } });
  expect(await translationOf(name)).toEqual({ x: 4, y: -1, z: 2.5 });

  // ...then the inverse — the same command fed the captured prior — restores it.
  await engine.call("set-transform", { entity: name, translation: prior });
  expect(await translationOf(name)).toEqual(prior);
});

test("rename-entity inverse restores the prior name (a second inverse oracle)", async () => {
  const name = "e2e-undo-rename";
  await engine.call("create-entity", { args: [name] });

  await engine.call("rename-entity", { entity: name, name: "e2e-undo-renamed" });
  expect(
    (await engine.call<Inspected>("inspect", { entity: "e2e-undo-renamed" })).components.Name.name,
  ).toBe("e2e-undo-renamed");

  // The inverse rename (back to the captured prior name) restores it.
  await engine.call("rename-entity", { entity: "e2e-undo-renamed", name });
  expect((await engine.call<Inspected>("inspect", { entity: name })).components.Name.name).toBe(name);
});
