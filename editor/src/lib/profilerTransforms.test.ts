import { describe, expect, test } from "bun:test";
import type { ProfileCaptureDto, ProfileCaptureMetadataDto, ProfileSpanDto } from "../protocol";
import { type CaptureTree, spansToFlameTree } from "./captureTree";
import { captureToChromeTrace } from "./chromeTrace";

function span(extra: Partial<ProfileSpanDto> = {}): ProfileSpanDto {
  return {
    name: "span",
    lane: "cpu",
    startNs: 0,
    endNs: 0,
    parentIndex: -1,
    depth: 0,
    ...extra,
  };
}

function metadata(extra: Partial<ProfileCaptureMetadataDto> = {}): ProfileCaptureMetadataDto {
  return {
    softwareGpu: true,
    correlated: false,
    deviceName: "llvmpipe",
    timestampPeriod: 1,
    targetFps: 60,
    mode: "timestamps",
    filter: "",
    frameCount: 1,
    ...extra,
  };
}

function capture(
  spans: ProfileSpanDto[],
  meta: Partial<ProfileCaptureMetadataDto> = {},
): ProfileCaptureDto {
  return { spans, metadata: metadata(meta) };
}

describe("spansToFlameTree", () => {
  test("empty span list yields empty lanes and a zero origin", () => {
    const tree = spansToFlameTree([]);
    expect(tree).toEqual({ cpu: [], gpu: [], originNs: 0 });
  });

  test("builds two lane forests: a nested cpu pair and a separate gpu root", () => {
    // Index 0: cpu root, starting at 2_000_000 ns (2ms) past the gpu origin.
    // Index 1: cpu child nested under index 0.
    // Index 2: gpu root, the earliest start -> the shared origin.
    const spans: ProfileSpanDto[] = [
      span({
        name: "frame",
        lane: "cpu",
        startNs: 3_000_000,
        endNs: 5_000_000,
        parentIndex: -1,
        depth: 0,
      }),
      span({
        name: "pass",
        lane: "cpu",
        startNs: 3_500_000,
        endNs: 4_000_000,
        parentIndex: 0,
        depth: 1,
      }),
      span({
        name: "gpu-pass",
        lane: "gpu",
        startNs: 1_000_000,
        endNs: 2_500_000,
        parentIndex: -1,
        depth: 0,
      }),
    ];
    const tree: CaptureTree = spansToFlameTree(spans);

    // Origin is the earliest start across all spans (the gpu root at 1ms).
    expect(tree.originNs).toBe(1_000_000);

    // CPU lane has one root with one nested child; the gpu span is not in cpu.
    expect(tree.cpu).toHaveLength(1);
    const cpuRoot = tree.cpu[0];
    expect(cpuRoot.name).toBe("frame");
    // start = (3_000_000 - 1_000_000) / 1e6 = 2 ms
    expect(cpuRoot.start).toBeCloseTo(2);
    // duration = (5_000_000 - 3_000_000) / 1e6 = 2 ms
    expect(cpuRoot.duration).toBeCloseTo(2);
    expect(cpuRoot.depth).toBe(0);

    // The child is nested inside the cpu root, not a lane root.
    expect(cpuRoot.children).toHaveLength(1);
    const child = cpuRoot.children[0];
    expect(child.name).toBe("pass");
    // start = (3_500_000 - 1_000_000) / 1e6 = 2.5 ms
    expect(child.start).toBeCloseTo(2.5);
    // duration = (4_000_000 - 3_500_000) / 1e6 = 0.5 ms
    expect(child.duration).toBeCloseTo(0.5);
    expect(child.depth).toBe(1);
    expect(child.children).toEqual([]);

    // GPU lane has its own root, zeroed against the same origin.
    expect(tree.gpu).toHaveLength(1);
    const gpuRoot = tree.gpu[0];
    expect(gpuRoot.name).toBe("gpu-pass");
    // start = (1_000_000 - 1_000_000) / 1e6 = 0 ms (it is the origin)
    expect(gpuRoot.start).toBeCloseTo(0);
    // duration = (2_500_000 - 1_000_000) / 1e6 = 1.5 ms
    expect(gpuRoot.duration).toBeCloseTo(1.5);
    expect(gpuRoot.children).toEqual([]);
  });

  test("clamps a negative-width span (endNs < startNs) to zero duration", () => {
    const spans: ProfileSpanDto[] = [
      span({
        name: "backwards",
        lane: "cpu",
        startNs: 5_000_000,
        endNs: 4_000_000,
        parentIndex: -1,
        depth: 0,
      }),
    ];
    const tree = spansToFlameTree(spans);
    expect(tree.originNs).toBe(5_000_000);
    expect(tree.cpu).toHaveLength(1);
    expect(tree.cpu[0].start).toBeCloseTo(0);
    expect(tree.cpu[0].duration).toBe(0);
  });
});

describe("captureToChromeTrace", () => {
  test("an empty capture yields only the three metadata events", () => {
    const json = captureToChromeTrace(capture([]));
    const parsed = JSON.parse(json);
    expect(parsed.traceEvents).toHaveLength(3);
    expect(parsed.traceEvents.every((e: { ph: string }) => e.ph === "M")).toBe(true);
    expect(parsed.displayTimeUnit).toBe("ns");
  });

  test("emits the metadata block then one X event per span, with lane->tid mapping", () => {
    const spans: ProfileSpanDto[] = [
      span({ name: "cpu-frame", lane: "cpu", startNs: 1000, endNs: 3000, depth: 0 }),
      span({ name: "gpu-pass", lane: "gpu", startNs: 2000, endNs: 5000, depth: 1 }),
    ];
    const json = captureToChromeTrace(capture(spans, { deviceName: "test-device" }));
    const parsed = JSON.parse(json);

    // 3 metadata (M) + 2 complete (X) events.
    expect(parsed.traceEvents).toHaveLength(5);

    const meta = parsed.traceEvents.slice(0, 3);
    expect(meta.map((e: { ph: string }) => e.ph)).toEqual(["M", "M", "M"]);
    expect(meta[0]).toMatchObject({ name: "process_name", pid: "SaffronAnima" });
    expect(meta[1]).toMatchObject({ tid: 1, name: "thread_name" });
    expect(meta[2]).toMatchObject({ tid: 2, name: "thread_name" });

    const events = parsed.traceEvents.slice(3);
    expect(events.every((e: { ph: string }) => e.ph === "X")).toBe(true);

    const cpuEvent = events.find((e: { name: string }) => e.name === "cpu-frame");
    expect(cpuEvent.tid).toBe(1); // cpu -> 1
    // ts/dur in microseconds (ns / 1000).
    expect(cpuEvent.ts).toBeCloseTo(1); // 1000 / 1000
    expect(cpuEvent.dur).toBeCloseTo(2); // (3000 - 1000) / 1000
    expect(cpuEvent.args).toEqual({ depth: 0 });
    expect(cpuEvent.pid).toBe("SaffronAnima");

    const gpuEvent = events.find((e: { name: string }) => e.name === "gpu-pass");
    expect(gpuEvent.tid).toBe(2); // gpu -> 2
    expect(gpuEvent.ts).toBeCloseTo(2); // 2000 / 1000
    expect(gpuEvent.dur).toBeCloseTo(3); // (5000 - 2000) / 1000
    expect(gpuEvent.args).toEqual({ depth: 1 });
  });

  test("clamps a negative-width span's dur to zero and round-trips through JSON.parse", () => {
    const spans: ProfileSpanDto[] = [
      span({ name: "backwards", lane: "cpu", startNs: 9000, endNs: 4000, depth: 0 }),
    ];
    const json = captureToChromeTrace(capture(spans, { frameCount: 2 }));
    // Round-trip: the output is valid JSON.
    const parsed = JSON.parse(json);
    expect(typeof json).toBe("string");

    const xEvent = parsed.traceEvents.find((e: { ph: string }) => e.ph === "X");
    expect(xEvent.dur).toBe(0); // Math.max(0, 4000 - 9000) / 1000
    expect(xEvent.ts).toBeCloseTo(9); // 9000 / 1000

    // metadata is mirrored into otherData.
    expect(parsed.otherData.frameCount).toBe(2);
  });
});
