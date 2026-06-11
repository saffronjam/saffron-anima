/// The node-graph material editor: a full-screen React Flow canvas over the live preview. Loads a
/// material's stored graph (material-get), lets you add/connect/edit nodes from the palette, and
/// auto-applies changes (debounced) via material-set-graph — re-rendering the studio-lit preview
/// sphere so the surface morphs as you edit. "Compile" forces codegen (material-compile-graph) for
/// procedural graphs that don't fold to params. Node types mirror the engine emitter (materials/graph.ts).
import { createContext, useCallback, useContext, useEffect, useMemo, useRef, useState } from "react";
import {
  addEdge,
  Background,
  type Connection,
  Controls,
  type Edge,
  Handle,
  type NodeProps,
  type NodeTypes,
  Position,
  ReactFlow,
  ReactFlowProvider,
  useEdgesState,
  useNodesState,
} from "@xyflow/react";
import "@xyflow/react/dist/style.css";
import { client } from "../control/client";
import { errorText, notifyError } from "../lib/flash";
import {
  type FlowNode,
  flowToGraph,
  freshNodeId,
  graphToFlow,
  NODE_SPECS,
  type SaffronNodeData,
} from "../materials/graph";
import { Button } from "@/components/ui/button";

interface NodeCallbacks {
  updateProps: (id: string, props: Record<string, unknown>) => void;
}
const NodeCallbacksContext = createContext<NodeCallbacks>({ updateProps: () => {} });

function handleTop(index: number, count: number): string {
  return `${((index + 1) / (count + 1)) * 100}%`;
}

/// One graph node card: a header, a left handle per input pin, a right handle per output pin, and a
/// minimal inline editor for the leaf nodes that carry data (constant value, texture slot).
function SaffronNode({ id, data }: NodeProps<FlowNode>) {
  const { spec, props } = data as SaffronNodeData;
  const { updateProps } = useContext(NodeCallbacksContext);
  const rows = Math.max(spec.inputs.length, 1);

  return (
    <div className="min-w-[140px] rounded border border-neutral-600 bg-neutral-800 text-[11px] text-neutral-200 shadow">
      <div className="rounded-t border-b border-neutral-600 bg-neutral-700 px-2 py-1 font-medium">
        {spec.label}
      </div>
      <div className="relative px-2 py-2" style={{ minHeight: `${rows * 18}px` }}>
        {spec.inputs.map((pin, i) => (
          <div key={pin} className="flex h-[18px] items-center">
            <Handle
              type="target"
              position={Position.Left}
              id={pin}
              style={{ top: handleTop(i, spec.inputs.length) }}
              className="!h-2 !w-2 !border-neutral-400 !bg-sky-500"
            />
            <span className="text-neutral-400">{pin}</span>
          </div>
        ))}
        {spec.outputs.map((pin, i) => (
          <Handle
            key={pin}
            type="source"
            position={Position.Right}
            id={pin}
            style={{ top: handleTop(i, spec.outputs.length) }}
            className="!h-2 !w-2 !border-neutral-400 !bg-emerald-500"
          />
        ))}

        {spec.type === "constant" ? (
          <div className="mt-1 grid grid-cols-4 gap-1">
            {[0, 1, 2, 3].map((c) => {
              const value = (props.value as number[] | undefined) ?? [1, 1, 1, 1];
              return (
                <input
                  key={c}
                  type="number"
                  step={0.1}
                  value={value[c] ?? 0}
                  onChange={(e) => {
                    const next = [...value];
                    next[c] = Number(e.target.value);
                    updateProps(id, { ...props, value: next });
                  }}
                  className="w-full rounded border border-neutral-600 bg-neutral-900 px-1 text-[10px]"
                />
              );
            })}
          </div>
        ) : null}

        {spec.type === "textureSlot" ? (
          <input
            type="text"
            value={(props.slot as string | undefined) ?? "albedo"}
            onChange={(e) => updateProps(id, { ...props, slot: e.target.value })}
            className="mt-1 w-full rounded border border-neutral-600 bg-neutral-900 px-1 text-[10px]"
          />
        ) : null}
      </div>
    </div>
  );
}

const NODE_TYPES: NodeTypes = { saffron: SaffronNode };

function GraphCanvas({ materialId, onClose }: { materialId: string; onClose: () => void }) {
  const [nodes, setNodes, onNodesChange] = useNodesState<FlowNode>([]);
  const [edges, setEdges, onEdgesChange] = useEdgesState<Edge>([]);
  const [preview, setPreview] = useState<string | null>(null);
  const [status, setStatus] = useState<string>("");
  const loadedRef = useRef(false);

  // Load the material's stored graph into the canvas.
  useEffect(() => {
    loadedRef.current = false;
    void (async () => {
      try {
        const material = await client.materialGet(materialId);
        const { nodes: n, edges: e } = graphToFlow(material.graph as Parameters<typeof graphToFlow>[0]);
        setNodes(n);
        setEdges(e);
        const result = await client.previewRender(materialId, 256);
        setPreview(result.png);
      } catch (err) {
        notifyError(errorText(err));
      }
    })();
  }, [materialId, setNodes, setEdges]);

  const onConnect = useCallback(
    (connection: Connection) => setEdges((eds) => addEdge(connection, eds)),
    [setEdges],
  );

  const updateProps = useCallback(
    (id: string, props: Record<string, unknown>) => {
      setNodes((ns) =>
        ns.map((n) => (n.id === id ? { ...n, data: { ...n.data, props } } : n)),
      );
    },
    [setNodes],
  );
  const nodeCallbacks = useMemo(() => ({ updateProps }), [updateProps]);

  const addNode = useCallback(
    (type: string) => {
      const spec = NODE_SPECS[type];
      if (!spec) {
        return;
      }
      const node: FlowNode = {
        id: freshNodeId(type),
        type: "saffron",
        position: { x: 200 + Math.random() * 120, y: 120 + Math.random() * 120 },
        data: { spec, props: { ...(spec.defaultProps ?? {}) } },
      };
      setNodes((ns) => [...ns, node]);
    },
    [setNodes],
  );

  // Debounced auto-apply: push the graph to the engine and re-render the preview as it changes. Skip
  // the very first settle after load (that graph is already saved).
  useEffect(() => {
    if (!loadedRef.current) {
      loadedRef.current = true;
      return;
    }
    const timer = setTimeout(() => {
      void (async () => {
        try {
          const graph = flowToGraph(nodes, edges);
          const set = await client.materialSetGraph(materialId, graph);
          setStatus(set.foldable ? "applied (folded to params)" : "applied (codegen)");
          const result = await client.previewRender(materialId, 256);
          setPreview(result.png);
        } catch (err) {
          notifyError(errorText(err));
          setStatus("apply failed");
        }
      })();
    }, 500);
    return () => clearTimeout(timer);
  }, [nodes, edges, materialId]);

  const compile = useCallback(async () => {
    try {
      const result = await client.materialCompileGraph(materialId);
      setStatus(result.ok ? "compiled OK" : "compile failed");
    } catch (err) {
      notifyError(errorText(err));
      setStatus("compile failed");
    }
  }, [materialId]);

  const palette = useMemo(() => {
    const cats: Record<string, string[]> = { input: [], math: [], output: [] };
    for (const spec of Object.values(NODE_SPECS)) {
      cats[spec.category]?.push(spec.type);
    }
    return cats;
  }, []);

  return (
    <div className="flex h-full w-full flex-col bg-neutral-950 text-[12px] text-neutral-200">
      <div className="flex items-center gap-2 border-b border-neutral-800 px-3 py-2">
        <span className="font-medium">Material Graph</span>
        <span className="text-neutral-500">{status}</span>
        <div className="ml-auto flex gap-2">
          <Button size="sm" variant="secondary" onClick={() => void compile()}>
            Compile
          </Button>
          <Button size="sm" onClick={onClose}>
            Close
          </Button>
        </div>
      </div>
      <div className="flex min-h-0 flex-1">
        <div className="w-40 shrink-0 overflow-y-auto border-r border-neutral-800 p-2">
          {(["input", "math", "output"] as const).map((cat) => (
            <div key={cat} className="mb-2">
              <div className="mb-1 text-[10px] uppercase text-neutral-500">{cat}</div>
              <div className="flex flex-col gap-1">
                {palette[cat].map((type) => (
                  <button
                    key={type}
                    onClick={() => addNode(type)}
                    className="rounded border border-neutral-700 bg-neutral-800 px-2 py-1 text-left text-[11px] hover:bg-neutral-700"
                  >
                    {NODE_SPECS[type].label}
                  </button>
                ))}
              </div>
            </div>
          ))}
        </div>
        <div className="min-w-0 flex-1">
          <NodeCallbacksContext.Provider value={nodeCallbacks}>
            <ReactFlow
              nodes={nodes}
              edges={edges}
              onNodesChange={onNodesChange}
              onEdgesChange={onEdgesChange}
              onConnect={onConnect}
              nodeTypes={NODE_TYPES}
              fitView
              proOptions={{ hideAttribution: true }}
            >
              <Background />
              <Controls />
            </ReactFlow>
          </NodeCallbacksContext.Provider>
        </div>
        <div className="w-64 shrink-0 border-l border-neutral-800 p-3">
          <div className="mb-2 text-[10px] uppercase text-neutral-500">Preview</div>
          {preview ? (
            <img
              src={`data:image/png;base64,${preview}`}
              alt="material preview"
              className="aspect-square w-full rounded border border-neutral-700 object-cover"
            />
          ) : (
            <div className="flex aspect-square w-full items-center justify-center rounded border border-dashed border-neutral-700 text-neutral-500">
              Rendering…
            </div>
          )}
        </div>
      </div>
    </div>
  );
}

/// Full-screen overlay host. Paints opaque so the viewport subsurface stays covered while open.
export function MaterialGraphEditor({ materialId, onClose }: { materialId: string; onClose: () => void }) {
  return (
    <div className="fixed inset-0 z-50 bg-neutral-950">
      <ReactFlowProvider>
        <GraphCanvas materialId={materialId} onClose={onClose} />
      </ReactFlowProvider>
    </div>
  );
}
