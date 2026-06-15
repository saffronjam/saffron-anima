import { describe, expect, test } from "bun:test";
import type { Edge } from "@xyflow/react";
import { type FlowNode, type MaterialGraph, flowToGraph, graphToFlow } from "./graph";

describe("graphToFlow / flowToGraph round-trip", () => {
  // A small graph: a constant node feeding materialOutput.baseColor. Both nodes carry an
  // already-rounded editorPos so positions survive the round-trip exactly.
  const smallGraph: MaterialGraph = {
    nodes: [
      { id: "c1", type: "constant", props: { value: [1, 0, 0, 1], editorPos: [60, 40] } },
      { id: "out", type: "materialOutput", props: { editorPos: [300, 200] } },
    ],
    edges: [{ from: ["c1", "rgba"], to: ["out", "baseColor"] }],
  };

  test("flowToGraph(graphToFlow(g)) equals g modulo editorPos rounding", () => {
    const { nodes, edges } = graphToFlow(smallGraph);
    const back = flowToGraph(nodes, edges);
    expect(back).toEqual(smallGraph);
  });

  test("rounds fractional positions back into editorPos on the way out", () => {
    const { nodes, edges } = graphToFlow(smallGraph);
    // Nudge a node to a fractional canvas position; flowToGraph should round it.
    const nudged: FlowNode[] = nodes.map((n) =>
      n.id === "c1" ? { ...n, position: { x: 60.6, y: 39.2 } } : n,
    );
    const back = flowToGraph(nudged, edges);
    const c1 = back.nodes.find((n) => n.id === "c1");
    expect(c1?.props?.editorPos).toEqual([61, 39]);
  });

  test("editorPos appears only in node.props, never in data.props on the FlowNode", () => {
    const { nodes } = graphToFlow(smallGraph);
    for (const n of nodes) {
      expect("editorPos" in n.data.props).toBe(false);
    }
    // The constant node keeps its non-position prop.
    const c1 = nodes.find((n) => n.id === "c1");
    expect(c1?.data.props.value).toEqual([1, 0, 0, 1]);
    // And the stored position is reflected on the FlowNode position, not in data.props.
    expect(c1?.position).toEqual({ x: 60, y: 40 });
  });

  test("flowToGraph re-folds the canvas position into props.editorPos", () => {
    const { nodes, edges } = graphToFlow(smallGraph);
    const back = flowToGraph(nodes, edges);
    const c1 = back.nodes.find((n) => n.id === "c1");
    expect(c1?.props?.editorPos).toEqual([60, 40]);
    const out = back.nodes.find((n) => n.id === "out");
    expect(out?.props?.editorPos).toEqual([300, 200]);
  });
});

describe("unknown node types", () => {
  const unknownGraph: MaterialGraph = {
    nodes: [{ id: "x1", type: "mysteryNode", props: { editorPos: [10, 20] } }],
    edges: [],
  };

  test("an unknown node type yields a fallback spec (inputs/outputs)", () => {
    const { nodes } = graphToFlow(unknownGraph);
    const spec = nodes[0].data.spec;
    expect(spec.type).toBe("mysteryNode");
    expect(spec.label).toBe("mysteryNode");
    expect(spec.category).toBe("math");
    expect(spec.inputs).toEqual(["a"]);
    expect(spec.outputs).toEqual(["rgba"]);
  });

  test("the unknown type survives the round-trip keeping its type string", () => {
    const { nodes, edges } = graphToFlow(unknownGraph);
    const back = flowToGraph(nodes, edges);
    expect(back.nodes[0].type).toBe("mysteryNode");
    expect(back).toEqual(unknownGraph);
  });
});

describe("edge handling", () => {
  test("an edge with a null sourceHandle is dropped by flowToGraph", () => {
    const node: FlowNode = {
      id: "a",
      type: "saffron",
      position: { x: 0, y: 0 },
      data: {
        spec: {
          type: "constant",
          label: "Constant",
          category: "input",
          inputs: [],
          outputs: ["rgba"],
        },
        props: {},
      },
    };
    const edges: Edge[] = [
      // dropped: no sourceHandle
      { id: "e0", source: "a", sourceHandle: null, target: "b", targetHandle: "baseColor" },
      // kept: both handles present
      { id: "e1", source: "a", sourceHandle: "rgba", target: "b", targetHandle: "baseColor" },
    ];
    const back = flowToGraph([node], edges);
    expect(back.edges).toEqual([{ from: ["a", "rgba"], to: ["b", "baseColor"] }]);
  });

  test("an edge with a null targetHandle is dropped too", () => {
    const edges: Edge[] = [
      { id: "e0", source: "a", sourceHandle: "rgba", target: "b", targetHandle: null },
    ];
    const back = flowToGraph([], edges);
    expect(back.edges).toEqual([]);
  });

  test("graphToFlow builds a deterministic edge id and copies the handles", () => {
    const g: MaterialGraph = {
      nodes: [],
      edges: [{ from: ["c1", "rgba"], to: ["out", "baseColor"] }],
    };
    const { edges } = graphToFlow(g);
    expect(edges).toHaveLength(1);
    expect(edges[0].id).toBe("e0-c1.rgba-out.baseColor");
    expect(edges[0].source).toBe("c1");
    expect(edges[0].sourceHandle).toBe("rgba");
    expect(edges[0].target).toBe("out");
    expect(edges[0].targetHandle).toBe("baseColor");
  });
});

describe("posOf auto-layout", () => {
  test("auto-layouts when editorPos is absent", () => {
    const g: MaterialGraph = {
      nodes: [
        { id: "n0", type: "constant" },
        { id: "n1", type: "add" },
        { id: "n2", type: "multiply" },
        { id: "n3", type: "uv" },
      ],
      edges: [],
    };
    const { nodes } = graphToFlow(g);
    // x = 60 + (i % 3) * 240, y = 40 + floor(i / 3) * 160
    expect(nodes[0].position).toEqual({ x: 60, y: 40 });
    expect(nodes[1].position).toEqual({ x: 300, y: 40 });
    expect(nodes[2].position).toEqual({ x: 540, y: 40 });
    expect(nodes[3].position).toEqual({ x: 60, y: 200 });
  });

  test("auto-layouts when editorPos is malformed (non-numeric / wrong shape)", () => {
    const g: MaterialGraph = {
      nodes: [
        { id: "bad-string", type: "constant", props: { editorPos: "nope" } },
        { id: "bad-elem", type: "add", props: { editorPos: ["x", 5] } },
      ],
      edges: [],
    };
    const { nodes } = graphToFlow(g);
    // Both fall back to the staggered-column layout at their index.
    expect(nodes[0].position).toEqual({ x: 60, y: 40 });
    expect(nodes[1].position).toEqual({ x: 300, y: 40 });
  });

  test("uses the stored editorPos when present and well-formed", () => {
    const g: MaterialGraph = {
      nodes: [{ id: "n0", type: "constant", props: { editorPos: [123, 456] } }],
      edges: [],
    };
    const { nodes } = graphToFlow(g);
    expect(nodes[0].position).toEqual({ x: 123, y: 456 });
  });
});

describe("empty / undefined graph", () => {
  test("an undefined graph yields empty nodes and edges", () => {
    const { nodes, edges } = graphToFlow(undefined);
    expect(nodes).toEqual([]);
    expect(edges).toEqual([]);
  });
});
