import { describe, expect, test } from "bun:test";
import {
  type DockBranch,
  type DockLayout,
  type DockLeaf,
  type DockPanelId,
  DEFAULT_LEAF,
  defaultAssetEditorLayout,
  defaultSceneLayout,
  findPanelLeaf,
  hasRequiredPanels,
  insertPanel,
  isLeaf,
  isNodeRendered,
  knownPanelIds,
  leafTabs,
  movePanel,
  normalize,
  openPanelResolve,
  pruneLastLocation,
  removePanel,
  renderedChildIds,
  reorderTab,
  splitLeaf,
  subtreeMinPx,
  validate,
} from "./dockLayout";

function leaf(id: string, tabs: DockPanelId[], extra: Partial<DockLeaf> = {}): DockLeaf {
  return { type: "leaf", id, tabs, activeTab: tabs[0] ?? null, ...extra };
}

function branch(
  id: string,
  orientation: "horizontal" | "vertical",
  children: string[],
): DockBranch {
  const each = 100 / children.length;
  return {
    type: "branch",
    id,
    orientation,
    children,
    sizes: Object.fromEntries(children.map((c) => [c, each])),
  };
}

function layoutOf(rootId: string, ...nodes: (DockLeaf | DockBranch)[]): DockLayout {
  return { version: 1, rootId, nodes: Object.fromEntries(nodes.map((n) => [n.id, n])) };
}

describe("default factories", () => {
  test("defaultSceneLayout has three persistent leaves with the trio in leftBottom", () => {
    const l = defaultSceneLayout();
    const lb = l.nodes["leaf:leftBottom"];
    expect(isLeaf(lb) && lb.tabs).toEqual(["inspector", "environment", "render"]);
    expect(isLeaf(lb) && lb.activeTab).toBe("inspector");
    for (const id of ["leaf:leftBottom", "leaf:right", "leaf:bottom"]) {
      const node = l.nodes[id];
      expect(isLeaf(node) && node.persistent).toBe(true);
    }
  });

  test("defaultSceneLayout survives normalize unchanged (persistent empties kept)", () => {
    const l = defaultSceneLayout();
    const n = normalize(l);
    expect(Object.keys(n.nodes).sort()).toEqual(Object.keys(l.nodes).sort());
    expect(leafTabs(n, "leaf:right")).toEqual([]);
    expect(leafTabs(n, "leaf:bottom")).toEqual([]);
  });

  test("defaultAssetEditorLayout: locked preview, vertical root over horizontal row", () => {
    const l = defaultAssetEditorLayout();
    const root = l.nodes[l.rootId];
    expect(root.type === "branch" && root.orientation).toBe("vertical");
    const preview = l.nodes["leaf:preview"];
    expect(isLeaf(preview) && preview.locked).toBe(true);
    const row = l.nodes["branch:ae-row"];
    expect(row.type === "branch" && row.orientation).toBe("horizontal");
  });

  test("defaultAssetEditorLayout survives normalize (locked preview + persistent empties kept)", () => {
    const n = normalize(defaultAssetEditorLayout());
    expect(n.nodes["leaf:preview"]).toBeDefined();
    // skeleton/clips/assetTimeline start empty + persistent (capability gating fills them).
    expect(leafTabs(n, "leaf:skeleton")).toEqual([]);
    expect(n.nodes["leaf:skeleton"]).toBeDefined();
    expect(n.nodes["leaf:assetTimeline"]).toBeDefined();
  });

  test("a static asset (no rig/clips) renders only the locked preview", () => {
    const l = defaultAssetEditorLayout();
    expect(isNodeRendered(l, "leaf:preview")).toBe(true); // locked
    expect(isNodeRendered(l, "leaf:skeleton")).toBe(false); // empty
    expect(isNodeRendered(l, "leaf:clips")).toBe(false);
    expect(isNodeRendered(l, "leaf:assetTimeline")).toBe(false);
    // Opening a panel (rigged model) makes its leaf render.
    const withRig = insertPanel(l, "skeleton", "leaf:skeleton", 0);
    expect(isNodeRendered(withRig, "leaf:skeleton")).toBe(true);
  });
});

describe("insertPanel / removePanel", () => {
  test("insertPanel adds at index and activates", () => {
    const l = insertPanel(defaultSceneLayout(), "stats", "leaf:right", 0);
    expect(leafTabs(l, "leaf:right")).toEqual(["stats"]);
    const right = l.nodes["leaf:right"];
    expect(isLeaf(right) && right.activeTab).toBe("stats");
  });

  test("removePanel falls the active tab back to the index-1 neighbor", () => {
    let l = defaultSceneLayout();
    // active is inspector; make render active, then remove it -> falls to environment.
    l = {
      ...l,
      nodes: {
        ...l.nodes,
        "leaf:leftBottom": leaf("leaf:leftBottom", ["inspector", "environment", "render"], {
          activeTab: "render",
          persistent: true,
        }),
      },
    };
    l = removePanel(l, "render");
    const lb = l.nodes["leaf:leftBottom"];
    expect(isLeaf(lb) && lb.tabs).toEqual(["inspector", "environment"]);
    expect(isLeaf(lb) && lb.activeTab).toBe("environment");
  });

  test("removePanel of the first active tab falls to the new first", () => {
    const l = removePanel(defaultSceneLayout(), "inspector");
    const lb = l.nodes["leaf:leftBottom"];
    expect(isLeaf(lb) && lb.activeTab).toBe("environment");
  });

  test("removePanel of a non-active tab leaves the active tab intact", () => {
    let l = defaultSceneLayout();
    l = {
      ...l,
      nodes: {
        ...l.nodes,
        "leaf:leftBottom": leaf("leaf:leftBottom", ["inspector", "environment", "render"], {
          activeTab: "render",
          persistent: true,
        }),
      },
    };
    l = removePanel(l, "environment");
    const lb = l.nodes["leaf:leftBottom"];
    expect(isLeaf(lb) && lb.activeTab).toBe("render");
  });
});

describe("reorderTab (without-moving-tab index space)", () => {
  test("moves a tab to a later index", () => {
    const l = reorderTab(defaultSceneLayout(), "leaf:leftBottom", "inspector", 2);
    expect(leafTabs(l, "leaf:leftBottom")).toEqual(["environment", "render", "inspector"]);
  });

  test("moves a tab to the front", () => {
    const l = reorderTab(defaultSceneLayout(), "leaf:leftBottom", "render", 0);
    expect(leafTabs(l, "leaf:leftBottom")).toEqual(["render", "inspector", "environment"]);
  });
});

describe("splitLeaf", () => {
  test("same-orientation parent: inserts an adjacent sibling, halving the slot", () => {
    const l = layoutOf(
      "b",
      branch("b", "horizontal", ["a", "c"]),
      leaf("a", ["material"]),
      leaf("c", ["timeline"]),
    );
    const next = splitLeaf(l, "a", "stats", "right", { leafId: "n" });
    const b = next.nodes["b"] as DockBranch;
    expect(b.children).toEqual(["a", "n", "c"]);
    expect(b.sizes["a"]).toBeCloseTo(25);
    expect(b.sizes["n"]).toBeCloseTo(25);
    expect(b.sizes["c"]).toBeCloseTo(50);
    expect(leafTabs(next, "n")).toEqual(["stats"]);
  });

  test("different-orientation: wraps the target in a new branch", () => {
    const l = layoutOf(
      "b",
      branch("b", "horizontal", ["a", "c"]),
      leaf("a", ["material"]),
      leaf("c", ["timeline"]),
    );
    const next = splitLeaf(l, "a", "stats", "bottom", { leafId: "n", branchId: "w" });
    const wrap = next.nodes["w"] as DockBranch;
    expect(wrap.orientation).toBe("vertical");
    expect(wrap.children).toEqual(["a", "n"]);
    const b = next.nodes["b"] as DockBranch;
    expect(b.children).toEqual(["w", "c"]);
  });

  test("root append: a fresh leaf becomes the root branch's trailing child", () => {
    const l = layoutOf(
      "b",
      branch("b", "horizontal", ["a"]),
      leaf("a", ["viewport"], { locked: true }),
    );
    const next = splitLeaf(l, "b", "stats", "right", { leafId: "n" });
    const b = next.nodes["b"] as DockBranch;
    expect(b.children).toEqual(["a", "n"]);
  });
});

describe("movePanel", () => {
  test("tab-merge across leaves collapses the emptied source", () => {
    const l = layoutOf(
      "b",
      branch("b", "horizontal", ["a", "c"]),
      leaf("a", ["material"]),
      leaf("c", ["timeline"]),
    );
    const next = movePanel(l, "material", { kind: "tab", leafId: "c", index: 1 });
    // source leaf "a" empties -> deleted -> single-child branch collapses -> root is "c".
    expect(isLeaf(next.nodes[next.rootId])).toBe(true);
    expect(leafTabs(next, next.rootId)).toEqual(["timeline", "material"]);
    const root = next.nodes[next.rootId];
    expect(isLeaf(root) && root.activeTab).toBe("material");
  });

  test("split places the panel beside the target leaf", () => {
    const l = layoutOf(
      "b",
      branch("b", "horizontal", ["a", "c"]),
      leaf("a", ["material", "stats"]),
      leaf("c", ["timeline"]),
    );
    const next = movePanel(l, "stats", { kind: "split", leafId: "c", edge: "bottom" });
    expect(findPanelLeaf(next, "stats")).not.toBeNull();
    expect(leafTabs(l, "a")).toEqual(["material", "stats"]); // input untouched (pure)
  });

  test("split then move the panel back out collapses to the original two-leaf shape", () => {
    const start = layoutOf(
      "b",
      branch("b", "horizontal", ["a", "c"]),
      leaf("a", ["material", "stats"]),
      leaf("c", ["timeline"]),
    );
    // Split `stats` beside the timeline leaf, then merge it back into leaf `a`.
    const split = movePanel(start, "stats", { kind: "split", leafId: "c", edge: "right" });
    const splitLeafId = findPanelLeaf(split, "stats");
    expect(splitLeafId).not.toBeNull();
    const back = movePanel(split, "stats", { kind: "tab", leafId: "a", index: 1 });
    // The transient split leaf collapsed; we are back to two leaves under one horizontal branch.
    const branches = Object.values(back.nodes).filter((n) => n.type === "branch");
    expect(branches).toHaveLength(1);
    expect(leafTabs(back, "a").sort()).toEqual(["material", "stats"]);
    expect(leafTabs(back, "c")).toEqual(["timeline"]);
  });
});

describe("normalize", () => {
  test("deletes an empty non-persistent leaf and collapses the single-child branch", () => {
    const l = layoutOf(
      "b",
      branch("b", "horizontal", ["a", "c"]),
      leaf("a", []),
      leaf("c", ["timeline"]),
    );
    const n = normalize(l);
    expect(isLeaf(n.nodes[n.rootId])).toBe(true);
    expect(n.rootId).toBe("c");
  });

  test("keeps an empty persistent leaf", () => {
    const l = layoutOf(
      "b",
      branch("b", "horizontal", ["a", "c"]),
      leaf("a", [], { persistent: true }),
      leaf("c", ["timeline"]),
    );
    const n = normalize(l);
    expect(n.nodes["a"]).toBeDefined();
  });

  test("keeps an empty locked leaf", () => {
    const l = layoutOf(
      "b",
      branch("b", "horizontal", ["a", "c"]),
      leaf("a", [], { locked: true }),
      leaf("c", ["timeline"]),
    );
    const n = normalize(l);
    expect(n.nodes["a"]).toBeDefined();
  });

  test("merges a same-orientation child branch into its parent", () => {
    const l = layoutOf(
      "p",
      branch("p", "horizontal", ["x", "q"]),
      leaf("x", ["material"]),
      branch("q", "horizontal", ["y", "z"]),
      leaf("y", ["timeline"]),
      leaf("z", ["stats"]),
    );
    const n = normalize(l);
    const p = n.nodes["p"] as DockBranch;
    expect(p.children).toEqual(["x", "y", "z"]);
    expect(n.nodes["q"]).toBeUndefined();
  });

  test("does not merge a different-orientation child branch", () => {
    const l = layoutOf(
      "p",
      branch("p", "horizontal", ["x", "q"]),
      leaf("x", ["material"]),
      branch("q", "vertical", ["y", "z"]),
      leaf("y", ["timeline"]),
      leaf("z", ["stats"]),
    );
    const n = normalize(l);
    expect(n.nodes["q"]).toBeDefined();
  });
});

describe("validate", () => {
  const scene = knownPanelIds("scene");

  test("drops unknown panel ids", () => {
    const l = layoutOf("a", leaf("a", ["inspector", "bogus" as DockPanelId]));
    const v = validate(l, scene);
    expect(v).not.toBeNull();
    expect(leafTabs(v as DockLayout, "a")).toEqual(["inspector"]);
  });

  test("returns null when the root is missing", () => {
    const l: DockLayout = { version: 1, rootId: "ghost", nodes: {} };
    expect(validate(l, scene)).toBeNull();
  });

  test("returns null when a branch child is dangling", () => {
    const l = layoutOf("b", branch("b", "horizontal", ["a", "missing"]), leaf("a", ["inspector"]));
    expect(validate(l, scene)).toBeNull();
  });
});

describe("pruneLastLocation", () => {
  test("drops entries whose leaf no longer exists", () => {
    const l = defaultSceneLayout();
    const pruned = pruneLastLocation(l, { material: "leaf:right", stats: "leaf:ghost" });
    expect(pruned).toEqual({ material: "leaf:right" });
  });
});

describe("Scene tree render helpers", () => {
  test("the default Scene tree has a locked viewport and hierarchy/assets leaves", () => {
    const l = defaultSceneLayout();
    const viewport = l.nodes["leaf:viewport"];
    expect(isLeaf(viewport) && viewport.locked).toBe(true);
    expect(leafTabs(l, "leaf:hierarchy")).toEqual(["hierarchy"]);
    expect(leafTabs(l, "leaf:assets")).toEqual(["assets"]);
    const root = l.nodes[l.rootId];
    expect(root.type === "branch" && root.orientation).toBe("horizontal");
  });

  test("isNodeRendered: empty non-locked leaf collapses, locked/non-empty render", () => {
    const l = defaultSceneLayout();
    expect(isNodeRendered(l, "leaf:right")).toBe(false); // empty, non-locked
    expect(isNodeRendered(l, "leaf:bottom")).toBe(false);
    expect(isNodeRendered(l, "leaf:viewport")).toBe(true); // locked
    expect(isNodeRendered(l, "leaf:leftBottom")).toBe(true); // non-empty
  });

  test("renderedChildIds drops empty leaves from the root", () => {
    const l = defaultSceneLayout();
    const root = l.nodes[l.rootId] as DockBranch;
    expect(renderedChildIds(l, root)).toEqual(["branch:scene-left", "branch:scene-center"]);
  });

  test("subtreeMinPx propagates the viewport's 520px width up through its column", () => {
    const l = defaultSceneLayout();
    // center column is vertical → width is the MAX of its rendered children's widths (520).
    expect(subtreeMinPx(l, "branch:scene-center", "width")).toBe(520);
    // root is horizontal → width is the SUM of rendered children's widths (left 200 + 520).
    expect(subtreeMinPx(l, l.rootId, "width")).toBeGreaterThanOrEqual(520 + 200);
  });

  test("hasRequiredPanels rejects a tree missing structural panels (stale persisted layout)", () => {
    expect(hasRequiredPanels(defaultSceneLayout(), "scene")).toBe(true);
    expect(hasRequiredPanels(defaultAssetEditorLayout(), "assetEditor")).toBe(true);
    // A tree from an older build — only the leftBottom trio, no hierarchy/viewport/assets —
    // is the bug that rendered Inspector/Environment/Render full-page.
    const stale = layoutOf(
      "r",
      branch("r", "horizontal", ["leaf:leftBottom", "leaf:right"]),
      leaf("leaf:leftBottom", ["inspector", "environment", "render"]),
      leaf("leaf:right", []),
    );
    expect(hasRequiredPanels(stale, "scene")).toBe(false);
  });
});

describe("openPanelResolve", () => {
  test("an already-open panel is activated in place", () => {
    const l = defaultSceneLayout();
    const r = openPanelResolve(l, "environment", { defaultLeafId: DEFAULT_LEAF.environment });
    expect(r.leafId).toBe("leaf:leftBottom");
    const lb = r.layout.nodes["leaf:leftBottom"];
    expect(isLeaf(lb) && lb.activeTab).toBe("environment");
  });

  test("falls to lastLocation when present and usable", () => {
    const l = defaultSceneLayout();
    const r = openPanelResolve(l, "stats", {
      defaultLeafId: "leaf:right",
      lastLeafId: "leaf:bottom",
    });
    expect(r.leafId).toBe("leaf:bottom");
    expect(leafTabs(r.layout, "leaf:bottom")).toEqual(["stats"]);
  });

  test("falls to the default leaf when no lastLocation", () => {
    const l = defaultSceneLayout();
    const r = openPanelResolve(l, "stats", { defaultLeafId: "leaf:right" });
    expect(r.leafId).toBe("leaf:right");
    expect(leafTabs(r.layout, "leaf:right")).toEqual(["stats"]);
  });

  test("terminal fallback appends a fresh leaf when only locked leaves exist", () => {
    const l = layoutOf("v", leaf("v", ["viewport"], { locked: true }));
    const r = openPanelResolve(l, "stats", { defaultLeafId: "leaf:none" });
    expect(findPanelLeaf(r.layout, "stats")).toBe(r.leafId);
    expect(r.leafId).not.toBe("v");
    const root = r.layout.nodes[r.layout.rootId];
    expect(root.type).toBe("branch");
  });
});
