import { beforeEach, describe, expect, mock, test } from "bun:test";
import type { AlarmEventDto } from "../protocol";

/// Recorded toast interactions for assertion. The sonner stub appends to these.
const warnings: { message: string; options: unknown; id: number }[] = [];
const errors: { message: string; options: unknown; id: number }[] = [];
const dismissed: (string | number)[] = [];
let nextId = 0;

mock.module("sonner", () => ({
  toast: {
    warning(message: string, options: unknown) {
      const id = ++nextId;
      warnings.push({ message, options, id });
      return id;
    },
    error(message: string, options: unknown) {
      const id = ++nextId;
      errors.push({ message, options, id });
      return id;
    },
    dismiss(id: string | number) {
      dismissed.push(id);
    },
  },
}));

// Imported after the mock is registered so the module binds to the stubbed sonner.
const { routeAlarmToasts, resetAlarmToasts } = await import("./alarmToasts");

/// A firing alarm event with sane defaults; override per case.
function event(over: Partial<AlarmEventDto> = {}): AlarmEventDto {
  return {
    seq: 1,
    fingerprint: "fp",
    metric: "frame-budget",
    pass: "lighting",
    severity: "warning",
    state: "firing",
    value: 18.5,
    threshold: 16.6,
    sinceFrame: 0,
    count: 1,
    durationMs: 0,
    ...over,
  };
}

beforeEach(() => {
  // Clear engine-side tracking and the recorded interactions so each case starts clean.
  resetAlarmToasts();
  warnings.length = 0;
  errors.length = 0;
  dismissed.length = 0;
});

describe("info severity", () => {
  test("an info event raises no toast", () => {
    routeAlarmToasts([event({ severity: "info" })], 0);
    expect(warnings).toHaveLength(0);
    expect(errors).toHaveLength(0);
    expect(dismissed).toHaveLength(0);
  });
});

describe("warning throttle", () => {
  test("a warning at now=0 raises one toast with duration 6000", () => {
    routeAlarmToasts([event()], 0);
    expect(warnings).toHaveLength(1);
    expect(warnings[0]?.options).toEqual({ duration: 6000 });
  });

  test("a same-fingerprint warning at now=5000 is suppressed (inside the 10s window)", () => {
    routeAlarmToasts([event()], 0);
    routeAlarmToasts([event()], 5000);
    expect(warnings).toHaveLength(1);
  });

  test("a same-fingerprint warning at now=11000 raises again (past the 10s window)", () => {
    routeAlarmToasts([event()], 0);
    routeAlarmToasts([event()], 5000); // suppressed
    routeAlarmToasts([event()], 11000); // window elapsed -> raises
    expect(warnings).toHaveLength(2);
  });

  test("a different fingerprint is throttled independently", () => {
    routeAlarmToasts([event({ fingerprint: "a" })], 0);
    routeAlarmToasts([event({ fingerprint: "b" })], 1000);
    expect(warnings).toHaveLength(2);
  });
});

describe("throttle survives fire/resolve cycling", () => {
  test("fire(warning) -> resolve -> fire(warning) within 10s raises only once", () => {
    routeAlarmToasts([event()], 0);
    expect(warnings).toHaveLength(1);
    const firstId = warnings[0]?.id;

    // Resolve dismisses the active toast but must NOT clear the per-fingerprint throttle.
    routeAlarmToasts([event({ state: "resolved" })], 2000);
    expect(dismissed).toContain(firstId);

    // Re-firing inside the window is still suppressed by the surviving throttle.
    routeAlarmToasts([event()], 4000);
    expect(warnings).toHaveLength(1);
  });
});

describe("critical severity", () => {
  test("a critical raises toast.error with duration Infinity", () => {
    routeAlarmToasts([event({ severity: "critical" })], 0);
    expect(errors).toHaveLength(1);
    expect(errors[0]?.options).toEqual({ duration: Infinity });
  });

  test("a second critical for the same fingerprint dismisses the first", () => {
    routeAlarmToasts([event({ severity: "critical" })], 0);
    const firstId = errors[0]?.id;
    routeAlarmToasts([event({ severity: "critical" })], 50);
    expect(errors).toHaveLength(2);
    expect(dismissed).toContain(firstId);
  });

  test("critical is not subject to the warning throttle", () => {
    routeAlarmToasts([event({ severity: "critical" })], 0);
    routeAlarmToasts([event({ severity: "critical" })], 100);
    expect(errors).toHaveLength(2);
  });
});

describe("resolved", () => {
  test("a resolved event dismisses the toast its fingerprint raised", () => {
    routeAlarmToasts([event({ severity: "critical" })], 0);
    const id = errors[0]?.id;
    routeAlarmToasts([event({ state: "resolved" })], 1000);
    expect(dismissed).toEqual([id]);
  });

  test("a resolved event with no active toast dismisses nothing", () => {
    routeAlarmToasts([event({ state: "resolved", fingerprint: "never-fired" })], 0);
    expect(dismissed).toHaveLength(0);
  });

  test("resolving clears the active id, so a re-resolve dismisses nothing", () => {
    routeAlarmToasts([event({ severity: "critical" })], 0);
    routeAlarmToasts([event({ state: "resolved" })], 100);
    dismissed.length = 0;
    routeAlarmToasts([event({ state: "resolved" })], 200);
    expect(dismissed).toHaveLength(0);
  });
});

describe("resetAlarmToasts", () => {
  test("dismisses every tracked toast and clears throttle state", () => {
    routeAlarmToasts([event({ fingerprint: "a", severity: "critical" })], 0);
    routeAlarmToasts([event({ fingerprint: "b" })], 0);
    const ids = [errors[0]?.id, warnings[0]?.id];

    resetAlarmToasts();
    expect(dismissed).toEqual(expect.arrayContaining(ids));

    // Throttle cleared: a fresh warning for "b" raises immediately even at the same `now`.
    warnings.length = 0;
    routeAlarmToasts([event({ fingerprint: "b" })], 0);
    expect(warnings).toHaveLength(1);
  });
});

describe("batched events in one call", () => {
  test("a mixed batch routes each event by severity/state", () => {
    routeAlarmToasts(
      [
        event({ fingerprint: "i", severity: "info" }),
        event({ fingerprint: "w", severity: "warning" }),
        event({ fingerprint: "c", severity: "critical" }),
      ],
      0,
    );
    expect(warnings).toHaveLength(1);
    expect(errors).toHaveLength(1);
    expect(dismissed).toHaveLength(0);
  });
});
