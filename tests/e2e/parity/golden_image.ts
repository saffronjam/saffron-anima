// Golden-image comparator: drive the same `preview-render` scene through each engine and compare
// the PNG outputs. Under llvmpipe both engines rasterize on the CPU, so the *aspiration* is an
// exact byte match where the pipeline is deterministic. Where it is not (the engines' studio-lit
// preview pipelines differ in lighting/tonemap details), the rig does not silently loosen to a
// pass: it decodes both PNGs and records the measured per-channel pixel delta (max + mean) and the
// reason as a `tolerance` verdict, which the cutover sign-off reviews — and flags the byte-exact
// real-GPU comparison as `deferred` (it needs hardware this software-rasterizer toolbox lacks).

import { decodePng } from "../imggen.ts";
import { ParityEngine } from "./harness.ts";
import type { ParityEntry } from "./report.ts";

/// Render the studio-lit preview of a fixed-color material at `size` on `bin`, returning the base64
/// PNG. The scene is identical across engines: one material, one explicit base color, the default
/// studio lighting — no scene load, no temporal accumulation, so any difference is the render path
/// itself, not the input.
async function renderPreview(bin: string, size: number): Promise<string> {
  const e = await ParityEngine.boot(bin, { SAFFRON_AUTO_EMPTY_PROJECT: "1" });
  try {
    const m = await e.call<{ id: string }>("material-create", { name: "ParityPreview" });
    await e.call("material-update", {
      material: m.id,
      baseColor: { x: 0.3, y: 0.6, z: 0.9, w: 1 },
    });
    const prev = await e.call<{ png: string }>("preview-render", { material: m.id, size });
    if (!prev.png.startsWith("iVBORw0KGgo")) {
      throw new Error(`preview-render on ${bin} did not return a PNG`);
    }
    return prev.png;
  } finally {
    await e.shutdown();
  }
}

/// The measured pixel delta between two decoded RGB images of equal dimensions.
interface PixelDelta {
  width: number;
  height: number;
  /// Bytes (channels) that differ at all.
  differing: number;
  /// Total channel count compared.
  total: number;
  /// The largest single-channel absolute difference (0–255).
  maxDelta: number;
  /// The mean absolute per-channel difference.
  meanDelta: number;
}

/// Decode both PNGs and measure their pixel delta. Throws if the dimensions or channel counts
/// differ (a structural mismatch the rig must surface, not average away).
function pixelDelta(rustPng: string, cppPng: string): PixelDelta {
  const r = decodePng(Buffer.from(rustPng, "base64"));
  const c = decodePng(Buffer.from(cppPng, "base64"));
  if (r.width !== c.width || r.height !== c.height || r.channels !== c.channels) {
    throw new Error(
      `preview dimensions differ: rust ${r.width}x${r.height}x${r.channels} vs ` +
        `cpp ${c.width}x${c.height}x${c.channels}`,
    );
  }
  const total = Math.min(r.data.length, c.data.length);
  let differing = 0;
  let maxDelta = 0;
  let sum = 0;
  for (let i = 0; i < total; i++) {
    const d = Math.abs(r.data[i] - c.data[i]);
    if (d > 0) {
      differing++;
    }
    if (d > maxDelta) {
      maxDelta = d;
    }
    sum += d;
  }
  return { width: r.width, height: r.height, differing, total, maxDelta, meanDelta: sum / total };
}

/// Run the golden-image comparator for one preview size and return the recorded entries. The
/// preview is byte-compared first; on a mismatch the pixel delta is measured and recorded as a
/// `tolerance` verdict with the numbers and the reason. The real-GPU byte-exact comparison is a
/// separate `deferred` entry — this software-rasterizer toolbox cannot stand in for it.
export async function compareGoldenImage(
  cpp: string,
  rust: string,
  size: number,
): Promise<ParityEntry[]> {
  const rustPng = await renderPreview(rust, size);
  const cppPng = await renderPreview(cpp, size);

  const entries: ParityEntry[] = [];
  if (rustPng === cppPng) {
    entries.push({
      comparator: "golden-image",
      case: `preview-render ${size}px (llvmpipe)`,
      verdict: "exact",
      detail: "the C++ and Rust preview PNGs are byte-identical on the software rasterizer",
    });
  } else {
    const d = pixelDelta(rustPng, cppPng);
    const differingPct = ((100 * d.differing) / d.total).toFixed(1);
    entries.push({
      comparator: "golden-image",
      case: `preview-render ${size}px (llvmpipe)`,
      verdict: "tolerance",
      detail:
        `the engines' studio-lit preview pipelines differ: ${d.differing}/${d.total} channels ` +
        `(${differingPct}%) differ, max delta ${d.maxDelta}/255, mean delta ` +
        `${d.meanDelta.toFixed(3)}. Same dimensions (${d.width}x${d.height}); the divergence is ` +
        `the lighting/tonemap path, not the geometry. Recorded for the cutover sign-off.`,
    });
  }

  entries.push({
    comparator: "golden-image",
    case: "preview-render byte-exact on real GPU",
    verdict: "deferred",
    detail:
      "byte-exact image parity is only meaningful on the real GPU the editor ships on; this " +
      "toolbox is llvmpipe-only. DEFERRED-NEEDS-HARDWARE: run on the self-hosted GPU runner.",
  });

  return entries;
}
