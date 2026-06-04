#!/usr/bin/env bun
// Contract test: launches a headless SaffronEngine, drives `se`-style control commands
// over the unix socket, and validates each result against schemas/control/*.schema.json.
//
// There are no named C++ DTO structs (every engine response is an ad-hoc nlohmann::json
// literal), so this test is the ONLY drift guard between the wire and the schemas the TS
// protocol is generated from. It also asserts the u64-id invariant: ids are emitted as
// quoted decimal strings (uuid.schema.json), so a JS client's plain JSON.parse keeps full
// precision even when an id exceeds 2^53.
//
// Zero runtime dependencies (a compact embedded validator) so it runs in the saffron-build
// toolbox without a network/`bun install`. The engine needs a display to open its swapchain;
// run this under a compositor, e.g. weston headless with WAYLAND_DISPLAY + SDL_VIDEODRIVER.

import { readFileSync, existsSync, mkdtempSync, rmSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join, dirname } from 'node:path'
import { fileURLToPath } from 'node:url'
import net from 'node:net'

const HERE = dirname(fileURLToPath(import.meta.url))
const REPO = join(HERE, '..', '..')
const SCHEMA_DIR = join(REPO, 'schemas', 'control')
const ENGINE = process.env.SAFFRON_ENGINE_BIN ?? join(REPO, 'build', 'debug', 'bin', 'SaffronEngine')
const SOCK = process.env.SAFFRON_CONTROL_SOCK ?? `/tmp/saffron-contract-${process.pid}.sock`
const APPDATA = process.env.SAFFRON_APPDATA_DIR ?? mkdtempSync(join(tmpdir(), 'saffron-contract-appdata.'))

// ---- compact JSON Schema (subset) validator ---------------------------------
const schemaCache = new Map<string, any>()
function loadSchema(file: string): any {
  if (!schemaCache.has(file)) {
    schemaCache.set(file, JSON.parse(readFileSync(join(SCHEMA_DIR, file), 'utf8')))
  }
  return schemaCache.get(file)
}
function typeOk(v: any, t: string): boolean {
  switch (t) {
    case 'object': return v !== null && typeof v === 'object' && !Array.isArray(v)
    case 'array': return Array.isArray(v)
    case 'string': return typeof v === 'string'
    case 'number': return typeof v === 'number'
    case 'integer': return typeof v === 'number' && Number.isInteger(v)
    case 'boolean': return typeof v === 'boolean'
    case 'null': return v === null
    default: return false
  }
}
function validate(schema: any, value: any, path: string, errors: string[]): void {
  if (schema.$ref) { validate(loadSchema(schema.$ref), value, path, errors); return }
  if (schema.oneOf) {
    const passes = schema.oneOf.filter((s: any) => { const e: string[] = []; validate(s, value, path, e); return e.length === 0 })
    if (passes.length !== 1) errors.push(`${path}: matched ${passes.length} of oneOf (expected 1)`)
    return
  }
  if (schema.const !== undefined && value !== schema.const) errors.push(`${path}: expected const ${JSON.stringify(schema.const)}, got ${JSON.stringify(value)}`)
  if (schema.enum && !schema.enum.includes(value)) errors.push(`${path}: ${JSON.stringify(value)} not in enum ${JSON.stringify(schema.enum)}`)
  if (schema.type) {
    const types = Array.isArray(schema.type) ? schema.type : [schema.type]
    if (!types.some((t: string) => typeOk(value, t))) {
      errors.push(`${path}: expected type ${types.join('|')}, got ${value === null ? 'null' : Array.isArray(value) ? 'array' : typeof value}`)
      return
    }
  }
  if (typeof schema.minimum === 'number' && typeof value === 'number' && value < schema.minimum) errors.push(`${path}: ${value} < minimum ${schema.minimum}`)
  if (typeOk(value, 'object') && schema.properties) {
    for (const key of schema.required ?? []) if (!(key in value)) errors.push(`${path}: missing required '${key}'`)
    for (const [key, sub] of Object.entries<any>(schema.properties)) if (key in value) validate(sub, value[key], `${path}.${key}`, errors)
    if (schema.additionalProperties === false) {
      for (const key of Object.keys(value)) if (!(key in schema.properties)) errors.push(`${path}: unexpected property '${key}'`)
    }
  }
  if (typeOk(value, 'array') && schema.items) value.forEach((item: any, i: number) => validate(schema.items, item, `${path}[${i}]`, errors))
}

// ---- u64-on-the-wire assertion (raw bytes, pre-parse) ------------------------
// Confirms every id in the result payload is emitted as a quoted decimal string, so a JS
// client's JSON.parse keeps full precision even past 2^53. A bare numeric (or float) token
// is a regression: JSON.parse would silently truncate it. Scans only the result portion so
// the envelope's numeric request-id echo isn't mistaken for an entity id.
function assertRawU64(raw: string, label: string, errors: string[]): void {
  const result = raw.slice(raw.indexOf('"result"'))
  for (const m of result.matchAll(/"(?:id|mesh|albedoTexture|skyTexture)"\s*:\s*([^,}\s]+)/g)) {
    const tok = m[1]
    const quoted = /^"(\d+)"$/.exec(tok)
    if (!quoted) { errors.push(`${label}: id token '${tok}' is not a quoted decimal string (an unquoted u64 loses precision through JSON.parse)`); continue }
    const digits = quoted[1]
    if (BigInt(digits).toString() !== digits) errors.push(`${label}: id token '${tok}' did not round-trip as BigInt`)
  }
}

// ---- unix socket control client ----------------------------------------------
let nextId = 1
function call(cmd: string, params: Record<string, unknown> = {}): Promise<{ envelope: any; raw: string }> {
  return new Promise((resolve, reject) => {
    const socket = net.connect({ path: SOCK })
    let buf = ''
    const timer = setTimeout(() => { socket.destroy(); reject(new Error(`timeout calling ${cmd}`)) }, 8000)
    socket.on('connect', () => socket.write(JSON.stringify({ id: nextId++, cmd, params }) + '\n'))
    socket.on('data', (d) => {
      buf += d.toString('utf8')
      const nl = buf.indexOf('\n')
      if (nl >= 0) { clearTimeout(timer); socket.end(); const raw = buf.slice(0, nl); resolve({ envelope: JSON.parse(raw), raw }) }
    })
    socket.on('error', (e) => { clearTimeout(timer); reject(e) })
  })
}
const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms))

// Extract the first id inside the result payload as a string. Ids are quoted decimal
// strings now, so a plain JSON.parse would also be lossless; the raw-bytes match keeps the
// extraction independent of envelope shape. Passing the string id back round-trips through
// resolveEntity exactly as a JS client would.
function firstResultId(raw: string): string | undefined {
  return raw.match(/"result"\s*:\s*[[{][\s\S]*?"id"\s*:\s*"(\d+)"/)?.[1]
}

// ---- run ---------------------------------------------------------------------
async function main(): Promise<number> {
  if (!existsSync(ENGINE)) { console.error(`engine binary not found: ${ENGINE}`); return 2 }
  const proc = Bun.spawn([ENGINE], {
    cwd: HERE,
    env: {
      ...process.env,
      SAFFRON_CONTROL_SOCK: SOCK,
      SAFFRON_APPDATA_DIR: APPDATA,
      SAFFRON_AUTO_EMPTY_PROJECT: '1',
    },
    stdout: 'ignore',
    stderr: 'ignore',
  })

  // Wait for the socket to bind (the engine must reach its first frame).
  let up = false
  for (let i = 0; i < 60 && !up; i++) {
    if (proc.exitCode !== null) { console.error(`engine exited early (code ${proc.exitCode})`); return 2 }
    if (existsSync(SOCK)) { try { await call('ping'); up = true } catch { /* not ready */ } }
    if (!up) await sleep(500)
  }
  if (!up) { proc.kill(); console.error('engine control socket never came up'); return 2 }

  const errors: string[] = []
  const checked: string[] = []
  const expect = async (cmd: string, params: Record<string, unknown>, schemaFile: string): Promise<string | undefined> => {
    const { envelope, raw } = await call(cmd, params)
    if (envelope.ok !== true) { errors.push(`${cmd}: ok=${envelope.ok} error=${envelope.error}`); return undefined }
    validate(loadSchema(schemaFile), envelope.result, cmd, errors)
    assertRawU64(raw, cmd, errors)
    checked.push(`${cmd} -> ${schemaFile}`)
    return raw  // raw bytes, for lossless id extraction by the caller
  }

  try {
    // Seed an entity so id-dependent fixtures have a target. ids are pulled from the raw
    // bytes as strings (never JSON.parse'd into a lossy JS number).
    const cubeRaw = await expect('add-entity', { preset: 'cube' }, 'entity-ref.schema.json')
    const cubeId = cubeRaw ? firstResultId(cubeRaw) : undefined
    await expect('get-project', {}, 'project-info.schema.json')
    await expect('save-project', {}, 'project-info.schema.json')
    await expect('list-entities', {}, 'entity-list.schema.json')
    await expect('get-selection', {}, 'selection.schema.json')
    if (cubeId !== undefined) {
      await expect('inspect', { entity: cubeId }, 'inspect-result.schema.json')
      await expect('rename-entity', { entity: cubeId, name: 'Renamed Cube' }, 'entity-ref.schema.json')
      await expect('copy-entity', { entity: cubeId }, 'entity-ref.schema.json')
    }
    await expect('get-gizmo', {}, 'gizmo-state.schema.json')
    await expect('set-gizmo', { op: 'rotate', space: 'local' }, 'gizmo-state.schema.json')
    await expect('get-camera', {}, 'editor-camera.schema.json')
    await expect('set-camera', { yaw: 12 }, 'editor-camera.schema.json')
    await expect('render-stats', {}, 'render-stats.schema.json')
    await expect('get-environment', {}, 'environment.schema.json')
    const assetsRaw = await expect('list-assets', {}, 'asset-list.schema.json')
    const meshId = assetsRaw ? firstResultId(assetsRaw) : undefined  // seeded catalog = the cube mesh
    if (meshId) await expect('get-thumbnail', { asset: meshId }, 'thumbnail.schema.json')
    else errors.push('list-assets: no mesh asset to thumbnail')
    const second = `contract-second-${process.pid}`
    await expect('new-project', { name: second, displayName: 'Contract Second Project' }, 'project-info.schema.json')
    await expect('open-project', { path: second }, 'project-info.schema.json')

    // The error envelope: a bad command must come back ok:false with a string error.
    const bad = await call('definitely-not-a-command')
    validate(loadSchema('envelope.schema.json'), bad.envelope, 'bad-command', errors)
    if (bad.envelope.ok !== false || typeof bad.envelope.error !== 'string') errors.push('bad-command: expected ok:false + string error')
    else checked.push('bad-command -> envelope (ok:false)')

    await call('quit').catch(() => {})
  } finally {
    proc.kill()
    if (process.env.SAFFRON_APPDATA_DIR === undefined) rmSync(APPDATA, { recursive: true, force: true })
  }

  for (const c of checked) console.log(`  ok  ${c}`)
  if (errors.length) { console.error(`\n${errors.length} contract failure(s):`); for (const e of errors) console.error(`  FAIL ${e}`); return 1 }
  console.log(`\nall ${checked.length} control contracts validated against schemas/control/`)
  return 0
}

main().then((code) => process.exit(code)).catch((e) => { console.error(e); process.exit(2) })
