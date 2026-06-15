import { describe, expect, test } from "bun:test";
import {
  type FolderNode,
  buildFolderTree,
  folderAncestorPaths,
  folderLabel,
} from "./AssetFolderTree";

describe("folderLabel", () => {
  test("returns the last segment of a multi-segment path", () => {
    expect(folderLabel("a/b/c")).toBe("c");
  });

  test("returns the whole string when there is no slash", () => {
    expect(folderLabel("root")).toBe("root");
  });

  test("returns the segment after the final slash for a two-segment path", () => {
    expect(folderLabel("textures/wood")).toBe("wood");
  });

  test("a trailing slash yields an empty label", () => {
    expect(folderLabel("a/b/")).toBe("");
  });

  test("the empty string maps to itself", () => {
    expect(folderLabel("")).toBe("");
  });
});

describe("folderAncestorPaths", () => {
  test('"a/b/c" => ["a", "a/b", "a/b/c"], shortest first, including itself', () => {
    expect(folderAncestorPaths("a/b/c")).toEqual(["a", "a/b", "a/b/c"]);
  });

  test("a single segment is its own only ancestor path", () => {
    expect(folderAncestorPaths("a")).toEqual(["a"]);
  });

  test("a two-segment path yields the parent then the full path", () => {
    expect(folderAncestorPaths("textures/wood")).toEqual(["textures", "textures/wood"]);
  });

  test("the last entry always equals the input", () => {
    const paths = folderAncestorPaths("x/y/z/w");
    expect(paths[paths.length - 1]).toBe("x/y/z/w");
    expect(paths).toEqual(["x", "x/y", "x/y/z", "x/y/z/w"]);
  });

  test("the empty string yields a single empty-string entry", () => {
    expect(folderAncestorPaths("")).toEqual([""]);
  });
});

describe("buildFolderTree", () => {
  test("an empty list yields no roots", () => {
    expect(buildFolderTree([])).toEqual([]);
  });

  test("a single top-level folder becomes one childless root", () => {
    expect(buildFolderTree(["models"])).toEqual([{ path: "models", name: "models", children: [] }]);
  });

  test("a nested path synthesizes its intermediate ancestor nodes", () => {
    // Only the deep leaf is in the flat list; "a" and "a/b" must be created.
    const roots = buildFolderTree(["a/b/c"]);
    expect(roots).toHaveLength(1);
    const a = roots[0];
    expect(a.path).toBe("a");
    expect(a.name).toBe("a");
    expect(a.children).toHaveLength(1);
    const ab = a.children[0];
    expect(ab.path).toBe("a/b");
    expect(ab.name).toBe("b");
    expect(ab.children).toHaveLength(1);
    const abc = ab.children[0];
    expect(abc.path).toBe("a/b/c");
    expect(abc.name).toBe("c");
    expect(abc.children).toEqual([]);
  });

  test("siblings under a shared explicit parent are nested under one node", () => {
    const roots = buildFolderTree(["assets", "assets/textures", "assets/models"]);
    expect(roots).toHaveLength(1);
    const assets = roots[0];
    expect(assets.path).toBe("assets");
    // Sorted by name (base sensitivity): models before textures.
    expect(assets.children.map((c) => c.path)).toEqual(["assets/models", "assets/textures"]);
  });

  test("an explicit parent listed after its child does not duplicate the synthesized node", () => {
    // "a" is synthesized when "a/b" is seen, then reused when "a" appears explicitly.
    const roots = buildFolderTree(["a/b", "a"]);
    expect(roots).toHaveLength(1);
    expect(roots[0].path).toBe("a");
    expect(roots[0].children.map((c) => c.path)).toEqual(["a/b"]);
  });

  test("multiple roots are sorted by name, case-insensitively", () => {
    const roots = buildFolderTree(["Zeta", "alpha", "Beta"]);
    // sensitivity: "base" -> case-insensitive ordering: alpha, Beta, Zeta.
    expect(roots.map((r) => r.name)).toEqual(["alpha", "Beta", "Zeta"]);
  });

  test("sibling ordering is numeric-aware (item2 before item10)", () => {
    const roots = buildFolderTree(["pack/item10", "pack/item2", "pack/item1"]);
    const pack = roots[0];
    expect(pack.children.map((c) => folderLabel(c.path))).toEqual(["item1", "item2", "item10"]);
  });

  test("sorting recurses into every branch, not just the roots", () => {
    const roots = buildFolderTree(["root/zeta/inner", "root/alpha/inner"]);
    const root = roots[0];
    expect(root.children.map((c) => c.name)).toEqual(["alpha", "zeta"]);
  });

  test("a duplicate path does not create a second node", () => {
    const roots = buildFolderTree(["dup", "dup"]);
    expect(roots).toHaveLength(1);
    expect(roots[0].path).toBe("dup");
  });

  test("two independent deep paths produce two separate roots, each fully synthesized", () => {
    const roots: FolderNode[] = buildFolderTree(["one/deep", "two/deep"]);
    expect(roots.map((r) => r.path)).toEqual(["one", "two"]);
    expect(roots[0].children.map((c) => c.path)).toEqual(["one/deep"]);
    expect(roots[1].children.map((c) => c.path)).toEqual(["two/deep"]);
  });

  test("a shared deep prefix across two leaf paths reuses the intermediate node", () => {
    const roots = buildFolderTree(["a/b/c", "a/b/d"]);
    expect(roots).toHaveLength(1);
    const ab = roots[0].children[0];
    expect(ab.path).toBe("a/b");
    expect(ab.children.map((c) => c.path)).toEqual(["a/b/c", "a/b/d"]);
  });
});
