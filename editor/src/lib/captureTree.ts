/// Pure transform from a flat, lane-tagged ProfileSpanDto[] (the wire shape) into the
/// per-lane flame-node forest the Profiler views consume. Kept a pure function so it is
/// unit-testable and the panel stays declarative. This is NOT the live-HUD frameSeries
/// ring — it is a per-capture, request-scoped structure.
import type { ProfileSpanDto } from "../protocol";

/// A flame-chart node: zero-based ms `start`/`duration` (so the timeline begins at 0 and
/// the CPU + GPU lanes share one origin when the capture is correlated), name, children.
export interface FlameNode {
  name: string;
  start: number;
  duration: number;
  depth: number;
  children: FlameNode[];
}

export interface CaptureTree {
  /// Top-level CPU lifecycle/pass spans, each with its nested children.
  cpu: FlameNode[];
  /// Top-level GPU pass spans, each with its nested sub-scopes.
  gpu: FlameNode[];
  /// The shared host-ns origin both lanes are zeroed against (the earliest span start).
  originNs: number;
}

/// Decode the flat span list into one tree per lane. `parentIndex` points within the same
/// lane (the engine rebases it per frame), so the forest is lane-coherent; a span with no
/// parent is a lane root. Both lanes are zeroed to the capture's earliest start, keeping a
/// correlated CPU/GPU pair aligned on one axis.
export function spansToFlameTree(spans: ProfileSpanDto[]): CaptureTree {
  if (spans.length === 0) {
    return { cpu: [], gpu: [], originNs: 0 };
  }
  let originNs = spans[0].startNs;
  for (const span of spans) {
    if (span.startNs < originNs) {
      originNs = span.startNs;
    }
  }
  const nodes: FlameNode[] = spans.map((span) => ({
    name: span.name,
    start: (span.startNs - originNs) / 1e6,
    duration: Math.max(0, span.endNs - span.startNs) / 1e6,
    depth: span.depth,
    children: [],
  }));
  const cpu: FlameNode[] = [];
  const gpu: FlameNode[] = [];
  spans.forEach((span, index) => {
    const node = nodes[index];
    if (span.parentIndex >= 0 && span.parentIndex < nodes.length) {
      nodes[span.parentIndex].children.push(node);
    } else if (span.lane === "gpu") {
      gpu.push(node);
    } else {
      cpu.push(node);
    }
  });
  return { cpu, gpu, originNs };
}
