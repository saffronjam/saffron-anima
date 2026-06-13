import { beforeEach, describe, expect, test } from "bun:test";
import type { FrameSampleDto } from "../protocol";
import { appendFrameSamples, bucketSeries, resetFrameSeries } from "./frameSeries";

/// Build a contiguous run of samples [from..to] inclusive. cpuMs/gpuMs/cpuWaitMs are
/// derived from frameIndex so a bucket's mean is predictable.
function run(from: number, to: number): FrameSampleDto[] {
  const out: FrameSampleDto[] = [];
  for (let i = from; i <= to; i++) {
    out.push({ frameIndex: i, cpuMs: i, gpuMs: i * 2, cpuWaitMs: i * 0.5 });
  }
  return out;
}

// The series is module-scoped state, so every test starts from a clean ring.
beforeEach(() => {
  resetFrameSeries();
});

describe("appendFrameSamples dedup across overlapping windows", () => {
  test("two overlapping windows accumulate without double-counting", () => {
    // First poll returns frames 0..4, the next overlapping poll returns 2..6.
    appendFrameSamples(run(0, 4));
    appendFrameSamples(run(2, 6));

    // 7 distinct frames (0..6); 2,3,4 are not counted twice.
    const s = bucketSeries(7, 1, 100, 1);
    expect(s.x).toHaveLength(7);
    expect(s.total).toHaveLength(7);
    expect(s.cpu).toHaveLength(7);
    expect(s.gpu).toHaveLength(7);

    // bucketFrames=1 → one frame per bucket, oldest..newest left→right.
    // total = cpuMs + cpuWaitMs = i + i*0.5, cpu = i, gpu = i*2.
    for (let i = 0; i <= 6; i++) {
      expect(s.cpu[i]).toBeCloseTo(i);
      expect(s.gpu[i]).toBeCloseTo(i * 2);
      expect(s.total[i]).toBeCloseTo(i + i * 0.5);
    }

    // x is seconds-ago, ascending, newest bucket ≈ 0 at the right edge. (+0 normalizes -0.)
    expect(s.x.map((v) => v + 0)).toEqual([-6, -5, -4, -3, -2, -1, 0]);
  });
});

describe("engine restart resets the history", () => {
  test("a frame index older than the last seen drops the stale run", () => {
    // A run reaches frame 100, then the engine restarts and resets to frame 0.
    appendFrameSamples([{ frameIndex: 100, cpuMs: 9, gpuMs: 18, cpuWaitMs: 1 }]);
    appendFrameSamples([{ frameIndex: 0, cpuMs: 3, gpuMs: 6, cpuWaitMs: 2 }]);

    // Only the restarted run's single frame remains.
    const s = bucketSeries(100, 1, 100, 1);
    expect(s.cpu).toHaveLength(1);
    expect(s.cpu[0]).toBeCloseTo(3);
    expect(s.gpu[0]).toBeCloseTo(6);
    expect(s.total[0]).toBeCloseTo(3 + 2);
    // Single bucket sits at the right edge (≈ 0). (+0 normalizes -0.)
    expect(s.x.map((v) => v + 0)).toEqual([0]);
  });
});

describe("empty / reset series", () => {
  test("bucketSeries on a fresh series returns empty arrays", () => {
    const s = bucketSeries(7, 1, 100, 1);
    expect(s).toEqual({ x: [], total: [], cpu: [], gpu: [] });
  });

  test("bucketSeries after an explicit reset returns empty arrays", () => {
    appendFrameSamples(run(0, 10));
    resetFrameSeries();
    expect(bucketSeries(11, 1, 100, 1)).toEqual({ x: [], total: [], cpu: [], gpu: [] });
  });
});

describe("downsampling into a fixed bucket count", () => {
  test("1000 frames into 50 buckets yields the mean of each 20-frame group", () => {
    appendFrameSamples(run(0, 999));

    const bucketSeconds = 0.25;
    const s = bucketSeries(1000, 1, 50, bucketSeconds);

    // maxBuckets caps it at 50 even though bucketFrames=1 would ask for 1000.
    expect(s.x).toHaveLength(50);
    expect(s.total).toHaveLength(50);
    expect(s.cpu).toHaveLength(50);
    expect(s.gpu).toHaveLength(50);

    // size = 1000/50 = 20 → bucket b covers frames [20b .. 20b+19].
    // cpuMs = frameIndex, so the mean is 20b + 9.5.
    for (let b = 0; b < 50; b++) {
      const meanIndex = 20 * b + 9.5;
      expect(s.cpu[b]).toBeCloseTo(meanIndex);
      expect(s.gpu[b]).toBeCloseTo(meanIndex * 2);
      expect(s.total[b]).toBeCloseTo(meanIndex + meanIndex * 0.5);
    }

    // x ascending: oldest most-negative, newest ≈ 0.
    for (let i = 1; i < s.x.length; i++) {
      expect(s.x[i]).toBeGreaterThan(s.x[i - 1]);
    }
    expect(s.x[49]).toBeCloseTo(0);
    expect(s.x[0]).toBeCloseTo(-(50 - 1) * bucketSeconds);
  });
});
