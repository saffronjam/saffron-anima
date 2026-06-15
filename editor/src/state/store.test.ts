import { describe, expect, test } from "bun:test";
import type { EntityListEntry } from "../protocol";
import { buildTree, reanchorPastBones } from "./store";

/// A flat entity-list entry. `parentId`/`bone` are optional on the wire, matching the
/// `EntityListEntry` DTO; the helpers below default the noise away.
function entry(id: string, extra: Partial<EntityListEntry> = {}): EntityListEntry {
  return { id, name: id, ...extra };
}

/// The id of each TreeNode child, in order — the assertion shape for tree shape.
function childIds(node: { children: { entity: EntityListEntry }[] }): string[] {
  return node.children.map((c) => c.entity.id);
}

describe("buildTree", () => {
  test("a flat list with parentId links nests children under their parent", () => {
    const roots = buildTree([
      entry("root"),
      entry("a", { parentId: "root" }),
      entry("b", { parentId: "root" }),
      entry("a1", { parentId: "a" }),
    ]);
    expect(roots.map((r) => r.entity.id)).toEqual(["root"]);
    expect(childIds(roots[0]!)).toEqual(["a", "b"]);
    const aNode = roots[0]!.children.find((c) => c.entity.id === "a")!;
    expect(childIds(aNode)).toEqual(["a1"]);
  });

  test("a missing parentId lands the entry at the root", () => {
    const roots = buildTree([entry("solo")]);
    expect(roots.map((r) => r.entity.id)).toEqual(["solo"]);
    expect(roots[0]!.children).toEqual([]);
  });

  test('parentId "0" is treated as the root sentinel', () => {
    const roots = buildTree([entry("a", { parentId: "0" }), entry("b", { parentId: "0" })]);
    expect(roots.map((r) => r.entity.id)).toEqual(["a", "b"]);
  });

  test("a self-referencing parentId lands the entry at the root (no self-child loop)", () => {
    const roots = buildTree([entry("loop", { parentId: "loop" })]);
    expect(roots.map((r) => r.entity.id)).toEqual(["loop"]);
    expect(roots[0]!.children).toEqual([]);
  });

  test("a child whose parent id is absent from the list becomes a root", () => {
    const roots = buildTree([entry("orphan", { parentId: "ghost" })]);
    expect(roots.map((r) => r.entity.id)).toEqual(["orphan"]);
    expect(roots[0]!.children).toEqual([]);
  });

  test("sibling and root order preserves the engine's array order", () => {
    const roots = buildTree([
      entry("z"),
      entry("y"),
      entry("z2", { parentId: "z" }),
      entry("z1", { parentId: "z" }),
    ]);
    expect(roots.map((r) => r.entity.id)).toEqual(["z", "y"]);
    expect(childIds(roots[0]!)).toEqual(["z2", "z1"]);
  });

  test("an empty list yields an empty forest", () => {
    expect(buildTree([])).toEqual([]);
  });

  test("each node carries the original entity object", () => {
    const e = entry("a", { name: "Alpha" });
    const roots = buildTree([e]);
    expect(roots[0]!.entity).toBe(e);
  });
});

describe("reanchorPastBones", () => {
  test("drops bone rows from the result", () => {
    const out = reanchorPastBones([entry("mesh"), entry("bone", { bone: true, parentId: "mesh" })]);
    expect(out.map((e) => e.id)).toEqual(["mesh"]);
  });

  test("re-anchors a non-bone child past a bone parent to the non-bone grandparent", () => {
    const out = reanchorPastBones([
      entry("rig"),
      entry("joint", { bone: true, parentId: "rig" }),
      entry("weapon", { parentId: "joint" }),
    ]);
    const weapon = out.find((e) => e.id === "weapon")!;
    expect(weapon.parentId).toBe("rig");
  });

  test("walks a multi-bone chain up to the nearest non-bone ancestor", () => {
    const out = reanchorPastBones([
      entry("rig"),
      entry("spine", { bone: true, parentId: "rig" }),
      entry("hand", { bone: true, parentId: "spine" }),
      entry("prop", { parentId: "hand" }),
    ]);
    const prop = out.find((e) => e.id === "prop")!;
    expect(prop.parentId).toBe("rig");
  });

  test("a child whose parentId is unchanged returns the IDENTICAL object", () => {
    const child = entry("child", { parentId: "parent" });
    const out = reanchorPastBones([entry("parent"), child]);
    const result = out.find((e) => e.id === "child")!;
    expect(Object.is(result, child)).toBe(true);
  });

  test("a root entity (no parent) is returned identical", () => {
    const root = entry("root");
    const out = reanchorPastBones([root]);
    expect(Object.is(out[0], root)).toBe(true);
  });

  test("a bone-only ancestor chain ending at root yields parentId undefined", () => {
    const out = reanchorPastBones([
      entry("boneRoot", { bone: true }),
      entry("boneMid", { bone: true, parentId: "boneRoot" }),
      entry("leaf", { parentId: "boneMid" }),
    ]);
    const leaf = out.find((e) => e.id === "leaf")!;
    expect(leaf.parentId).toBeUndefined();
    // The bone ancestors are filtered out; only the re-anchored leaf survives.
    expect(out.map((e) => e.id)).toEqual(["leaf"]);
  });

  test("a parent cycle through bones terminates without hanging", () => {
    // x and y are bones pointing at each other; z is a non-bone child of x. The bounded
    // walk (steps <= entities.length) must terminate rather than loop forever. The walk
    // gives up on the step bound while `parent` is still a bone on the cycle (it never
    // reaches a non-bone or a parentless root), so the resolved parent stays on the cycle
    // — the only contract that matters here is termination, not the landing id.
    const out = reanchorPastBones([
      entry("x", { bone: true, parentId: "y" }),
      entry("y", { bone: true, parentId: "x" }),
      entry("z", { parentId: "x" }),
    ]);
    // Bones are filtered out; only the non-bone z survives, and the call returned.
    expect(out.map((e) => e.id)).toEqual(["z"]);
  });

  test("a non-bone parent absent from the map re-anchors to undefined", () => {
    // parentId points at an entity not in the list: byId.get returns undefined, the loop
    // never runs (undefined?.bone is falsy), parentId becomes undefined — a change, so a
    // fresh object is returned.
    const child = entry("child", { parentId: "ghost" });
    const out = reanchorPastBones([child]);
    const result = out[0]!;
    expect(result.parentId).toBeUndefined();
    expect(Object.is(result, child)).toBe(false);
  });

  test("leaves the surviving entities in their original order", () => {
    const out = reanchorPastBones([
      entry("a"),
      entry("bone", { bone: true }),
      entry("b"),
      entry("c", { parentId: "a" }),
    ]);
    expect(out.map((e) => e.id)).toEqual(["a", "b", "c"]);
  });
});
