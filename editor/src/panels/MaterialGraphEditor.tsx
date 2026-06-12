/// The node-graph material editor: a React Flow canvas + live preview, hosted as a main tab (see
/// App.tsx / openMaterialGraphTab). Loads a material's stored graph (material-get), lets you
/// add (right-click the canvas) / connect / edit nodes, and auto-applies changes (debounced) via
/// material-set-graph — re-rendering the studio-lit preview sphere so the surface morphs as you
/// edit. "Compile" forces codegen (material-compile-graph) for procedural graphs that don't fold to
/// params. Node types mirror the engine emitter (materials/graph.ts).
import {
  createContext,
  useCallback,
  useContext,
  useEffect,
  useMemo,
  useRef,
  useState,
} from "react";
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
  useReactFlow,
} from "@xyflow/react";
import "@xyflow/react/dist/style.css";
import { Hammer } from "lucide-react";
import { client } from "../control/client";
import { ColorField } from "../components/ColorField";
import { errorText, notifyError } from "../lib/flash";
import { humanizeFieldName } from "../lib/humanize";
import {
  type FlowNode,
  flowToGraph,
  freshNodeId,
  graphToFlow,
  type NodeCategory,
  NODE_SPECS,
  type SaffronNodeData,
  TEXTURE_SLOTS,
} from "../materials/graph";
import { Button } from "@/components/ui/button";
import {
  ContextMenu,
  ContextMenuContent,
  ContextMenuGroup,
  ContextMenuItem,
  ContextMenuLabel,
  ContextMenuTrigger,
} from "@/components/ui/context-menu";
import { ResizableHandle, ResizablePanel, ResizablePanelGroup } from "@/components/ui/resizable";
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from "@/components/ui/select";

/// Last rendered preview PNG per material, kept module-level so it survives the tab unmounting:
/// switching to the Scene tab and back shows the cached sphere immediately instead of re-rendering.
const previewCache = new Map<string, string>();

interface NodeCallbacks {
  updateProps: (id: string, props: Record<string, unknown>) => void;
}
const NodeCallbacksContext = createContext<NodeCallbacks>({ updateProps: () => {} });

/// Pin labels are Sentence case (humanizeFieldName), but single-letter math pins (a/b/t) stay lowercase.
function pinLabel(pin: string): string {
  return pin.length === 1 ? pin : humanizeFieldName(pin);
}

/// The single output handle + label, vertically centered in its row (used by the editor-node layout so
/// the anchor sits beside the editor, not in a separate pin row above it).
function OutputAnchor({ pin }: { pin: string }) {
  return (
    <>
      <span className="ml-auto pr-3 text-muted-foreground">{pinLabel(pin)}</span>
      <Handle
        type="source"
        position={Position.Right}
        id={pin}
        className="!h-2 !w-2 !border-muted-foreground !bg-emerald-500"
      />
    </>
  );
}

/// One graph node card. Editor nodes (constant → color picker, texture → slot select) put the editor in a
/// single body row with the output anchor centered on its right. Other nodes are pins only: a row per pin,
/// inputs on the left + outputs on the right, the handle centered on its label.
function SaffronNode({ id, data }: NodeProps<FlowNode>) {
  const { spec, props } = data as SaffronNodeData;
  const { updateProps } = useContext(NodeCallbacksContext);
  const hasEditor = spec.type === "constant" || spec.type === "textureSlot";
  const editorOutput = spec.outputs[0];

  return (
    <div className="min-w-[150px] rounded border border-border bg-card text-[11px] text-foreground shadow">
      <div className="rounded-t border-b border-border bg-muted px-2 py-1 font-medium">
        {spec.label}
      </div>
      {hasEditor ? (
        <div className="relative flex items-center gap-2 py-2 pl-2">
          {spec.type === "constant" ? (
            (() => {
              const value = (props.value as number[] | undefined) ?? [1, 1, 1, 1];
              // Fixed width so the inline rgba channels shrink to fit instead of expanding to the
              // native number-input width (which blows the node out to ~600px).
              return (
                <div className="nodrag w-44">
                  <ColorField
                    kind="color4"
                    value={{
                      x: value[0] ?? 0,
                      y: value[1] ?? 0,
                      z: value[2] ?? 0,
                      w: value[3] ?? 1,
                    }}
                    onChange={(patch) => {
                      const next = [...value];
                      if (patch.x !== undefined) next[0] = patch.x;
                      if (patch.y !== undefined) next[1] = patch.y;
                      if (patch.z !== undefined) next[2] = patch.z;
                      if (patch.w !== undefined) next[3] = patch.w;
                      updateProps(id, { ...props, value: next });
                    }}
                  />
                </div>
              );
            })()
          ) : (
            <Select
              value={(props.slot as string | undefined) ?? "albedo"}
              onValueChange={(slot) => updateProps(id, { ...props, slot })}
            >
              <SelectTrigger size="sm" className="nodrag h-7 w-36 text-[11px]">
                <SelectValue />
              </SelectTrigger>
              <SelectContent>
                {TEXTURE_SLOTS.map((slot) => (
                  <SelectItem key={slot} value={slot} className="text-[11px]">
                    {humanizeFieldName(slot)}
                  </SelectItem>
                ))}
              </SelectContent>
            </Select>
          )}
          {editorOutput ? <OutputAnchor pin={editorOutput} /> : null}
        </div>
      ) : (
        <div className="py-1">
          {Array.from({ length: Math.max(spec.inputs.length, spec.outputs.length) }).map((_, k) => {
            const inPin = spec.inputs[k];
            const outPin = spec.outputs[k];
            return (
              <div
                key={`${inPin ?? ""}|${outPin ?? ""}`}
                className="relative flex h-6 items-center justify-between"
              >
                {inPin ? (
                  <Handle
                    type="target"
                    position={Position.Left}
                    id={inPin}
                    className="!h-2 !w-2 !border-muted-foreground !bg-sky-500"
                  />
                ) : null}
                <span className="pl-3 text-muted-foreground">{inPin ? pinLabel(inPin) : ""}</span>
                <span className="pr-3 text-muted-foreground">{outPin ? pinLabel(outPin) : ""}</span>
                {outPin ? (
                  <Handle
                    type="source"
                    position={Position.Right}
                    id={outPin}
                    className="!h-2 !w-2 !border-muted-foreground !bg-emerald-500"
                  />
                ) : null}
              </div>
            );
          })}
        </div>
      )}
    </div>
  );
}

const NODE_TYPES: NodeTypes = { saffron: SaffronNode };
const PALETTE_CATEGORIES: NodeCategory[] = ["input", "math", "output"];

function GraphCanvas({ materialId }: { materialId: string }) {
  const [nodes, setNodes, onNodesChange] = useNodesState<FlowNode>([]);
  const [edges, setEdges, onEdgesChange] = useEdgesState<Edge>([]);
  const [preview, setPreview] = useState<string | null>(() => previewCache.get(materialId) ?? null);
  const [status, setStatus] = useState<string>("");
  const loadedRef = useRef(false);
  // The screen point of the last right-click, so the context menu creates a node under the cursor.
  const menuPosRef = useRef<{ x: number; y: number } | null>(null);
  const reactFlow = useReactFlow();

  // Load the material's stored graph into the canvas. Show the cached preview immediately, then
  // re-render to refresh it (and cache the result so the next tab switch is instant).
  useEffect(() => {
    loadedRef.current = false;
    setPreview(previewCache.get(materialId) ?? null);
    void (async () => {
      try {
        const material = await client.materialGet(materialId);
        const { nodes: n, edges: e } = graphToFlow(
          material.graph as Parameters<typeof graphToFlow>[0],
        );
        setNodes(n);
        setEdges(e);
        const result = await client.previewRender(materialId, 256);
        previewCache.set(materialId, result.png);
        setPreview(result.png);
      } catch (err) {
        notifyError(errorText(err));
      }
    })();
  }, [materialId, setNodes, setEdges]);

  const onConnect = useCallback(
    (connection: Connection) => {
      if (connection.source === connection.target) {
        return; // no self-loops
      }
      // The emitter takes one source per input pin, so a new wire into an occupied input replaces it.
      setEdges((eds) => {
        const freed = eds.filter(
          (e) => !(e.target === connection.target && e.targetHandle === connection.targetHandle),
        );
        return addEdge(connection, freed);
      });
    },
    [setEdges],
  );

  const updateProps = useCallback(
    (id: string, props: Record<string, unknown>) => {
      setNodes((ns) => ns.map((n) => (n.id === id ? { ...n, data: { ...n.data, props } } : n)));
    },
    [setNodes],
  );
  const nodeCallbacks = useMemo(() => ({ updateProps }), [updateProps]);
  // Reject a self-loop during the drag (visual feedback); onConnect enforces the rest.
  const isValidConnection = useCallback((c: Connection | Edge) => c.source !== c.target, []);

  // Create a node from the context menu at the last right-click position (flow coordinates).
  const addNode = useCallback(
    (type: string) => {
      const spec = NODE_SPECS[type];
      if (!spec) {
        return;
      }
      const screen = menuPosRef.current;
      const position = screen ? reactFlow.screenToFlowPosition(screen) : { x: 200, y: 120 };
      const node: FlowNode = {
        id: freshNodeId(type),
        type: "saffron",
        position,
        data: { spec, props: { ...(spec.defaultProps ?? {}) } },
      };
      setNodes((ns) => [...ns, node]);
    },
    [reactFlow, setNodes],
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
          previewCache.set(materialId, result.png);
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
    const cats: Record<NodeCategory, string[]> = { input: [], math: [], output: [] };
    for (const spec of Object.values(NODE_SPECS)) {
      cats[spec.category].push(spec.type);
    }
    return cats;
  }, []);

  return (
    <div className="flex h-full w-full flex-col bg-background text-[12px] text-foreground">
      <div className="flex items-center gap-2 border-b border-border px-3 py-2">
        <span className="font-medium">Material graph</span>
        <span className="text-muted-foreground">{status}</span>
        <div className="ml-auto flex gap-2">
          <Button size="sm" variant="secondary" onClick={() => void compile()}>
            <Hammer className="size-3.5" />
            Compile
          </Button>
        </div>
      </div>
      <ResizablePanelGroup orientation="horizontal" className="min-h-0 flex-1">
        <ResizablePanel defaultSize={76} minSize={40} className="min-w-0">
          {/* modal=false: the default modal overlay swallows a second right-click, so the menu
              wouldn't reposition until dismissed. Without it, each right-click re-anchors here. */}
          <ContextMenu modal={false}>
            <ContextMenuTrigger asChild>
              <div
                className="h-full w-full"
                onContextMenu={(e) => {
                  menuPosRef.current = { x: e.clientX, y: e.clientY };
                }}
              >
                <NodeCallbacksContext.Provider value={nodeCallbacks}>
                  <ReactFlow
                    nodes={nodes}
                    edges={edges}
                    onNodesChange={onNodesChange}
                    onEdgesChange={onEdgesChange}
                    onConnect={onConnect}
                    isValidConnection={isValidConnection}
                    nodeTypes={NODE_TYPES}
                    colorMode="dark"
                    fitView
                    fitViewOptions={{ maxZoom: 1, padding: 0.3 }}
                    proOptions={{ hideAttribution: true }}
                  >
                    <Background />
                    <Controls />
                  </ReactFlow>
                </NodeCallbacksContext.Provider>
              </div>
            </ContextMenuTrigger>
            <ContextMenuContent className="w-44">
              {PALETTE_CATEGORIES.map((cat) => (
                <ContextMenuGroup key={cat}>
                  <ContextMenuLabel className="text-[10px] uppercase text-muted-foreground">
                    {cat}
                  </ContextMenuLabel>
                  {palette[cat].map((type) => (
                    <ContextMenuItem key={type} onSelect={() => addNode(type)}>
                      {NODE_SPECS[type].label}
                    </ContextMenuItem>
                  ))}
                </ContextMenuGroup>
              ))}
            </ContextMenuContent>
          </ContextMenu>
        </ResizablePanel>
        <ResizableHandle />
        <ResizablePanel defaultSize={24} minSize={12} className="min-w-0">
          <div className="h-full overflow-y-auto p-3">
            <div className="mb-2 text-[10px] uppercase text-muted-foreground">Preview</div>
            {preview ? (
              <img
                src={`data:image/png;base64,${preview}`}
                alt="material preview"
                className="aspect-square w-full rounded border border-border object-cover"
              />
            ) : (
              <div className="flex aspect-square w-full items-center justify-center rounded border border-dashed border-border text-muted-foreground">
                Rendering…
              </div>
            )}
          </div>
        </ResizablePanel>
      </ResizablePanelGroup>
    </div>
  );
}

/// The material-graph main-tab body (mounted by App.tsx when a `materialGraph` ViewTab is active).
export function MaterialGraphEditor({ materialId }: { materialId: string }) {
  return (
    <ReactFlowProvider>
      <GraphCanvas materialId={materialId} />
    </ReactFlowProvider>
  );
}
