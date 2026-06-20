// The cross-engine parity rig (`13-testing-and-verification` phase 7): assert the Rust engine
// matches the C++ engine it replaces on the three contracts the editor and existing projects
// cannot tolerate drifting — rendered pixels, physics sim traces, and serialized bytes — by running
// each comparator against BOTH binaries and emitting a parity report for the cutover sign-off
// (`14-migration`).
//
// CUTOVER-ONLY (NO LEGACY). The rig exists only while both engines are alive. It is gated on the
// C++ binary's presence: once `engine-old/` (and its `build/debug/bin/SaffronAnima` output) is
// deleted at cutover, there is no second engine to diff against, so the suite SKIPS cleanly rather
// than failing. The rig is removed with `engine-old/`.
//
// Autonomous vs deferred (the task gate). The autonomous parts run here on the llvmpipe toolbox and
// are asserted green: the physics sim-trace is bit-identical C++-vs-Rust, and the project DATA
// survives a cross-engine serde round-trip. The non-autonomous parts are RECORDED, not asserted:
// the byte-exact preview-image match is meaningful only on the real GPU the editor ships on
// (DEFERRED-NEEDS-HARDWARE), and the raw-byte serde round-trip carries a recorded key-order
// tolerance. Tolerances are recorded with their reason, never silently loosened — a drift the rig
// cannot explain is a cutover blocker, which is what the report surfaces to the sign-off.

import { afterAll, beforeAll, describe, expect, test } from "bun:test";
import { engines, enginesAvailable, CPP_BIN, RUST_BIN } from "./parity/harness.ts";
import { ParityReporter } from "./parity/report.ts";
import { compareGoldenImage } from "./parity/golden_image.ts";
import { comparePhysicsTrace } from "./parity/physics_trace.ts";
import { compareSerdeRoundtrip } from "./parity/serde_roundtrip.ts";

const available = enginesAvailable();
const guarded = available ? describe : describe.skip;

guarded("cross-engine parity rig (cutover-only)", () => {
  const { cpp, rust } = engines();
  const reporter = new ParityReporter(cpp, rust);

  afterAll(() => {
    // Emit the report artifact even if an assertion failed mid-run, so the sign-off always sees the
    // recorded verdicts.
    const report = reporter.flush();
    console.log(`parity report written to ${ParityReporter.reportPath()} (${report.entries.length} entries)`);
    for (const e of report.entries) {
      console.log(`  [${e.verdict}] ${e.comparator} :: ${e.case} — ${e.detail}`);
    }
  });

  // Physics sim trace — the bit-exact comparator (the determinism gate on the C++-vs-Rust axis).
  test(
    "physics sim trace is bit-identical across the C++ and Rust engines",
    async () => {
      const entry = await comparePhysicsTrace(cpp, rust);
      reporter.record(entry);
      expect(entry.verdict).toBe("exact");
    },
    120_000,
  );

  // Serde byte-equality — the project DATA must survive a cross-engine round-trip in both
  // directions; the raw-byte verdict is recorded (key-order tolerance) but not asserted exact.
  test(
    "a project round-trips data-identically across both engines (both directions)",
    async () => {
      const entries = await compareSerdeRoundtrip(cpp, rust);
      for (const entry of entries) {
        reporter.record(entry);
      }
      const dataEntries = entries.filter((e) => e.case.includes("(data)"));
      expect(dataEntries.length).toBe(2);
      for (const entry of dataEntries) {
        expect(entry.verdict).toBe("exact");
      }
    },
    180_000,
  );

  // Golden image — recorded (preview pipelines differ under llvmpipe; byte-exact deferred to the
  // real GPU). Asserts the rig produces a verdict + that the deferred leg is flagged, not that the
  // images match exactly on software.
  test(
    "golden-image comparator records a verdict and flags the real-GPU leg deferred",
    async () => {
      const entries = await compareGoldenImage(cpp, rust, 64);
      for (const entry of entries) {
        reporter.record(entry);
      }
      const llvmpipe = entries.find((e) => e.case.includes("llvmpipe"));
      expect(llvmpipe).toBeDefined();
      expect(["exact", "tolerance"]).toContain(llvmpipe!.verdict);
      const deferred = entries.find((e) => e.verdict === "deferred");
      expect(deferred).toBeDefined();
    },
    120_000,
  );

  // The report artifact is emitted with at least the three comparators present, plus the standing
  // deferred axes the runner here cannot cover (real GPU, ARM, the live editor).
  test("the parity report covers all three comparators and the deferred axes", () => {
    reporter.recordDeferredAxes();
    const comparators = new Set(reporter.all().map((e) => e.comparator));
    expect(comparators.has("physics-trace")).toBe(true);
    expect(comparators.has("serde-roundtrip")).toBe(true);
    expect(comparators.has("golden-image")).toBe(true);
    expect(reporter.all().some((e) => e.verdict === "deferred")).toBe(true);
  });
});

// A standing record of the gate so a tree with `engine-old/` removed reports *why* the rig skipped
// rather than vanishing silently.
describe("parity rig availability", () => {
  test("the rig is gated on both engine binaries existing (cutover-only)", () => {
    if (!available) {
      console.log(
        `parity rig SKIPPED — missing a binary (cpp=${CPP_BIN} exists=${enginesAvailable()}; ` +
          `rust=${RUST_BIN}). This is expected once engine-old/ is deleted at cutover.`,
      );
    }
    expect(typeof available).toBe("boolean");
  });
});
