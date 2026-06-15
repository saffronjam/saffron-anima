// End-to-end harness for SaffronAnima: boots a real engine headlessly and drives it over
// the JSON-over-unix-socket control plane — the same wire the editor and `sa` CLI use. No
// C++ is involved beyond the engine binary; tests are plain TypeScript on `bun test`.
//
// Each Engine spawns its own headless weston so runs are isolated and never open a window,
// then launches build/debug/bin/SaffronAnima pointed at a per-run control socket. Engine
// stdout+stderr (incl. Vulkan validation messages) is captured into `.log` for assertions.

import { spawn, type ChildProcess } from "node:child_process";
import net from "node:net";
import { existsSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const HERE = dirname(fileURLToPath(import.meta.url));
export const REPO = join(HERE, "..", "..");
export const ENGINE_BIN =
  process.env.SAFFRON_ANIMA_BIN ?? join(REPO, "build", "debug", "bin", "SaffronAnima");

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

/// A booted engine plus a typed control client. Always call shutdown() (afterAll/finally).
export class Engine {
  readonly socketPath: string;
  private proc: ChildProcess;
  private weston: ChildProcess;
  private exited = false;
  private buf = "";
  private nextId = 1;

  private constructor(proc: ChildProcess, weston: ChildProcess, socketPath: string) {
    this.proc = proc;
    this.weston = weston;
    this.socketPath = socketPath;
  }

  /// Everything the engine has written to stdout+stderr so far.
  get log(): string {
    return this.buf;
  }

  /// Lines the validation layers flagged as errors (empty = clean). The engine's debug
  /// messenger prints them as `[saffron:vulkan] error: [validation] …`.
  validationErrors(): string[] {
    return this.buf
      .split("\n")
      .filter((line) => line.includes("[saffron:vulkan] error: [validation]"));
  }

  static async boot(env: Record<string, string> = {}): Promise<Engine> {
    const runtime = process.env.XDG_RUNTIME_DIR ?? `/run/user/${process.getuid?.() ?? 1000}`;
    const stamp = `${process.pid}-${Date.now()}`;
    const wlSocket = `wl-e2e-${stamp}`;
    const weston = spawn(
      "weston",
      ["--backend=headless", "--width=1280", "--height=720", `--socket=${wlSocket}`, "--idle-time=0"],
      { env: { ...process.env, XDG_RUNTIME_DIR: runtime }, stdio: "ignore" },
    );
    await waitFor(() => existsSync(join(runtime, wlSocket)), 10_000, "weston socket");

    const socketPath = `/tmp/saffron-e2e-${stamp}.sock`;
    const proc = spawn(ENGINE_BIN, [], {
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
    const engine = new Engine(proc, weston, socketPath);
    proc.stdout?.on("data", (d) => (engine.buf += d.toString()));
    proc.stderr?.on("data", (d) => (engine.buf += d.toString()));
    proc.on("exit", () => (engine.exited = true));

    await waitFor(() => engine.exited || existsSync(socketPath), 30_000, "control socket");
    if (engine.exited) {
      throw new Error(`engine exited before the control socket appeared:\n${engine.buf}`);
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

  /// Fetch a thumbnail, transparently retrying the `pending` reply the engine sends while its
  /// worker thread generates a cold-cache entry (mirrors the editor's backoff). Resolves the final
  /// PNG reply; rejects on an engine error or after `timeoutMs`.
  async getThumbnail<T = { base64: string; width: number; height: number; format: string }>(
    cmd: "get-thumbnail" | "view-asset",
    params: Record<string, unknown>,
    timeoutMs = 10_000,
  ): Promise<T> {
    const start = Date.now();
    let delayMs = 30;
    for (;;) {
      const reply = await this.call<T & { pending?: boolean }>(cmd, params);
      if (!reply.pending) {
        return reply;
      }
      if (Date.now() - start > timeoutMs) {
        throw new Error(`timeout waiting for ${cmd} to resolve (still pending)`);
      }
      await delay(delayMs);
      delayMs = Math.min(delayMs * 2, 500);
    }
  }

  /// Let the engine run a few render frames so deferred GPU work + validation surface.
  async settle(ms = 300): Promise<void> {
    await delay(ms);
  }

  /// Import a glTF/OBJ as a .smodel asset, then instantiate it into the scene, returning the placed
  /// root entity. The standard "get a model into the scene" path: import bakes the asset, instantiate
  /// places it.
  async importEntity(path: string, name?: string): Promise<{ id: string; name: string }> {
    const model = await this.call<{ id: string }>("import-model", { path });
    const params = name === undefined ? { asset: model.id } : { asset: model.id, name };
    return this.call<{ id: string; name: string }>("instantiate-model", params);
  }

  /// The rig descendant of an instantiated model: a skinned model wraps its node forest under a
  /// container root, so the SkinnedMesh (and the auto-fit BonePhysics) live on a child. Returns the
  /// first entity carrying a SkinnedMesh, falling back to `root` for a non-skinned model.
  async rig(root: string): Promise<string> {
    const { entities } = await this.call<{ entities: { id: string }[] }>("list-entities");
    for (const e of entities) {
      const info = await this.call<{ components: Record<string, unknown> }>("inspect", { entity: e.id });
      if (info.components.SkinnedMesh) {
        return e.id;
      }
    }
    return root;
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
