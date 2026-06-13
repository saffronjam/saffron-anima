import { describe, expect, test } from "bun:test";
import type { EntityListEntry } from "../protocol";
import { isInSubtree, subtreeIds } from "./HierarchyTree";

/// Build an entity entry; omit `parentId` for a root (the store leaves it unset
/// rather than emitting the "0" sentinel for top-level entities).
function ent(id: string, parentId?: string, name = id): EntityListEntry {
  return parentId === undefined ? { id, name } : { id, name, parentId };
}

// A small cycle-free forest:
//
//   root        other      orphan (parentId "0" sentinel)
//   ├─ a          └─ p
//   │  ├─ a1
//   │  └─ a2
//   │     └─ a2x
//   └─ b
const forest: EntityListEntry[] = [
  ent("root"),
  ent("a", "root"),
  ent("a1", "a"),
  ent("a2", "a"),
  ent("a2x", "a2"),
  ent("b", "root"),
  ent("other"),
  ent("p", "other"),
  ent("orphan", "0"),
];

describe("isInSubtree", () => {
  test("a node is in its own subtree", () => {
    expect(isInSubtree(forest, "root", "root")).toBe(true);
    expect(isInSubtree(forest, "a2", "a2")).toBe(true);
  });

  test("a direct child is in the parent's subtree", () => {
    expect(isInSubtree(forest, "root", "a")).toBe(true);
    expect(isInSubtree(forest, "a", "a1")).toBe(true);
  });

  test("a deep descendant is included", () => {
    // a2x → a2 → a → root, so a2x sits in root's subtree.
    expect(isInSubtree(forest, "root", "a2x")).toBe(true);
    expect(isInSubtree(forest, "a", "a2x")).toBe(true);
  });

  test("an unrelated node in another tree is not in the subtree", () => {
    expect(isInSubtree(forest, "root", "other")).toBe(false);
    expect(isInSubtree(forest, "root", "p")).toBe(false);
    expect(isInSubtree(forest, "a", "b")).toBe(false);
  });

  test("an ancestor is not in its descendant's subtree (walk is upward only)", () => {
    // root is an ancestor of a, so root is NOT inside a's subtree.
    expect(isInSubtree(forest, "a", "root")).toBe(false);
    expect(isInSubtree(forest, "a2", "a")).toBe(false);
  });

  test("a sibling is not in the subtree", () => {
    expect(isInSubtree(forest, "a1", "a2")).toBe(false);
    expect(isInSubtree(forest, "a", "b")).toBe(false);
  });

  test("the '0' root sentinel terminates the walk", () => {
    // orphan's parent is the "0" sentinel; the walk stops at "0" before hitting it,
    // so orphan is in no real entity's subtree but is in its own.
    expect(isInSubtree(forest, "orphan", "orphan")).toBe(true);
    expect(isInSubtree(forest, "0", "orphan")).toBe(false);
    expect(isInSubtree(forest, "root", "orphan")).toBe(false);
  });

  test("a candidate with no parent (root entity) only matches itself", () => {
    expect(isInSubtree(forest, "other", "root")).toBe(false);
    expect(isInSubtree(forest, "root", "root")).toBe(true);
  });

  test("a candidate id absent from the map only matches itself", () => {
    // "ghost" has no entry, so parentOf.get returns undefined and the walk ends.
    expect(isInSubtree(forest, "ghost", "ghost")).toBe(true);
    expect(isInSubtree(forest, "root", "ghost")).toBe(false);
  });

  test("an empty forest: a node is still in its own subtree, nothing else", () => {
    expect(isInSubtree([], "x", "x")).toBe(true);
    expect(isInSubtree([], "x", "y")).toBe(false);
  });

  test("a self-parent cycle is bounded and does not loop forever", () => {
    // Corrupt data: loop points at itself. The step bound stops the walk; the
    // node still matches its own root, and an unrelated root does not.
    const cyclic: EntityListEntry[] = [ent("c", "c"), ent("d", "c")];
    expect(isInSubtree(cyclic, "c", "c")).toBe(true);
    expect(isInSubtree(cyclic, "c", "d")).toBe(true);
    expect(isInSubtree(cyclic, "z", "d")).toBe(false);
  });

  test("a two-node mutual cycle is bounded", () => {
    // e ↔ f point at each other; the steps <= entities.length bound terminates.
    const cyclic: EntityListEntry[] = [ent("e", "f"), ent("f", "e")];
    expect(isInSubtree(cyclic, "e", "e")).toBe(true);
    expect(isInSubtree(cyclic, "f", "e")).toBe(true);
    expect(isInSubtree(cyclic, "ghost", "e")).toBe(false);
  });
});

describe("subtreeIds", () => {
  test("a leaf's subtree is just itself", () => {
    expect(subtreeIds(forest, "a2x")).toEqual(new Set(["a2x"]));
    expect(subtreeIds(forest, "b")).toEqual(new Set(["b"]));
  });

  test("a node with children enumerates self + every descendant", () => {
    expect(subtreeIds(forest, "a")).toEqual(new Set(["a", "a1", "a2", "a2x"]));
  });

  test("the top of a cycle-free forest enumerates its whole tree only", () => {
    expect(subtreeIds(forest, "root")).toEqual(
      new Set(["root", "a", "a1", "a2", "a2x", "b"]),
    );
    // The disjoint tree is untouched.
    expect(subtreeIds(forest, "other")).toEqual(new Set(["other", "p"]));
  });

  test("subtrees of disjoint roots do not overlap", () => {
    const fromRoot = subtreeIds(forest, "root");
    const fromOther = subtreeIds(forest, "other");
    for (const id of fromOther) {
      expect(fromRoot.has(id)).toBe(false);
    }
  });

  test("an intermediate node enumerates only its own branch", () => {
    // a2 owns only a2x; siblings (a1) and ancestors (a, root) are excluded.
    expect(subtreeIds(forest, "a2")).toEqual(new Set(["a2", "a2x"]));
  });

  test("a root id absent from the forest yields a singleton of itself", () => {
    expect(subtreeIds(forest, "ghost")).toEqual(new Set(["ghost"]));
  });

  test("an empty forest yields a singleton of the requested root", () => {
    expect(subtreeIds([], "x")).toEqual(new Set(["x"]));
  });

  test("entities with no parent are grouped under the '0' sentinel, not under each other", () => {
    // root and other both lack parentId, so neither appears in the other's subtree.
    expect(subtreeIds(forest, "root").has("other")).toBe(false);
    // Asking for the "0" bucket collects every top-level entity (no-parent roots plus
    // the explicit "0"-parented orphan) and walks each of their full trees.
    expect(subtreeIds(forest, "0")).toEqual(
      new Set(["0", "root", "a", "a1", "a2", "a2x", "b", "other", "p", "orphan"]),
    );
  });

  test("a self-parent cycle terminates via the visited set", () => {
    // c points at itself as parent; the !ids.has(child) guard prevents re-push.
    const cyclic: EntityListEntry[] = [ent("c", "c"), ent("d", "c")];
    expect(subtreeIds(cyclic, "c")).toEqual(new Set(["c", "d"]));
  });

  test("a two-node mutual cycle terminates and collects both", () => {
    const cyclic: EntityListEntry[] = [ent("e", "f"), ent("f", "e")];
    expect(subtreeIds(cyclic, "e")).toEqual(new Set(["e", "f"]));
  });

  test("isInSubtree and subtreeIds agree across the forest", () => {
    // Cross-check: a node is in root's subtree iff subtreeIds(root) contains it.
    const inRoot = subtreeIds(forest, "root");
    for (const e of forest) {
      expect(isInSubtree(forest, "root", e.id)).toBe(inRoot.has(e.id));
    }
  });
});
