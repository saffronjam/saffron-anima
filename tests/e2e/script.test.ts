// Per-entity Lua scripting over the control plane: a ScriptComponent slot moves its
// entity only inside the play duplicate (the discard guarantee holds), slots on one
// entity run in list order within a tick, and a script error is contained — it lands
// in the drain-script-errors ring with a traceback, pauses play, and never crashes
// the host. Test scripts are authored into the auto project's src/ on the fly.

import { afterAll, beforeAll, expect, test } from "bun:test";
import { existsSync, mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { isAbsolute, join } from "node:path";
import { Engine, REPO } from "./harness.ts";

let engine: Engine;
let srcDir: string;

interface Ref {
  id: string;
  name: string;
}
interface Inspect {
  id: string;
  name: string;
  components: Record<string, any>;
}
interface PlayState {
  state: string;
}
interface ScriptStatus {
  state: string;
  instances: number;
  errorHighWater: number;
}
interface ScriptErrors {
  events: { seq: number; entity: string; script: string; message: string; tick: number }[];
  highWaterSeq: number;
  oldestSeq: number;
  overflowed: boolean;
}

beforeAll(async () => {
  engine = await Engine.boot({ SAFFRON_AUTO_EMPTY_PROJECT: "1" });
  // The auto project's root is relative to the engine's cwd (the repo root).
  const project = await engine.call<{ root: string }>("get-project");
  const root = isAbsolute(project.root) ? project.root : join(REPO, project.root);
  srcDir = join(root, "src");
  mkdirSync(srcDir, { recursive: true });

  writeFileSync(
    join(srcDir, "move.lua"),
    `local Mover = {}
function Mover.on_update(self, dt)
  local p = self.entity:get_position()
  self.entity:set_position(p.x + dt, p.y, p.z)
end
return Mover
`,
  );
  writeFileSync(
    join(srcDir, "player.lua"),
    `local Player = {}
function Player.on_update(self, dt)
  if se.is_key_pressed("w") then
    local p = self.entity:get_position()
    self.entity:set_position(p.x + dt, p.y, p.z)
  end
end
return Player
`,
  );
  writeFileSync(
    join(srcDir, "first.lua"),
    `local First = {}
function First.on_update(self, dt)
  self.entity:set_position(5, 0, 0)
end
return First
`,
  );
  writeFileSync(
    join(srcDir, "second.lua"),
    `local Second = {}
function Second.on_update(self, dt)
  local p = self.entity:get_position()
  self.entity:set_position(p.x, p.x * 2, p.z)
end
return Second
`,
  );
  writeFileSync(
    join(srcDir, "boom.lua"),
    `local Boom = {}
function Boom.on_update(self, dt)
  error("boom")
end
return Boom
`,
  );
  // Lua-side asserts fail the tick, which pauses play — the tests detect API
  // breakage as state ~= "playing". Writes derive only from never-written fields
  // so every tick is idempotent.
  writeFileSync(
    join(srcDir, "reader.lua"),
    `local Reader = {}
function Reader.on_update(self, dt)
  assert(self.entity:valid(), "self.entity must be valid")
  assert(self.entity:name() == "Reader Cube", "name() mismatch: " .. self.entity:name())
  assert(self.entity:get_component("NoSuchComponent") == nil, "unknown component must be nil")
  local t = self.entity:get_component("Transform")
  assert(t ~= nil, "Transform snapshot missing")
  self.entity:set_position(t.translation.z * 2, 50, t.translation.z)
  self.entity:set_rotation(0.5, 0, 0)
  self.entity:set_scale(2, 2, 2)
end
return Reader
`,
  );
  writeFileSync(
    join(srcDir, "chaser.lua"),
    `local Chaser = {}
function Chaser.on_update(self, dt)
  assert(not se.get_entity_by_name("No Such Entity"):valid(), "missing lookup must be invalid")
  local target = se.get_entity_by_name("Target")
  if target:valid() then
    local p = target:get_position()
    target:set_position(p.x, p.y, p.z + dt)
  end
end
return Chaser
`,
  );
  writeFileSync(
    join(srcDir, "camera.lua"),
    `local Cam = {}
function Cam.on_update(self, dt)
  local cam = se.primary_camera()
  if cam:valid() then
    cam:set_position(0, 5, 10)
  end
end
return Cam
`,
  );
  // Declared fields: defaults live in the .lua; the scene stores only overrides.
  // `weird` is deliberately uninferable (2 numbers, not a vec3) and must be skipped.
  writeFileSync(
    join(srcDir, "turret.lua"),
    `local Turret = {}
Turret.properties = {
  speed = 2.0,
  label = "idle",
  enabled = true,
  offset = { 0, 1, 0 },
  weird = { 1, 2 },
}
function Turret.on_update(self, dt)
  assert(self.label == "idle" or self.label == "fast", "label: " .. tostring(self.label))
  assert(type(self.enabled) == "boolean", "enabled must be a bool")
  assert(self.offset[2] == 1, "offset must inject as a 3-number table")
  if self.enabled then
    local p = self.entity:get_position()
    self.entity:set_position(p.x + self.speed * dt, p.y, p.z)
  end
end
return Turret
`,
  );
});
afterAll(async () => {
  await engine?.shutdown();
});

async function attachScripts(entityId: string, paths: string[]): Promise<void> {
  await engine.call("add-component", { entity: entityId, component: "Script" });
  await engine.call("set-component", {
    entity: entityId,
    component: "Script",
    json: { scripts: paths.map((scriptPath) => ({ scriptPath, overrides: {} })) },
  });
}

test("a script slot moves its entity during play; stop restores the authored scene", async () => {
  const cube = await engine.call<Ref>("add-entity", { args: ["cube"] });
  await engine.call("set-transform", { entity: cube.id, translation: { x: 1, y: 2, z: 3 } });
  await attachScripts(cube.id, ["move.lua"]);

  // The slot list is authored data, visible in the inspector wire shape.
  const authored = await engine.call<Inspect>("inspect", { entity: cube.id });
  expect(authored.components.Script.scripts).toEqual([{ scriptPath: "move.lua", overrides: {} }]);

  await engine.call("play");
  const status = await engine.call<ScriptStatus>("get-script-status");
  expect(status.state).toBe("playing");
  expect(status.instances).toBe(1);

  await engine.settle(400);
  const during = await engine.call<Inspect>("inspect", { entity: cube.id });
  expect(during.components.Transform.translation.x).toBeGreaterThan(1.05); // drifted +X by ~0.4s of dt
  expect(during.components.Transform.translation.y).toBeCloseTo(2);

  await engine.call("stop");
  const after = await engine.call<Inspect>("inspect", { entity: cube.id });
  expect(after.components.Transform.translation).toEqual({ x: 1, y: 2, z: 3 }); // the discard is the restore
  expect((await engine.call<ScriptStatus>("get-script-status")).instances).toBe(0);
  await engine.call("destroy-entity", { entity: cube.id });
});

test("script input exposes held keys to Lua", async () => {
  const cube = await engine.call<Ref>("add-entity", { args: ["cube"] });
  await engine.call("set-transform", { entity: cube.id, translation: { x: 1, y: 2, z: 3 } });
  await attachScripts(cube.id, ["player.lua"]);

  await engine.call("script-input", { keys: ["w"] });
  await engine.call("play");
  await engine.settle(300);
  const moved = await engine.call<Inspect>("inspect", { entity: cube.id });
  expect(moved.components.Transform.translation.x).toBeGreaterThan(1.05);

  await engine.call("script-input", { keys: [] });
  const stoppedAt = (await engine.call<Inspect>("inspect", { entity: cube.id })).components.Transform.translation.x;
  await engine.settle(300);
  const stopped = await engine.call<Inspect>("inspect", { entity: cube.id });
  expect(stopped.components.Transform.translation.x).toBeCloseTo(stoppedAt);

  await engine.call("stop");
  await engine.call("destroy-entity", { entity: cube.id });
});

test("slots on one entity run in list order within a tick", async () => {
  const cube = await engine.call<Ref>("add-entity", { args: ["cube"] });
  await attachScripts(cube.id, ["first.lua", "second.lua"]);

  await engine.call("play");
  expect((await engine.call<ScriptStatus>("get-script-status")).instances).toBe(2);
  await engine.settle();

  // second.lua reads the x first.lua wrote this same tick: y == x * 2 only if
  // slot order held. Reversed order would leave y stale on every frame.
  const during = await engine.call<Inspect>("inspect", { entity: cube.id });
  expect(during.components.Transform.translation.x).toBeCloseTo(5);
  expect(during.components.Transform.translation.y).toBeCloseTo(10);

  await engine.call("stop");
  await engine.call("destroy-entity", { entity: cube.id });
});

test("a script error is contained: drained with a traceback, play pauses, the host survives", async () => {
  const cube = await engine.call<Ref>("add-entity", { args: ["cube"] });
  await attachScripts(cube.id, ["boom.lua"]);

  await engine.call("play");
  await engine.settle();

  expect((await engine.call<PlayState>("get-play-state")).state).toBe("paused");

  const drained = await engine.call<ScriptErrors>("drain-script-errors", { since: 0 });
  expect(drained.events.length).toBeGreaterThan(0);
  const event = drained.events[0]!;
  expect(event.script).toBe("boom.lua");
  expect(event.message).toContain("boom");
  expect(event.message).toContain("stack traceback");
  expect(event.entity).toBe(cube.id);
  expect(drained.highWaterSeq).toBeGreaterThanOrEqual(event.seq);

  // The cursor protocol: draining from the high-water returns nothing new.
  const again = await engine.call<ScriptErrors>("drain-script-errors", { since: drained.highWaterSeq });
  expect(again.events).toEqual([]);

  // The host is alive and play still stops cleanly.
  await engine.call("ping");
  expect((await engine.call<PlayState>("stop")).state).toBe("edit");
  await engine.call("destroy-entity", { entity: cube.id });
});

test("component snapshots, name(), and the rotation/scale setters work from Lua", async () => {
  const cube = await engine.call<Ref>("add-entity", { args: ["cube"] });
  await engine.call("rename-entity", { entity: cube.id, name: "Reader Cube" });
  await engine.call("set-transform", { entity: cube.id, translation: { x: 1, y: 2, z: 3 } });
  await attachScripts(cube.id, ["reader.lua"]);

  await engine.call("play");
  await engine.settle();
  // Any failed Lua assert pauses play, so "playing" means the whole API behaved.
  expect((await engine.call<PlayState>("get-play-state")).state).toBe("playing");

  const during = await engine.call<Inspect>("inspect", { entity: cube.id });
  expect(during.components.Transform.translation).toEqual({ x: 6, y: 50, z: 3 });
  expect(during.components.Transform.rotation.x).toBeCloseTo(0.5);
  expect(during.components.Transform.scale).toEqual({ x: 2, y: 2, z: 2 });

  await engine.call("stop");
  const after = await engine.call<Inspect>("inspect", { entity: cube.id });
  expect(after.components.Transform.translation).toEqual({ x: 1, y: 2, z: 3 });
  expect(after.components.Transform.scale).toEqual({ x: 1, y: 1, z: 1 });
  await engine.call("destroy-entity", { entity: cube.id });
});

test("a script reaches another entity by name and moves it", async () => {
  const target = await engine.call<Ref>("add-entity", { args: ["cube"] });
  await engine.call("rename-entity", { entity: target.id, name: "Target" });
  const driver = await engine.call<Ref>("add-entity", { args: ["empty"] });
  await attachScripts(driver.id, ["chaser.lua"]);

  await engine.call("play");
  await engine.settle(400);
  expect((await engine.call<PlayState>("get-play-state")).state).toBe("playing");
  const during = await engine.call<Inspect>("inspect", { entity: target.id });
  expect(during.components.Transform.translation.z).toBeGreaterThan(0.05); // chased +Z by ~0.4s of dt

  await engine.call("stop");
  const after = await engine.call<Inspect>("inspect", { entity: target.id });
  expect(after.components.Transform.translation.z).toBe(0);
  await engine.call("destroy-entity", { entity: target.id });
  await engine.call("destroy-entity", { entity: driver.id });
});

test("a script moves the primary camera through its transform", async () => {
  const camera = await engine.call<Ref>("add-entity", { args: ["camera"] });
  const driver = await engine.call<Ref>("add-entity", { args: ["empty"] });
  await attachScripts(driver.id, ["camera.lua"]);

  await engine.call("play");
  await engine.settle();
  expect((await engine.call<PlayState>("get-play-state")).state).toBe("playing");
  const during = await engine.call<Inspect>("inspect", { entity: camera.id });
  expect(during.components.Transform.translation).toEqual({ x: 0, y: 5, z: 10 });

  await engine.call("stop");
  const after = await engine.call<Inspect>("inspect", { entity: camera.id });
  expect(after.components.Transform.translation).not.toEqual({ x: 0, y: 5, z: 10 });
  await engine.call("destroy-entity", { entity: camera.id });
  await engine.call("destroy-entity", { entity: driver.id });
});

interface ScriptSchema {
  fields: { name: string; type: string; defaultValue: unknown }[];
}

test("get-script-schema reads declared fields with inferred types, sorted by name", async () => {
  const schema = await engine.call<ScriptSchema>("get-script-schema", { path: "turret.lua" });
  expect(schema.fields).toEqual([
    { name: "enabled", type: "bool", defaultValue: true },
    { name: "label", type: "string", defaultValue: "idle" },
    { name: "offset", type: "vec3", defaultValue: [0, 1, 0] },
    { name: "speed", type: "number", defaultValue: 2 },
  ]); // `weird` (a 2-number table) is skipped, not an error

  await expect(engine.call("get-script-schema", { path: "does-not-exist.lua" })).rejects.toThrow();
  await expect(engine.call("get-script-schema", { path: "../escape.lua" })).rejects.toThrow(/relative/);
});

test("declared defaults drive the script; an override on the slot wins", async () => {
  const cube = await engine.call<Ref>("add-entity", { args: ["cube"] });
  await attachScripts(cube.id, ["turret.lua"]);

  // No overrides: the turret moves at the declared default (speed = 2).
  await engine.call("play");
  await engine.settle(400);
  expect((await engine.call<PlayState>("get-play-state")).state).toBe("playing"); // no Lua assert tripped
  const defaultRun = await engine.call<Inspect>("inspect", { entity: cube.id });
  const defaultX = defaultRun.components.Transform.translation.x;
  expect(defaultX).toBeGreaterThan(0.4);
  await engine.call("stop");

  // Override speed and label on the authored slot; the next session reads them.
  const written = await engine.call<{ scriptPath: string; overrides: Record<string, unknown> }>(
    "set-script-override",
    { entity: cube.id, slot: 0, name: "speed", value: 10 },
  );
  expect(written.overrides).toEqual({ speed: 10 });
  await engine.call("set-script-override", { entity: cube.id, slot: 0, name: "label", value: "fast" });

  await engine.call("play");
  await engine.settle(400);
  expect((await engine.call<PlayState>("get-play-state")).state).toBe("playing");
  const overriddenX = (await engine.call<Inspect>("inspect", { entity: cube.id })).components.Transform
    .translation.x;
  await engine.call("stop");
  expect(overriddenX).toBeGreaterThan(defaultX * 2); // 5x the rate, generous margin

  // A null value clears the override; a stale key (renamed/removed field) is
  // ignored at injection, never an error.
  const cleared = await engine.call<{ overrides: Record<string, unknown> }>("set-script-override", {
    entity: cube.id,
    slot: 0,
    name: "speed",
    value: null,
  });
  expect(cleared.overrides).toEqual({ label: "fast" });
  await engine.call("set-script-override", { entity: cube.id, slot: 0, name: "renamed_away", value: 99 });
  await engine.call("play");
  await engine.settle();
  expect((await engine.call<PlayState>("get-play-state")).state).toBe("playing");
  await engine.call("stop");
  await engine.call("destroy-entity", { entity: cube.id });
});

test("a new project scaffolds src/ with a runnable starter script", async () => {
  // createProject (which the auto-empty boot rides) ensures src/ + example.lua.
  const example = join(srcDir, "example.lua");
  expect(existsSync(example)).toBe(true);
  const text = readFileSync(example, "utf8");
  expect(text).toContain("Example.properties");
  expect(text).toContain("on_update");

  // The starter is immediately demonstrable: attach, play, and it orbits the
  // authored spot in the x/y plane. The angle depends on wall-clock timing, but
  // the orbit invariant doesn't: the cube stays `radius` from the circle's
  // center (one radius left of the authored position) at all times.
  const cube = await engine.call<Ref>("add-entity", { args: ["cube"] });
  await attachScripts(cube.id, ["example.lua"]);
  await engine.call("play");
  await engine.settle(400);
  expect((await engine.call<PlayState>("get-play-state")).state).toBe("playing");
  const during = await engine.call<Inspect>("inspect", { entity: cube.id });
  const p = during.components.Transform.translation;
  expect(p.y).toBeGreaterThan(0.05); // ~sin(0.4s * speed) * radius, well off the start
  const radius = Math.hypot(p.x - -2, p.y - 0); // center = authored (0,0) - (radius, 0)
  expect(radius).toBeCloseTo(2, 1);
  await engine.call("stop");
  await engine.call("destroy-entity", { entity: cube.id });
});

test("create-script writes a runnable class-table boilerplate and rejects duplicates", async () => {
  const created = await engine.call<{ path: string }>("create-script", { name: "spawner" });
  expect(created.path).toBe("spawner.lua"); // .lua appended
  const text = readFileSync(join(srcDir, "spawner.lua"), "utf8");
  expect(text).toContain("local Spawner = {}");
  expect(text).toContain("Spawner.properties");
  expect(text).toContain("function Spawner.on_update(self, dt)");

  await expect(engine.call("create-script", { name: "spawner.lua" })).rejects.toThrow(/exists/);
  await expect(engine.call("create-script", { name: "../escape" })).rejects.toThrow(/invalid/);

  // The boilerplate is valid as written: attach + play stays clean.
  const cube = await engine.call<Ref>("add-entity", { args: ["cube"] });
  await attachScripts(cube.id, ["spawner.lua"]);
  await engine.call("play");
  await engine.settle();
  expect((await engine.call<ScriptStatus>("get-script-status")).instances).toBe(1);
  expect((await engine.call<PlayState>("get-play-state")).state).toBe("playing");
  await engine.call("stop");
  await engine.call("destroy-entity", { entity: cube.id });
});

test("a missing script file is a logged skip, not a crash", async () => {
  const cube = await engine.call<Ref>("add-entity", { args: ["cube"] });
  await attachScripts(cube.id, ["does-not-exist.lua"]);

  await engine.call("play");
  expect((await engine.call<ScriptStatus>("get-script-status")).instances).toBe(0);
  expect((await engine.call<PlayState>("get-play-state")).state).toBe("playing");
  await engine.call("stop");
  await engine.call("destroy-entity", { entity: cube.id });
});

test("the scripting cases leave the validation log clean", () => {
  expect(engine.validationErrors()).toEqual([]);
});
