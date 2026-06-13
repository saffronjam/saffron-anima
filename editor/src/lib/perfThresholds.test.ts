import { describe, expect, test } from "bun:test";
import type { PerfConfigDto } from "../protocol";
import { frameTimeStatus, passStatus, vramStatus } from "./perfThresholds";

/// A representative 60fps config mirroring the engine's PerfConfig defaults.
function makeConfig(overrides: Partial<PerfConfigDto> = {}): PerfConfigDto {
  return {
    targetFps: 60,
    budgetMs: 16.67,
    greenBudgetFrac: 0.8,
    greenMedianMul: 1.5,
    amberMedianMul: 2,
    frozenMs: 100,
    vramWarnFrac: 0.8,
    vramCritFrac: 0.95,
    ...overrides,
  };
}

describe("frameTimeStatus", () => {
  const config = makeConfig();

  test("red when over budget", () => {
    // 20ms > 16.67 budget; median high enough that the spike rule does not also trip.
    expect(frameTimeStatus(20, config, 18)).toBe("red");
  });

  test("red on a hard spike (> amberMedianMul x median) even under budget", () => {
    // 10ms is well under the 16.67 budget and under frozenMs, but median is 4 ->
    // amberMedianMul (2) * 4 = 8, and 10 > 8 so the spike rule forces red.
    expect(frameTimeStatus(10, config, 4)).toBe("red");
  });

  test("red when over the frozen band even under budget", () => {
    // budgetMs alone would not catch this if budget were huge, so isolate the frozen rule.
    const loose = makeConfig({ budgetMs: 1000 });
    expect(frameTimeStatus(150, loose, 80)).toBe("red");
  });

  test("amber at the greenBudgetFrac boundary", () => {
    // greenBudgetFrac (0.8) * budget (16.67) = 13.336; ms exactly at the boundary is >= -> amber.
    // Median chosen so neither red spike (2x) nor the green-median rule misgrades:
    // amber-median 2*13 = 26 (no red), green-median 1.5*13 = 19.5 (>= here only via budget frac).
    const ms = config.greenBudgetFrac * config.budgetMs;
    expect(frameTimeStatus(ms, config, 13)).toBe("amber");
  });

  test("amber on a moderate spike (> greenMedianMul x median) under the budget fraction", () => {
    // 7ms is below greenBudgetFrac*budget (13.336) so the budget-frac rule is green, but
    // median 4 -> greenMedianMul (1.5) * 4 = 6, and 7 > 6 -> amber. Stays under amberMedianMul*4=8.
    expect(frameTimeStatus(7, config, 4)).toBe("amber");
  });

  test("green with headroom and a consistent median", () => {
    // 5ms is under the budget fraction (13.336) and under greenMedianMul*median (1.5*6=9).
    expect(frameTimeStatus(5, config, 6)).toBe("green");
  });

  test("medianMs <= 0 does not throw and does not trip the median rules", () => {
    // With median 0 the median-based red/amber clauses are gated off; only budget/frozen apply.
    // 5ms is under budget and frozen, so it must be green despite a non-positive median.
    expect(() => frameTimeStatus(5, config, 0)).not.toThrow();
    expect(frameTimeStatus(5, config, 0)).toBe("green");
    expect(frameTimeStatus(5, config, -10)).toBe("green");
    // A negative median must not flip the grade to red via a sign mistake.
    expect(frameTimeStatus(5, config, -1)).not.toBe("red");
  });
});

describe("vramStatus", () => {
  const config = makeConfig();

  test("fraction >= 1 is red", () => {
    expect(vramStatus(1, config)).toBe("red");
    expect(vramStatus(1.2, config)).toBe("red");
  });

  test("fraction >= crit (but < 1) is red", () => {
    expect(vramStatus(config.vramCritFrac, config)).toBe("red");
    expect(vramStatus(0.97, config)).toBe("red");
  });

  test("fraction >= warn (but < crit) is amber", () => {
    expect(vramStatus(config.vramWarnFrac, config)).toBe("amber");
    expect(vramStatus(0.85, config)).toBe("amber");
  });

  test("fraction below warn is green", () => {
    expect(vramStatus(0.5, config)).toBe("green");
    expect(vramStatus(0, config)).toBe("green");
  });
});

describe("passStatus", () => {
  test("budget <= 0 is green regardless of ms", () => {
    expect(passStatus(50, 0)).toBe("green");
    expect(passStatus(50, -5)).toBe("green");
  });

  test("share > 0.5 of budget is red", () => {
    expect(passStatus(6, 10)).toBe("red"); // 0.6
  });

  test("share > 0.25 (but <= 0.5) of budget is amber", () => {
    expect(passStatus(3, 10)).toBe("amber"); // 0.3
  });

  test("exactly 0.5 is amber (boundary is strict >)", () => {
    expect(passStatus(5, 10)).toBe("amber"); // 0.5 is not > 0.5
  });

  test("exactly 0.25 is green (boundary is strict >)", () => {
    expect(passStatus(2.5, 10)).toBe("green"); // 0.25 is not > 0.25
  });

  test("small share is green", () => {
    expect(passStatus(1, 10)).toBe("green"); // 0.1
  });
});
