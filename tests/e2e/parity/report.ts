// The parity report: the structured verdict the rig emits, consumed by the cutover sign-off
// (`14-migration`). Each comparator records one of three verdicts — `exact` (byte/bit-identical
// across both engines, the cutover-clean case), `tolerance` (a measured, *recorded* difference
// with its reason, which the sign-off reviews explicitly), or `deferred` (a leg that cannot run
// on this hardware — real GPU, a second arch, the live editor — flagged for the runner that can).
//
// Tolerances are recorded, never hidden (the phase's discipline): a difference the rig cannot
// explain is a cutover blocker, not a rounding footnote, so each non-`exact` verdict carries the
// `detail` an implementer or sign-off reviewer acts on.

import { writeFileSync } from "node:fs";
import { join } from "node:path";
import { REPO } from "./harness.ts";

/// One comparator's verdict.
export type Verdict = "exact" | "tolerance" | "deferred";

/// One comparator's recorded result.
export interface ParityEntry {
  /// The comparator name (`golden-image`, `physics-trace`, `serde-roundtrip`).
  comparator: string;
  /// The specific case within the comparator (which scene / direction / scenario).
  case: string;
  verdict: Verdict;
  /// Human-readable detail: the measured tolerance + why for `tolerance`, the missing
  /// capability for `deferred`, a one-line confirmation for `exact`.
  detail: string;
}

/// The whole parity run, written to `appdata/parity-report.json` for the cutover sign-off.
export interface ParityReport {
  generatedAt: string;
  cppBin: string;
  rustBin: string;
  entries: ParityEntry[];
}

/// Accumulates entries across comparators and writes the JSON artifact on `flush`.
export class ParityReporter {
  private entries: ParityEntry[] = [];

  constructor(
    private readonly cppBin: string,
    private readonly rustBin: string,
  ) {}

  record(entry: ParityEntry): void {
    this.entries.push(entry);
  }

  /// All recorded entries (for in-test assertions on the verdict set).
  all(): readonly ParityEntry[] {
    return this.entries;
  }

  /// Record the standing deferred axes — the parity legs that cannot run on this software-rasterizer
  /// x86 toolbox and must run on the runner that can. They belong in the report so the cutover
  /// sign-off has the full picture of what is *not yet* covered, not just what passed here.
  recordDeferredAxes(): void {
    this.record({
      comparator: "cross-arch-determinism",
      case: "Rust-x86 trace hash == Rust-aarch64 trace hash",
      verdict: "deferred",
      detail:
        "the cross-arch half of the physics determinism gate (this rig proves C++-vs-Rust on x86; " +
        "the aarch64 equality runs on the self-hosted ARM runner against the committed golden hash). " +
        "DEFERRED-NEEDS-HARDWARE.",
    });
    this.record({
      comparator: "live-editor",
      case: "Tauri editor drives the Rust host on a Wayland subsurface",
      verdict: "deferred",
      detail:
        "presenting host frames under the transparent webview needs a real Wayland session + GPU; " +
        "the headless toolbox cannot stand in. DEFERRED-NEEDS-DISPLAY: verified at cutover sign-off.",
    });
  }

  /// The report artifact path under the gitignored `appdata/`.
  static reportPath(): string {
    return join(REPO, "appdata", "parity-report.json");
  }

  /// Write the report JSON and return the report object. Idempotent; safe to call from `afterAll`.
  flush(): ParityReport {
    const report: ParityReport = {
      generatedAt: new Date().toISOString(),
      cppBin: this.cppBin,
      rustBin: this.rustBin,
      entries: this.entries,
    };
    writeFileSync(ParityReporter.reportPath(), JSON.stringify(report, null, 2) + "\n");
    return report;
  }
}
