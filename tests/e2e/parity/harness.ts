// The cross-engine parity harness: boots a *named* engine binary headlessly and drives it over
// the same JSON-over-unix-socket control plane the editor uses. It mirrors `../harness.ts`, but
// takes the binary path as an argument instead of freezing `SAFFRON_ANIMA_BIN` at import — the
// parity rig drives both the C++ `SaffronAnima` and the Rust `saffron-host` from one process, so
// it cannot rely on the import-time freeze the shared harness uses.
//
// This rig is cutover-only. It compares the Rust engine against the C++ engine it replaces and is
// deleted with `engine-old/` at cutover (NO LEGACY) — there is no second engine to diff against
// once that directory is gone. [`enginesAvailable`] reports whether both binaries are present so
// the suite skips cleanly (not fails) on a tree where the C++ engine has already been removed.

import { spawn, type ChildProcess } from "node:child_process";
import net from "node:net";
import { existsSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const HERE = dirname(fileURLToPath(import.meta.url));
export const REPO = join(HERE, "..", "..", "..");

/// The C++ `SaffronAnima` host (the binary `engine-old/` builds into `build/debug/bin/`). The rig
/// is gated on this existing — once `engine-old/` and its build output are gone, parity no-ops.
export const CPP_BIN = join(REPO, "build", "debug", "bin", "SaffronAnima");

/// The Rust `saffron-host` host produced by `cargo build` (the binary the cutover flips to).
export const RUST_BIN = join(REPO, "engine", "target", "debug", "saffron-host");

/// The two engines a parity comparator diffs, by label.
export interface Engines {
  cpp: string;
  rust: string;
}

/// Both binaries by their parity labels. A comparator runs each leg against `engines().cpp` and
/// `engines().rust`.
export function engines(): Engines {
  return { cpp: CPP_BIN, rust: RUST_BIN };
}

/// Whether the rig can run: both the C++ and Rust binaries are present on disk. The C++ binary is
/// the cutover-only half — when `engine-old/` has been deleted (and so its build output is gone),
/// this is false and the parity suite skips rather than fails. The Rust binary is built by
/// `cargo build` / `make engine`.
export function enginesAvailable(): boolean {
  return existsSync(CPP_BIN) && existsSync(RUST_BIN);
}

const delay = (ms: number) => new Promise<void>((resolve) => setTimeout(resolve, ms));

async function waitFor(ready: () => boolean, timeoutMs: number, what: string): Promise<void> {
  const start = Date.now();
  while (!ready()) {
    if (Date.now() - start > timeoutMs) {
      throw new Error(`timeout waiting for ${what}`);
    }
    await delay(50);
  }
}

/// A booted engine of a *specified* binary plus a minimal control client. Always `shutdown()`.
export class ParityEngine {
  readonly bin: string;
  readonly socketPath: string;
  private proc: ChildProcess;
  private weston: ChildProcess;
  private exited = false;
  private buf = "";
  private nextId = 1;

  private constructor(bin: string, proc: ChildProcess, weston: ChildProcess, socketPath: string) {
    this.bin = bin;
    this.proc = proc;
    this.weston = weston;
    this.socketPath = socketPath;
  }

  /// Everything the engine has written to stdout+stderr so far.
  get log(): string {
    return this.buf;
  }

  /// Lines the validation layers flagged as errors (empty = clean).
  validationErrors(): string[] {
    return this.buf
      .split("\n")
      .filter((line) => line.includes("[saffron:vulkan] error: [validation]"));
  }

  /// Boot `bin` headlessly on its own weston + control socket, with `env` overlaid on the inherited
  /// environment (e.g. `SAFFRON_AUTO_EMPTY_PROJECT`).
  static async boot(bin: string, env: Record<string, string> = {}): Promise<ParityEngine> {
    const runtime = process.env.XDG_RUNTIME_DIR ?? `/run/user/${process.getuid?.() ?? 1000}`;
    const stamp = `${process.pid}-${Date.now()}-${Math.floor(Math.random() * 1e6)}`;
    const wlSocket = `wl-parity-${stamp}`;
    const weston = spawn(
      "weston",
      ["--backend=headless", "--width=1280", "--height=720", `--socket=${wlSocket}`, "--idle-time=0"],
      { env: { ...process.env, XDG_RUNTIME_DIR: runtime }, stdio: "ignore" },
    );
    await waitFor(() => existsSync(join(runtime, wlSocket)), 10_000, "weston socket");

    const socketPath = `/tmp/saffron-parity-${stamp}.sock`;
    const proc = spawn(bin, [], {
      cwd: REPO,
      env: {
        ...process.env,
        XDG_RUNTIME_DIR: runtime,
        WAYLAND_DISPLAY: wlSocket,
        SDL_VIDEODRIVER: "wayland",
        SAFFRON_CONTROL_SOCK: socketPath,
        ...env,
      },
      stdio: ["ignore", "pipe", "pipe"],
    });
    const engine = new ParityEngine(bin, proc, weston, socketPath);
    proc.stdout?.on("data", (d) => (engine.buf += d.toString()));
    proc.stderr?.on("data", (d) => (engine.buf += d.toString()));
    proc.on("exit", () => (engine.exited = true));

    await waitFor(() => engine.exited || existsSync(socketPath), 30_000, "control socket");
    if (engine.exited) {
      throw new Error(`engine ${bin} exited before the control socket appeared:\n${engine.buf}`);
    }
    return engine;
  }

  /// Send one control command; resolves its `result`, rejects on `ok:false` or transport error.
  call<T = unknown>(cmd: string, params: Record<string, unknown> = {}): Promise<T> {
    return new Promise<T>((resolve, reject) => {
      const socket = net.connect({ path: this.socketPath });
      const id = this.nextId++;
      let data = "";
      const timer = setTimeout(() => {
        socket.destroy();
        reject(new Error(`timeout calling ${cmd}`));
      }, 15_000);
      socket.on("connect", () => socket.write(JSON.stringify({ id, cmd, params }) + "\n"));
      socket.on("data", (chunk) => {
        data += chunk.toString();
        const nl = data.indexOf("\n");
        if (nl < 0) {
          return;
        }
        clearTimeout(timer);
        socket.end();
        let envelope: { ok?: boolean; result?: T; error?: string };
        try {
          envelope = JSON.parse(data.slice(0, nl));
        } catch (err) {
          reject(err as Error);
          return;
        }
        if (envelope.ok === false) {
          reject(new Error(`${cmd}: ${envelope.error}`));
        } else {
          resolve(envelope.result as T);
        }
      });
      socket.on("error", (err) => {
        clearTimeout(timer);
        reject(err);
      });
    });
  }

  async shutdown(): Promise<void> {
    try {
      await this.call("quit");
    } catch {
      // already gone, or quit raced the socket close
    }
    this.proc.kill("SIGTERM");
    this.weston.kill("SIGTERM");
    await delay(100);
  }
}
