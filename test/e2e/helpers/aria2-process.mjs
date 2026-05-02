// Spawn a freshly-built aria2c with the same parameter shape Motrix Turbo
// uses in production: BOTH `--save-session` and `--enable-sqlite3-persistence`
// are turned on, so the test exercises the dual-persistence path that real
// users hit.
//
// The CLI flag list intentionally mirrors `Aria2ConfigBuilder.buildArgs`
// (motrix-turbo/src/core/engine/aria2/Aria2ConfigBuilder.ts) — keep them in
// sync so the e2e reproducer stays representative of production.

import { spawn } from 'node:child_process'
import path from 'node:path'
import { fileURLToPath } from 'node:url'
import { mkdir } from 'node:fs/promises'
import { waitForRpc } from './rpc-client.mjs'

const HERE = path.dirname(fileURLToPath(import.meta.url))
const REPO_ROOT = path.resolve(HERE, '..', '..', '..')
const ARIA2_BIN_DEFAULT = path.join(
  REPO_ROOT,
  'build-release',
  'aria2.arm64',
  'aria2c'
)

export function defaultAria2Bin() {
  return process.env.ARIA2_E2E_BIN || ARIA2_BIN_DEFAULT
}

/**
 * Build the argv array. `paths` carries the per-instance file locations so
 * a downloader and seeder can share the same builder while keeping their
 * own session/db/log files.
 */
export function buildArgs({
  rpcPort,
  rpcSecret,
  listenPort,
  dhtListenPort,
  dir,
  sessionPath,
  sqliteDbPath,
  saveSessionInterval = 1,
  logPath,
  logLevel = 'info',
  extra = [],
}) {
  return [
    // engine binding
    '--enable-rpc=true',
    '--rpc-allow-origin-all=true',
    '--rpc-listen-all=false',
    `--rpc-listen-port=${rpcPort}`,
    `--rpc-secret=${rpcSecret}`,
    `--listen-port=${listenPort}`,
    `--dht-listen-port=${dhtListenPort}`,
    // for the e2e we keep DHT/PEX/LPD on; web-seed delivers data either way,
    // but enabling these matches the production parameter envelope and lets
    // sqlite-persistence see the full set of fields it needs to round-trip.
    '--enable-dht=true',
    '--enable-dht6=false',
    '--enable-peer-exchange=true',
    '--bt-enable-lpd=false',
    // dual persistence — exactly the mode motrix-turbo uses today
    `--save-session=${sessionPath}`,
    '--enable-sqlite3-persistence=true',
    `--sqlite3-db-path=${sqliteDbPath}`,
    '--sqlite3-history-limit=-1',
    `--save-session-interval=${saveSessionInterval}`,
    // L1 invariants from Aria2ConfigBuilder
    '--bt-save-metadata=true',
    '--bt-metadata-only=false',
    '--auto-file-renaming=false',
    '--allow-overwrite=false',
    '--rpc-save-upload-metadata=true',
    '--force-save=true',
    '--pause=false',
    '--pause-metadata=false',
    '--bt-seed-unverified=false',
    // sundry knobs that influence sqlite columns
    '--max-concurrent-downloads=5',
    '--bt-max-peers=55',
    '--seed-ratio=0.0',
    '--seed-time=0',
    '--file-allocation=none',
    `--dir=${dir}`,
    `--log=${logPath}`,
    `--log-level=${logLevel}`,
    '--console-log-level=warn',
    ...extra,
  ]
}

export async function spawnAria2({
  bin = defaultAria2Bin(),
  args,
  silent = true,
} = {}) {
  const proc = spawn(bin, args, { stdio: ['ignore', 'pipe', 'pipe'] })
  if (silent) {
    proc.stdout?.on('data', () => {})
    proc.stderr?.on('data', () => {})
  } else {
    proc.stdout?.on('data', (c) => process.stdout.write(c))
    proc.stderr?.on('data', (c) => process.stderr.write(c))
  }
  return proc
}

export async function startInstance(opts) {
  await mkdir(opts.dir, { recursive: true })
  await mkdir(path.dirname(opts.sessionPath), { recursive: true })
  await mkdir(path.dirname(opts.sqliteDbPath), { recursive: true })
  const args = buildArgs(opts)
  const proc = await spawnAria2({ bin: opts.bin, args, silent: opts.silent })
  try {
    await waitForRpc(opts.rpcPort, opts.startupTimeoutMs ?? 10_000)
  } catch (err) {
    if (!proc.killed) proc.kill('SIGKILL')
    throw err
  }
  return {
    proc,
    args,
    rpcPort: opts.rpcPort,
    rpcSecret: opts.rpcSecret,
    stop: (signal = 'SIGTERM', graceMs = 4000) =>
      stopInstance(proc, signal, graceMs),
  }
}

export function stopInstance(proc, signal = 'SIGTERM', graceMs = 4000) {
  return new Promise((resolve) => {
    if (proc.killed || proc.exitCode !== null || proc.signalCode !== null) {
      resolve({ code: proc.exitCode, signal: proc.signalCode })
      return
    }
    const hardKill = setTimeout(() => {
      if (proc.exitCode === null && proc.signalCode === null) {
        proc.kill('SIGKILL')
      }
    }, graceMs)
    hardKill.unref()
    proc.once('exit', (code, sig) => {
      clearTimeout(hardKill)
      resolve({ code, signal: sig })
    })
    proc.kill(signal)
  })
}

let nextPortBase = 19_800 + Math.floor(Math.random() * 1000)
export function allocPorts() {
  const base = nextPortBase
  nextPortBase += 4
  return {
    rpcPort: base,
    listenPort: base + 1,
    dhtListenPort: base + 2,
  }
}
