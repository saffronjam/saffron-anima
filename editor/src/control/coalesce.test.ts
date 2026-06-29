import { afterEach, beforeEach, describe, expect, mock, test } from "bun:test";
import { makeCoalescer } from "./coalesce";

/// A deferred promise we can resolve/reject from the test to drive the coalescer's
/// async `send` sink deterministically.
function deferred<T = void>(): {
  promise: Promise<T>;
  resolve: (value: T) => void;
  reject: (err: unknown) => void;
} {
  let resolve!: (value: T) => void;
  let reject!: (err: unknown) => void;
  const promise = new Promise<T>((res, rej) => {
    resolve = res;
    reject = rej;
  });
  return { promise, resolve, reject };
}

/// The coalescer reads `performance.now()` and schedules via `setTimeout` directly,
/// so we replace those globals with a controllable clock + a manual timer queue.
/// This keeps the throttle window fully deterministic without relying on a fake-timer
/// implementation animating `performance.now`.
interface FakeTimer {
  id: number;
  fireAt: number;
  cb: () => void;
}

let nowMs = 0;
let timers: FakeTimer[] = [];
let nextTimerId = 1;

let realNow: () => number;
let realSetTimeout: typeof setTimeout;
let realClearTimeout: typeof clearTimeout;
let realConsoleError: typeof console.error;

const errorLog: unknown[][] = [];

beforeEach(() => {
  nowMs = 0;
  timers = [];
  nextTimerId = 1;
  errorLog.length = 0;

  realNow = performance.now.bind(performance);
  realSetTimeout = globalThis.setTimeout;
  realClearTimeout = globalThis.clearTimeout;
  realConsoleError = console.error;

  performance.now = () => nowMs;

  globalThis.setTimeout = ((cb: () => void, delay?: number): number => {
    const id = nextTimerId++;
    timers.push({ id, fireAt: nowMs + (delay ?? 0), cb });
    return id;
    // The coalescer only passes a 0-arg closure, so we ignore extra args.
  }) as unknown as typeof setTimeout;

  globalThis.clearTimeout = ((id?: number): void => {
    timers = timers.filter((t) => t.id !== id);
  }) as unknown as typeof clearTimeout;

  console.error = mock((...args: unknown[]) => {
    errorLog.push(args);
  });
});

afterEach(() => {
  performance.now = realNow;
  globalThis.setTimeout = realSetTimeout;
  globalThis.clearTimeout = realClearTimeout;
  console.error = realConsoleError;
});

/// Advance the fake monotonic clock to `target` and fire every timer whose deadline
/// has passed, oldest-deadline first. Re-armed timers scheduled during a callback are
/// picked up on the next loop iteration, mirroring a real event loop draining the queue.
function advanceTo(target: number): void {
  nowMs = target;
  for (;;) {
    const due = timers.filter((t) => t.fireAt <= nowMs).sort((a, b) => a.fireAt - b.fireAt);
    if (due.length === 0) {
      break;
    }
    const next = due[0]!;
    timers = timers.filter((t) => t.id !== next.id);
    next.cb();
  }
}

/// Let queued microtasks settle. The send chain is
/// `Promise.resolve(send(v)).catch(logRejection).finally(...)`, so a resolution/rejection
/// takes several microtask hops to reach `.finally`; await enough ticks to drain them all.
async function flushMicrotasks(): Promise<void> {
  for (let i = 0; i < 8; i++) {
    await Promise.resolve();
  }
}

describe("makeCoalescer throttle + single-in-flight", () => {
  test("a burst within one throttle window coalesces to one send of the LAST value", async () => {
    const sent: number[] = [];
    const gate = deferred();
    const send = mock((v: number) => {
      sent.push(v);
      return gate.promise;
    });
    const c = makeCoalescer<number>({ throttleMs: 16, send });

    // Clock starts at 0 and lastSentAt starts at 0, so the very first push is throttled
    // (elapsed 0 < 16) and only arms a timer. The whole burst lands before any send.
    c.push(1);
    c.push(2);
    c.push(3);

    expect(send).toHaveBeenCalledTimes(0);
    expect(c.stats().sent).toBe(0);

    // Fire the throttle timer.
    advanceTo(16);
    await flushMicrotasks();

    expect(send).toHaveBeenCalledTimes(1);
    expect(sent).toEqual([3]);
    expect(c.stats().sent).toBe(1);
    expect(c.stats().inFlight).toBe(1);
  });

  test("a push while a send is unresolved does not start a second send", async () => {
    const sent: number[] = [];
    const gates: ReturnType<typeof deferred>[] = [];
    const send = mock((v: number) => {
      sent.push(v);
      const g = deferred();
      gates.push(g);
      return g.promise;
    });
    const c = makeCoalescer<number>({ throttleMs: 16, send });

    // Get the first send in flight: push, then clear the throttle window and fire the timer.
    c.push(10);
    advanceTo(16);
    await flushMicrotasks();
    expect(send).toHaveBeenCalledTimes(1);
    expect(c.stats().inFlight).toBe(1);

    // While that send is unresolved, push more. No second send starts; inFlight stays 1.
    advanceTo(100);
    c.push(20);
    c.push(30);
    await flushMicrotasks();
    expect(send).toHaveBeenCalledTimes(1);
    expect(c.stats().inFlight).toBe(1);
    expect(sent).toEqual([10]);

    // Resolving the in-flight send re-drives the pump and flushes the buffered LAST value once.
    gates[0]!.resolve();
    await flushMicrotasks();
    expect(send).toHaveBeenCalledTimes(2);
    expect(sent).toEqual([10, 30]);
    expect(c.stats().sent).toBe(2);
    expect(c.stats().completed).toBe(1);
    expect(c.stats().inFlight).toBe(1);
  });

  test("send starts are spaced at least throttleMs apart", async () => {
    const startTimes: number[] = [];
    let pendingGate = deferred();
    const send = mock((_v: number) => {
      startTimes.push(nowMs);
      const g = pendingGate;
      return g.promise;
    });
    const c = makeCoalescer<number>({ throttleMs: 16, send });

    // First send at t=16 (throttled from the t=0 start).
    c.push(1);
    advanceTo(16);
    await flushMicrotasks();
    expect(startTimes).toEqual([16]);

    // Resolve it almost immediately (t=20). A new push must wait until t>=32 to start.
    advanceTo(20);
    const first = pendingGate;
    pendingGate = deferred();
    first.resolve();
    await flushMicrotasks();

    c.push(2);
    // Not yet 16ms since the last start (lastSentAt=16, now=20).
    expect(startTimes).toEqual([16]);

    advanceTo(32);
    await flushMicrotasks();
    expect(startTimes).toEqual([16, 32]);
    expect(startTimes[1]! - startTimes[0]!).toBeGreaterThanOrEqual(16);
  });

  test("a rejected send is swallowed (no throw) and the next push still sends", async () => {
    const sent: number[] = [];
    let gate = deferred();
    const send = mock((v: number) => {
      sent.push(v);
      return gate.promise;
    });
    const c = makeCoalescer<number>({ throttleMs: 16, send });

    // Start the clock past the 2000ms error-log throttle floor (lastErrorLoggedAt=0) so
    // this first rejection is actually logged rather than suppressed.
    advanceTo(3000);
    c.push(1);
    advanceTo(3016);
    await flushMicrotasks();
    expect(sent).toEqual([1]);
    expect(c.stats().inFlight).toBe(1);

    // Reject the in-flight send. It must not throw; the rejection is logged once.
    const failing = gate;
    gate = deferred();
    failing.reject(new Error("boom"));
    await flushMicrotasks();

    expect(c.stats().completed).toBe(1);
    expect(c.stats().inFlight).toBe(0);
    expect(console.error).toHaveBeenCalledTimes(1);

    // A subsequent push still sends (the failure did not wedge the pump).
    advanceTo(3040);
    c.push(2);
    advanceTo(3056);
    await flushMicrotasks();
    expect(sent).toEqual([1, 2]);
    expect(c.stats().sent).toBe(2);
  });

  test("default throttleMs is 16 when omitted", async () => {
    const sent: number[] = [];
    const gate = deferred();
    const send = mock((v: number) => {
      sent.push(v);
      return gate.promise;
    });
    const c = makeCoalescer<number>({ send });

    c.push(7);
    // Just before the default 16ms window: still no send.
    advanceTo(15);
    await flushMicrotasks();
    expect(sent).toEqual([]);

    advanceTo(16);
    await flushMicrotasks();
    expect(sent).toEqual([7]);
  });

  test("repeated rejections inside the log window are logged at most once", async () => {
    let gate = deferred();
    const send = mock((_v: number) => {
      const g = gate;
      return g.promise;
    });
    const c = makeCoalescer<number>({ throttleMs: 16, send });

    // First send + reject -> one console.error. Start past the 2000ms log-throttle floor
    // (lastErrorLoggedAt=0) so the first rejection is logged at all.
    advanceTo(3000);
    c.push(1);
    advanceTo(3016);
    await flushMicrotasks();
    let failing = gate;
    gate = deferred();
    failing.reject(new Error("a"));
    await flushMicrotasks();
    expect(console.error).toHaveBeenCalledTimes(1);

    // Second send + reject still inside the 2000ms log-throttle window (logged at 3016,
    // now ~3040) -> not logged again.
    advanceTo(3040);
    c.push(2);
    advanceTo(3056);
    await flushMicrotasks();
    failing = gate;
    gate = deferred();
    failing.reject(new Error("b"));
    await flushMicrotasks();
    expect(console.error).toHaveBeenCalledTimes(1);
  });

  test("stats counters track sent/completed across the lifecycle", async () => {
    let gate = deferred();
    const send = mock((_v: number) => {
      const g = gate;
      return g.promise;
    });
    const c = makeCoalescer<number>({ throttleMs: 16, send });

    expect(c.stats()).toEqual({ sent: 0, completed: 0, inFlight: 0 });

    c.push(1);
    advanceTo(16);
    await flushMicrotasks();
    expect(c.stats()).toEqual({ sent: 1, completed: 0, inFlight: 1 });

    const first = gate;
    gate = deferred();
    first.resolve();
    await flushMicrotasks();
    expect(c.stats()).toEqual({ sent: 1, completed: 1, inFlight: 0 });
  });

  test("reset drops the buffered value and cancels the pending timer (no further send)", async () => {
    const sent: number[] = [];
    const gate = deferred();
    const send = mock((v: number) => {
      sent.push(v);
      return gate.promise;
    });
    const c = makeCoalescer<number>({ throttleMs: 16, send });

    // A push at t=0 only arms the throttle timer (elapsed 0 < 16); nothing sent yet.
    c.push(1);
    expect(send).toHaveBeenCalledTimes(0);

    // Reset before the timer fires: the buffered value is dropped and the timer cancelled.
    c.reset();
    advanceTo(100);
    await flushMicrotasks();
    expect(send).toHaveBeenCalledTimes(0);
    expect(sent).toEqual([]);

    // The coalescer is still usable afterwards — a fresh push sends normally.
    c.push(2);
    advanceTo(200);
    await flushMicrotasks();
    expect(sent).toEqual([2]);
  });

  test("reset while a send is in flight stops the buffered follow-up from sending", async () => {
    const sent: number[] = [];
    const gates: ReturnType<typeof deferred>[] = [];
    const send = mock((v: number) => {
      sent.push(v);
      const g = deferred();
      gates.push(g);
      return g.promise;
    });
    const c = makeCoalescer<number>({ throttleMs: 16, send });

    // Get one send in flight.
    c.push(1);
    advanceTo(16);
    await flushMicrotasks();
    expect(sent).toEqual([1]);
    expect(c.stats().inFlight).toBe(1);

    // Buffer a follow-up, then reset: the in-flight send still completes, but the buffered
    // value must not be sent when it resolves.
    c.push(2);
    c.reset();
    gates[0]!.resolve();
    advanceTo(100);
    await flushMicrotasks();
    expect(sent).toEqual([1]);
  });
});
