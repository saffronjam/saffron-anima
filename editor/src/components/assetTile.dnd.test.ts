import { describe, expect, test } from "bun:test";
import {
  ASSET_DND_MIME,
  assetIdsFromPayload,
  catalogDragEffectAllowed,
  type AssetDragPayload,
  FOLDER_DND_MIME,
  firstModelAssetId,
  isCatalogDrag,
  readAssetPayload,
  readFolderPayload,
} from "./AssetTile";
import type { AssetEntry } from "../protocol";

/// A minimal DataTransfer stand-in: `getData(mime)` returns the stored string (or
/// "" for an absent mime, matching the DOM), and `types` lists the present mimes.
function makeDataTransfer(data: Record<string, string>): DataTransfer {
  return {
    getData: (mime: string): string => data[mime] ?? "",
    types: Object.keys(data),
  } as unknown as DataTransfer;
}

function withAsset(value: string): DataTransfer {
  return makeDataTransfer({ [ASSET_DND_MIME]: value });
}

function withFolder(value: string): DataTransfer {
  return makeDataTransfer({ [FOLDER_DND_MIME]: value });
}

describe("readAssetPayload", () => {
  test("returns null when the asset mime is absent", () => {
    expect(readAssetPayload(makeDataTransfer({}))).toBeNull();
  });

  test("returns null when the asset mime holds an empty string", () => {
    expect(readAssetPayload(withAsset(""))).toBeNull();
  });

  test("reads a single-id payload and carries its type through", () => {
    expect(readAssetPayload(withAsset(JSON.stringify({ id: "42", type: "mesh" })))).toEqual({
      id: "42",
      type: "mesh",
    });
  });

  test("reads a single-id payload with no type (type stays undefined)", () => {
    expect(readAssetPayload(withAsset(JSON.stringify({ id: "7" })))).toEqual({
      id: "7",
      type: undefined,
    });
  });

  test("prefers the ids list and drops everything else (no id, no type)", () => {
    expect(
      readAssetPayload(withAsset(JSON.stringify({ ids: ["a", "b"], id: "c", type: "texture" }))),
    ).toEqual({ ids: ["a", "b"] });
  });

  test("accepts an empty ids array as a valid multi payload", () => {
    expect(readAssetPayload(withAsset(JSON.stringify({ ids: [] })))).toEqual({ ids: [] });
  });

  test("falls back to id when ids has a non-string element", () => {
    expect(readAssetPayload(withAsset(JSON.stringify({ ids: ["a", 3], id: "fallback" })))).toEqual({
      id: "fallback",
      type: undefined,
    });
  });

  test("returns null when ids is non-string-array and there is no string id", () => {
    expect(readAssetPayload(withAsset(JSON.stringify({ ids: [1, 2] })))).toBeNull();
  });

  test("returns null when id is present but not a string", () => {
    expect(readAssetPayload(withAsset(JSON.stringify({ id: 99 })))).toBeNull();
  });

  test("returns null for a well-formed-but-empty object", () => {
    expect(readAssetPayload(withAsset(JSON.stringify({})))).toBeNull();
  });

  test("returns null for malformed JSON instead of throwing", () => {
    expect(readAssetPayload(withAsset("{ not json"))).toBeNull();
  });

  test("ignores a payload on the folder mime", () => {
    expect(readAssetPayload(withFolder(JSON.stringify({ id: "1" })))).toBeNull();
  });
});

describe("assetIdsFromPayload", () => {
  test("returns an empty array for a null payload", () => {
    expect(assetIdsFromPayload(null)).toEqual([]);
  });

  test("returns the ids list when it is non-empty", () => {
    expect(assetIdsFromPayload({ ids: ["x", "y"] })).toEqual(["x", "y"]);
  });

  test("prefers a non-empty ids list over a single id", () => {
    expect(assetIdsFromPayload({ ids: ["only"], id: "ignored" })).toEqual(["only"]);
  });

  test("falls back to the single id when ids is an empty array", () => {
    expect(assetIdsFromPayload({ ids: [], id: "solo" })).toEqual(["solo"]);
  });

  test("wraps a lone id in a one-element array", () => {
    expect(assetIdsFromPayload({ id: "lonely" })).toEqual(["lonely"]);
  });

  test("returns an empty array when neither ids nor id is present", () => {
    expect(assetIdsFromPayload({} as AssetDragPayload)).toEqual([]);
  });

  test("round-trips a single-id payload read off a DataTransfer", () => {
    const payload = readAssetPayload(withAsset(JSON.stringify({ id: "m1", type: "model" })));
    expect(assetIdsFromPayload(payload)).toEqual(["m1"]);
  });

  test("round-trips a multi-id payload read off a DataTransfer", () => {
    const payload = readAssetPayload(withAsset(JSON.stringify({ ids: ["m1", "m2"] })));
    expect(assetIdsFromPayload(payload)).toEqual(["m1", "m2"]);
  });
});

describe("catalogDragEffectAllowed", () => {
  test("asset drags allow copy placement and move organization", () => {
    expect(catalogDragEffectAllowed(["model-1"])).toBe("copyMove");
  });

  test("folder-only drags remain move-only", () => {
    expect(catalogDragEffectAllowed([])).toBe("move");
  });
});

describe("firstModelAssetId", () => {
  const assets = [
    { id: "texture-1", name: "Texture", path: "assets/texture.png", type: "texture" },
    { id: "model-1", name: "Model", path: "assets/model.smodel", type: "model" },
    { id: "model-2", name: "Second Model", path: "assets/second.smodel", type: "model" },
  ] satisfies AssetEntry[];

  test("returns the first dragged id whose catalog entry is a model", () => {
    expect(firstModelAssetId(["texture-1", "model-2", "model-1"], assets)).toBe("model-2");
  });

  test("returns null when the drag contains no model assets", () => {
    expect(firstModelAssetId(["texture-1", "missing"], assets)).toBeNull();
  });
});

describe("readFolderPayload", () => {
  test("returns an empty array when the folder mime is absent", () => {
    expect(readFolderPayload(makeDataTransfer({}))).toEqual([]);
  });

  test("returns an empty array when the folder mime holds an empty string", () => {
    expect(readFolderPayload(withFolder(""))).toEqual([]);
  });

  test("reads a paths list", () => {
    expect(readFolderPayload(withFolder(JSON.stringify({ paths: ["a/b", "c"] })))).toEqual([
      "a/b",
      "c",
    ]);
  });

  test("filters non-string and empty-string entries out of the paths list", () => {
    expect(
      readFolderPayload(withFolder(JSON.stringify({ paths: ["keep", "", 5, "also/keep"] }))),
    ).toEqual(["keep", "also/keep"]);
  });

  test("returns an empty array for a paths list with no valid entries", () => {
    expect(readFolderPayload(withFolder(JSON.stringify({ paths: ["", 1, null] })))).toEqual([]);
  });

  test("wraps a single non-empty path string in a one-element array", () => {
    expect(readFolderPayload(withFolder(JSON.stringify({ path: "assets/textures" })))).toEqual([
      "assets/textures",
    ]);
  });

  test("ignores an empty single path string", () => {
    expect(readFolderPayload(withFolder(JSON.stringify({ path: "" })))).toEqual([]);
  });

  test("prefers the paths list over a single path (paths takes precedence even if empty)", () => {
    expect(
      readFolderPayload(withFolder(JSON.stringify({ paths: ["from-list"], path: "ignored" }))),
    ).toEqual(["from-list"]);
  });

  test("an empty paths array short-circuits the single-path fallback", () => {
    expect(readFolderPayload(withFolder(JSON.stringify({ paths: [], path: "ignored" })))).toEqual(
      [],
    );
  });

  test("returns an empty array for a well-formed-but-empty object", () => {
    expect(readFolderPayload(withFolder(JSON.stringify({})))).toEqual([]);
  });

  test("returns an empty array for malformed JSON instead of throwing", () => {
    expect(readFolderPayload(withFolder("}{"))).toEqual([]);
  });

  test("ignores a folder payload sitting on the asset mime", () => {
    expect(readFolderPayload(withAsset(JSON.stringify({ path: "x" })))).toEqual([]);
  });
});

describe("isCatalogDrag", () => {
  test("true when the asset mime is among the types", () => {
    expect(isCatalogDrag(withAsset(JSON.stringify({ id: "1" })))).toBe(true);
  });

  test("true when the folder mime is among the types", () => {
    expect(isCatalogDrag(withFolder(JSON.stringify({ path: "p" })))).toBe(true);
  });

  test("true for a mixed asset + folder drag carrying both mimes", () => {
    const dt = makeDataTransfer({
      [ASSET_DND_MIME]: JSON.stringify({ id: "1" }),
      [FOLDER_DND_MIME]: JSON.stringify({ path: "p" }),
    });
    expect(isCatalogDrag(dt)).toBe(true);
  });

  test("false when no catalog mime is present", () => {
    expect(isCatalogDrag(makeDataTransfer({}))).toBe(false);
  });

  test("false for an unrelated mime such as an OS file drop", () => {
    expect(isCatalogDrag(makeDataTransfer({ "text/plain": "hello" }))).toBe(false);
  });
});
