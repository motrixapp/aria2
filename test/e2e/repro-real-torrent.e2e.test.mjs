// Direct reproduction of the user-reported bug using
// `motrix-turbo/demo/ubuntu-25.10-desktop-amd64.iso.torrent`.
//
// Differs from `persistence-restart.e2e.test.mjs` in three ways:
//   1. Uses a REAL torrent (single-file format, ~5 GB) — no web-seed patch.
//   2. Mimics Motrix Turbo's `.motrix` in-flight container by setting
//      aria2 `dir` to `<workdir>/<finalname>.motrix`.
//   3. Relies on real BT peers (DHT/PEX/trackers) — gated behind the env
//      var `RUN_REAL_BT=1` so it never runs by accident in CI.
//
// Set `RUN_REAL_BT=1` to opt in:
//   RUN_REAL_BT=1 node --test --test-timeout=300000 \
//     test/e2e/repro-real-torrent.e2e.test.mjs

import { describe, it, before, after } from 'node:test'
import assert from 'node:assert/strict'
import { mkdtemp, rm, mkdir, readFile } from 'node:fs/promises'
import path from 'node:path'
import os from 'node:os'
import { fileURLToPath } from 'node:url'

import { Aria2Rpc } from './helpers/rpc-client.mjs'
import { startInstance, allocPorts, defaultAria2Bin } from './helpers/aria2-process.mjs'
import { waitFor, sleep } from './helpers/wait.mjs'

const HERE = path.dirname(fileURLToPath(import.meta.url))
const TORRENT = path.join(HERE, 'fixtures', 'ubuntu.torrent')
const FINAL_NAME = 'ubuntu-25.10-desktop-amd64.iso'

const SHOULD_RUN = process.env.RUN_REAL_BT === '1'

describe.skip = !SHOULD_RUN
describe('real-torrent restart reproduction (RUN_REAL_BT=1 to opt in)', () => {
  if (!SHOULD_RUN) {
    it('skipped — set RUN_REAL_BT=1 to enable', () => {})
    return
  }

  let workDir
  let downloaderHandle
  let rpc
  let sessionPath
  let sqlitePath
  let dlDir
  let logPath
  let ports
  let gid

  before(async () => {
    workDir = await mkdtemp(path.join(os.tmpdir(), 'a2-e2e-real-bt-'))
    // Reproduce Motrix Turbo's `.motrix` in-flight container shape:
    //   <workdir>/<final-name>.motrix/
    dlDir = path.join(workDir, `${FINAL_NAME}.motrix`)
    sessionPath = path.join(workDir, 'aria2.session')
    sqlitePath = path.join(workDir, 'aria2.db')
    logPath = path.join(workDir, 'aria2.log')
    await mkdir(dlDir, { recursive: true })

    ports = allocPorts()
    downloaderHandle = await startInstance({
      bin: defaultAria2Bin(),
      rpcPort: ports.rpcPort,
      rpcSecret: 'e2e_secret',
      listenPort: ports.listenPort,
      dhtListenPort: ports.dhtListenPort,
      dir: dlDir,
      sessionPath,
      sqliteDbPath: sqlitePath,
      saveSessionInterval: 1,
      logPath,
      silent: true,
    })
    rpc = new Aria2Rpc({ port: ports.rpcPort, secret: 'e2e_secret' })
  })

  after(async () => {
    try {
      await downloaderHandle?.stop('SIGTERM')
    } catch {}
    if (process.env.ARIA2_E2E_KEEP_TMP) {
      console.log(`workDir kept: ${workDir}`)
    } else if (workDir) {
      await rm(workDir, { recursive: true, force: true })
    }
  })

  it('add → 10s download → pause → resume → pause → restart → resume must NOT enter error', async () => {
    const buf = await readFile(TORRENT)
    gid = await rpc.addTorrent(buf.toString('base64'), {
      dir: dlDir,
      pause: 'false',
      'seed-time': '0',
      'seed-ratio': '0.0',
      'max-download-limit': '256K',
    })
    console.log(`[diag] added gid=${gid}`)

    const beforePause = await waitFor(
      async () => {
        const s = await rpc.tellStatus(gid, [
          'status',
          'completedLength',
          'totalLength',
          'downloadSpeed',
          'errorCode',
        ])
        if (s.status === 'active' && Number(s.completedLength) > 0) return s
        return null
      },
      60_000,
      500,
      'first active+progress (real BT — needs peers)'
    )
    console.log(
      `[diag] pre-pause completed=${beforePause.completedLength}/${beforePause.totalLength} speed=${beforePause.downloadSpeed}`
    )

    // 10s of real download to mirror the user's report
    await sleep(10_000)
    const afterTen = await rpc.tellStatus(gid, [
      'status',
      'completedLength',
      'downloadSpeed',
    ])
    console.log(
      `[diag] +10s completed=${afterTen.completedLength} speed=${afterTen.downloadSpeed}`
    )

    // Pause / resume / pause cycle (use plain `aria2.pause`, NOT forcePause,
    // to match what Motrix Turbo issues in production).
    await rpc.pause(gid)
    await waitFor(
      async () => (await rpc.tellStatus(gid, ['status'])).status === 'paused',
      10_000,
      200,
      'first pause'
    )
    await rpc.unpause(gid)
    await waitFor(
      async () => {
        const st = (await rpc.tellStatus(gid, ['status'])).status
        return st === 'active' || st === 'waiting'
      },
      10_000,
      200,
      'first resume'
    )
    await rpc.pause(gid)
    await waitFor(
      async () => (await rpc.tellStatus(gid, ['status'])).status === 'paused',
      10_000,
      200,
      'second pause'
    )

    // Flush + SIGTERM
    try {
      await rpc.saveSession()
    } catch {}
    await sleep(2_000)
    const exit = await downloaderHandle.stop('SIGTERM', 10_000)
    console.log(`[diag] aria2 exit:`, JSON.stringify(exit))

    // Restart
    downloaderHandle = await startInstance({
      bin: defaultAria2Bin(),
      rpcPort: ports.rpcPort,
      rpcSecret: 'e2e_secret',
      listenPort: ports.listenPort,
      dhtListenPort: ports.dhtListenPort,
      dir: dlDir,
      sessionPath,
      sqliteDbPath: sqlitePath,
      saveSessionInterval: 1,
      logPath: logPath + '.2',
      silent: true,
    })
    rpc = new Aria2Rpc({ port: ports.rpcPort, secret: 'e2e_secret' })

    const reloaded = await waitFor(
      async () => {
        try {
          return await rpc.tellStatus(gid, [
            'gid',
            'status',
            'errorCode',
            'errorMessage',
          ])
        } catch {
          return null
        }
      },
      15_000,
      200,
      'task reload after restart'
    )
    console.log(`[diag] post-restart:`, JSON.stringify(reloaded))
    assert.equal(reloaded.gid, gid, 'gid must match across restart')
    assert.equal(reloaded.status, 'paused', 'must be paused after restart')

    // The critical step: unpause and observe over 10 s. THIS is where the
    // user-reported errorCode=13 manifests.
    await rpc.unpause(gid)
    const observed = []
    const deadline = Date.now() + 10_000
    let firstError = null
    while (Date.now() < deadline) {
      let s
      try {
        s = await rpc.tellStatus(gid, [
          'status',
          'errorCode',
          'errorMessage',
          'completedLength',
        ])
      } catch (err) {
        const stopped = await rpc.tellStopped(0, 100, [
          'gid',
          'status',
          'errorCode',
          'errorMessage',
        ])
        const me = stopped.find((t) => t.gid === gid)
        if (me) {
          observed.push(`stopped:${me.status}:${me.errorCode}`)
          if (me.status === 'error') {
            firstError = me
            break
          }
        }
        break
      }
      observed.push(`${s.status}:${s.errorCode ?? '-'}@${s.completedLength}`)
      if (s.status === 'error' || (s.errorCode && s.errorCode !== '0')) {
        firstError = s
        break
      }
      if (s.status === 'complete' || s.status === 'completed') break
      await sleep(200)
    }
    console.log(`[diag] post-resume observed:`, observed.slice(0, 30).join(' | '))
    assert.equal(
      firstError,
      null,
      `task entered error after post-restart resume: ${JSON.stringify(
        firstError
      )}`
    )
  })
})
