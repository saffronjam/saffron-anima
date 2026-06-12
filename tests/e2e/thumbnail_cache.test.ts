// The engine caches generated thumbnails to <projectRoot>/cache/thumbnails/ so they survive a
// restart: a second engine serves the persisted PNG without re-rendering, and a source-file edit
// (bumped mtime) invalidates the entry and regenerates. The cache key folds uuid + size + a source
// stat, so a stale entry is simply never matched again.

import { afterAll, beforeAll, expect, test } from "bun:test";
import {
  existsSync,
  mkdtempSync,
  readdirSync,
  rmSync,
  utimesSync,
  writeFileSync,
} from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { Engine } from "./harness.ts";
import { makePng, pngSize } from "./imggen.ts";

let root: string;
let id: string;

function cachePngs(): string[] {
  const dir = join(root, "cache", "thumbnails");
  return existsSync(dir) ? readdirSync(dir).filter((f) => f.endsWith(".png")) : [];
}

beforeAll(() => {
  root = mkdtempSync(join(tmpdir(), "saffron-thumbcache-"));
});
afterAll(() => {
  rmSync(root, { recursive: true, force: true });
});

test("thumbnails persist across an engine restart and invalidate on source edit", async () => {
  // First engine: create the project, import a texture, generate its thumbnail.
  const e1 = await Engine.boot();
  await e1.call("new-project", { name: "cachetest", root });
  const src = join(root, "src.png");
  writeFileSync(src, makePng(1024, 640, (x, y) => [x & 255, y & 255, (x ^ y) & 255]));
  const imported = await e1.call<{ texture: string }>("import-texture", { path: src });
  id = imported.texture;
  await e1.call("save-project", {}); // persist the catalog so the restart sees the texture

  const t1 = await e1.getThumbnail<{ base64: string; width: number; height: number }>("get-thumbnail", {
    asset: id,
    size: 128,
  });
  const real = Buffer.from(t1.base64, "base64");
  expect(cachePngs().length).toBe(1); // the miss wrote one cache file
  expect(e1.validationErrors()).toEqual([]);
  await e1.shutdown();

  // Replace the cached PNG with a distinct sentinel so a cache HIT is observable (a regenerated
  // thumbnail would be the deterministic `real` bytes, never the sentinel).
  const sentinel = makePng(40, 24, () => [10, 200, 90]);
  const cacheFile = join(root, "cache", "thumbnails", cachePngs()[0]);
  writeFileSync(cacheFile, sentinel);

  // Second engine: load the same project; the thumbnail must come from the persisted cache.
  const e2 = await Engine.boot();
  await e2.call("open-project", { path: root });
  const t2 = await e2.getThumbnail<{ base64: string; width: number; height: number }>("get-thumbnail", {
    asset: id,
    size: 128,
  });
  expect(Buffer.from(t2.base64, "base64").equals(sentinel)).toBe(true); // served from disk cache
  const dims = pngSize(sentinel);
  expect(t2.width).toBe(dims.width); // dimensions read truthfully from the cached PNG header
  expect(t2.height).toBe(dims.height);

  // Bump the source mtime → the stamp changes → the sentinel entry no longer matches → regenerate.
  const future = new Date(Date.now() + 4000);
  utimesSync(join(root, "assets", "textures", `${id}.png`), future, future);
  const t3 = await e2.getThumbnail<{ base64: string }>("get-thumbnail", { asset: id, size: 128 });
  expect(Buffer.from(t3.base64, "base64").equals(sentinel)).toBe(false); // not the stale entry
  expect(Buffer.from(t3.base64, "base64").equals(real)).toBe(true); // the freshly regenerated PNG
  expect(cachePngs().length).toBeGreaterThanOrEqual(2); // a new-stamp file was written
  expect(e2.validationErrors()).toEqual([]);
  await e2.shutdown();
});
