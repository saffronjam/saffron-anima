// Physics sim-trace comparator: run a fixed deterministic scenario through each engine over the
// wire and diff the serialized body-state traces for bit-exactness. This is the determinism gate's
// scenario reused on a *different axis* — C++-engine-vs-Rust-engine rather than x86-vs-ARM. The
// physics phase-5 gate proves the Rust bridge is cross-arch deterministic against a committed
// golden hash; this proves it is bit-identical to the C++ engine's Jolt. Both must hold for the
// lockstep/replay premise.
//
// The scenario is driven deterministically through the control plane: `play` → `pause` →
// `step {frames:1}` advances exactly one fixed tick (`PhysicsFixedStep`), so each engine's trace is
// a function of the tick count, never wall-clock.
//
// THE PRE-PAUSE SLIP, and why we align rather than compare index-for-index. `play` enters Playing
// and the host's main render loop runs `tick_play` every frame until `pause` arrives — so a
// wall-clock-dependent number of fixed steps (observed 1–2, load-dependent) elapse before the pause
// lands. Each engine's per-step integration is bit-deterministic (verified: the same step count
// always yields the identical pose to the last bit), but the two engines can pause at *different*
// step counts on a loaded machine. Comparing index-for-index would then flake on a difference that
// is the driver's race, not an engine divergence. Instead we capture each engine's full trace and
// find the slip offset that aligns them, then assert the aligned overlap is bit-identical: that is
// the real claim (the Rust per-step physics equals the C++ per-step physics across the falling +
// contact + settle trajectory), with the driver's pre-pause race factored out.

import { ParityEngine } from "./harness.ts";
import type { ParityEntry } from "./report.ts";

/// How many fixed ticks each engine records from post-pause. Long enough to cover the full
/// trajectory: fall, contact, and settle on the floor.
const TRACE_TICKS = 120;

/// The maximum pre-pause slip (in fixed ticks) the alignment search tolerates between the two
/// engines. Observed slip is 0–2; ±4 is comfortable headroom.
const MAX_SLIP = 4;

/// The aligned overlap must be at least this many ticks for the comparison to be meaningful (a
/// guard against a degenerate alignment that matches only a trivial tail).
const MIN_OVERLAP = TRACE_TICKS - MAX_SLIP - 8;

interface WorldTransform {
  translation: { x: number; y: number; z: number };
}

/// One f64 as its 8 little-endian bytes in hex — the bit-exact trace element. No tolerance: the raw
/// bit pattern is the comparison unit (matching the in-process gate's raw-byte discipline).
function f64hex(x: number): string {
  const b = Buffer.alloc(8);
  b.writeDoubleLE(x, 0);
  return b.toString("hex");
}

/// The hex length of one tick's sample (3 f64s = 24 bytes = 48 hex chars).
const TICK_HEX = 48;

/// Build the fixed falling-box scenario, step it `TRACE_TICKS` fixed ticks, and return the per-tick
/// world-position hex samples (one entry per tick). Off-axis start (x=0.1, z=-0.2) so the settle
/// exercises all three axes, not a trivial vertical drop.
async function trace(bin: string): Promise<string[]> {
  const e = await ParityEngine.boot(bin, { SAFFRON_AUTO_EMPTY_PROJECT: "1" });
  try {
    const floor = (await e.call<{ id: string }>("create-entity", { name: "Floor" })).id;
    await e.call("set-transform", { entity: floor, translation: { x: 0, y: 0, z: 0 } });
    await e.call("add-component", { entity: floor, component: "Collider" });
    await e.call("set-component-field", {
      entity: floor,
      component: "Collider",
      field: "halfExtents",
      value: { x: 10, y: 0.1, z: 10 },
    });

    const box = (await e.call<{ id: string }>("create-entity", { name: "Box" })).id;
    await e.call("set-transform", { entity: box, translation: { x: 0.1, y: 5, z: -0.2 } });
    await e.call("add-component", { entity: box, component: "Collider" });
    await e.call("add-component", { entity: box, component: "Rigidbody" });

    await e.call("play");
    await e.call("pause");
    const samples: string[] = [];
    for (let i = 0; i < TRACE_TICKS; i++) {
      await e.call("step", { frames: 1 });
      const t = await e.call<WorldTransform>("get-world-transform", { entity: box });
      samples.push(f64hex(t.translation.x) + f64hex(t.translation.y) + f64hex(t.translation.z));
    }
    if (e.validationErrors().length > 0) {
      throw new Error(`physics trace on ${bin} was not validation-clean`);
    }
    return samples;
  } finally {
    await e.shutdown();
  }
}

/// The best slip alignment between two traces, factoring out the pre-pause-window race. Slides one
/// trace against the other by ±`MAX_SLIP` ticks and returns the offset whose overlapping samples
/// are bit-identical with the largest coverage; `null` if no offset yields a bit-identical overlap.
interface Alignment {
  slip: number;
  overlap: number;
}

function align(a: string[], b: string[]): Alignment | null {
  let best: Alignment | null = null;
  for (let slip = -MAX_SLIP; slip <= MAX_SLIP; slip++) {
    // slip > 0: b started `slip` ticks later than a (b is `slip` steps behind), so a[i+slip] ~ b[i].
    const aStart = Math.max(0, slip);
    const bStart = Math.max(0, -slip);
    const overlap = Math.min(a.length - aStart, b.length - bStart);
    if (overlap <= 0) {
      continue;
    }
    let identical = true;
    for (let i = 0; i < overlap; i++) {
      if (a[aStart + i] !== b[bStart + i]) {
        identical = false;
        break;
      }
    }
    if (identical && (best === null || overlap > best.overlap)) {
      best = { slip, overlap };
    }
  }
  return best;
}

/// The first tick (in the best near-zero alignment) at which the traces diverge — the actionable
/// "where did the physics drift" for a genuine mismatch, reported when no alignment is bit-identical.
function firstDivergentTick(a: string[], b: string[]): number {
  const n = Math.min(a.length, b.length);
  for (let i = 0; i < n; i++) {
    if (a[i] !== b[i]) {
      return i;
    }
  }
  return n;
}

/// Run the physics-trace comparator and return the recorded entry. The verdict is `exact` when the
/// two engines' traces align bit-identically over a substantial overlap (the per-step physics is
/// bit-identical, with the pre-pause slip factored out); a genuine divergence (no bit-identical
/// alignment) is recorded `tolerance` naming the first divergent tick — a cutover blocker the
/// sign-off must see, never a silently-loosened pass.
export async function comparePhysicsTrace(cpp: string, rust: string): Promise<ParityEntry> {
  const rustTrace = await trace(rust);
  const cppTrace = await trace(cpp);

  const aligned = align(cppTrace, rustTrace);
  if (aligned !== null && aligned.overlap >= MIN_OVERLAP) {
    const slipNote =
      aligned.slip === 0
        ? "no pre-pause slip"
        : `pre-pause slip ${aligned.slip} tick(s) (driver race, factored out)`;
    return {
      comparator: "physics-trace",
      case: `falling-box, ${TRACE_TICKS} fixed ticks`,
      verdict: "exact",
      detail:
        `the C++ and Rust Jolt traces are bit-identical (raw f64 world positions) across ` +
        `${aligned.overlap} aligned fixed ticks — ${slipNote} — complements the cross-arch ` +
        `determinism gate`,
    };
  }

  const divergentTick = firstDivergentTick(cppTrace, rustTrace);
  return {
    comparator: "physics-trace",
    case: `falling-box, ${TRACE_TICKS} fixed ticks`,
    verdict: "tolerance",
    detail:
      `no pre-pause-slip alignment (±${MAX_SLIP} ticks) makes the traces bit-identical; the ` +
      `nearest alignment first diverges at tick ${divergentTick}. The Rust bridge is NOT ` +
      `bit-identical to the C++ engine's Jolt — a cutover blocker (the lockstep/replay premise): ` +
      `escalate, do not relax the comparison.`,
  };
}
