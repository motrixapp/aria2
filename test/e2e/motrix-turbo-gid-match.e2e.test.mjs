// Simulates Motrix Turbo's `SessionManager.restore()` gid-match path to
// verify whether, given aria2.db preserved across restart, motrix-turbo
// SHOULD be able to adopt the existing aria2 task without re-adding.
//
// If this test passes: the gid-match path in motrix-turbo is sound at the
//   aria2 layer. If motrix-turbo still ends up calling reAddOrMarkError in
//   real usage, the divergence must lie OUTSIDE this match window
//   (e.g. motrix.db not being saved at exit, or a transition-phase reset
//   that triggers TaskRecoveryService instead).
//
// If this test fails: aria2's gid format/reload is unstable, and that's a
//   bug in aria2_motrix that must be fixed regardless of motrix-turbo.

import { describe, it, before, after } from 'node:test'
import assert from 'node:assert/strict'
import { mkdtemp, rm, mkdir, writeFile, readFile } from 'node:fs/promises'
import path from 'node:path'
import os from 'node:os'
import { fileURLToPath } from 'node:url'

import { patchTorrentWithWebSeed } from './helpers/torrent-patch.mjs'
import { startWebSeedServer } from './helpers/webseed-server.mjs'
import { Aria2Rpc } from './helpers/rpc-client.mjs'
import {
  startInstance,
  allocPorts,
  defaultAria2Bin,
} from './helpers/aria2-process.mjs'
import { waitFor, sleep } from './helpers/wait.mjs'

const HERE = path.dirname(fileURLToPath(import.meta.url))
const FIXTURES = path.join(HERE, 'fixtures')

describe('motrix-turbo gid match across restart (sanity)', () => {
  let workDir, webseed, downloaderHandle, rpc
  let dlDir, sessionPath, sqlitePath, logPath, ports
  let patchedTorrentPath

  before(async () => {
    workDir = await mkdtemp(path.join(os.tmpdir(), 'a2-e2e-gidmatch-'))
    webseed = await startWebSeedServer(FIXTURES)
    const patched = await patchTorrentWithWebSeed(
      path.join(FIXTURES, 'single.torrent'),
      webseed.url
    )
    patchedTorrentPath = path.join(workDir, 'single.torrent')
    await writeFile(patchedTorrentPath, patched)

    dlDir = path.join(workDir, 'ubuntu-like.iso.motrix')
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
    try { await downloaderHandle?.stop('SIGTERM') } catch {}
    try { await webseed?.close() } catch {}
    if (process.env.ARIA2_E2E_KEEP_TMP) {
      console.log(`workDir kept: ${workDir}`)
    } else if (workDir) {
      await rm(workDir, { recursive: true, force: true })
    }
  })

  it('after restart, the SAME gid appears in tellWaiting() — match succeeds', async () => {
    // Round 1: add → progress → pause → save → SIGTERM
    const buf = await readFile(patchedTorrentPath)
    const motrixSavedGid = await rpc.addTorrent(buf.toString('base64'), {
      dir: dlDir,
      pause: 'false',
      'seed-time': '0',
      'seed-ratio': '0.0',
      'max-download-limit': '64K',
    })
    console.log(`[diag] motrix.db would persist engineTaskId=${motrixSavedGid}`)

    await waitFor(
      async () => {
        const s = await rpc.tellStatus(motrixSavedGid, [
          'status',
          'completedLength',
        ])
        return s.status === 'active' && Number(s.completedLength) > 0
          ? s
          : null
      },
      30_000,
      200,
      'first progress'
    )
    await rpc.forcePause(motrixSavedGid)
    await waitFor(
      async () =>
        (await rpc.tellStatus(motrixSavedGid, ['status'])).status === 'paused',
      5_000,
      100,
      'paused'
    )
    try { await rpc.saveSession() } catch {}
    await sleep(1_500)
    await downloaderHandle.stop('SIGTERM', 8_000)

    // Restart aria2 with the same db / session paths — analogous to
    // motrix-turbo restarting and supervisor.start()'ing a fresh aria2.
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

    // Reproduce SessionManager.restore() exactly:
    //   1. tellActive + tellWaiting + tellStopped
    //   2. build aria2ByGid map
    //   3. look up motrix.db's saved gid
    const [active, waiting, stopped] = await Promise.all([
      rpc.tellActive(['gid', 'status', 'bittorrent']),
      rpc.tellWaiting(0, 1000, ['gid', 'status', 'bittorrent']),
      rpc.tellStopped(0, 1000, ['gid', 'status', 'bittorrent']),
    ])
    const aria2Tasks = [...active, ...waiting, ...stopped]
    const aria2ByGid = new Map(aria2Tasks.map((t) => [t.gid, t]))
    console.log(
      `[diag] aria2 lists: active=${active.length} waiting=${waiting.length} stopped=${stopped.length}`
    )
    console.log(
      `[diag] aria2 gids:`,
      aria2Tasks.map((t) => t.gid).join(', ')
    )

    const matched = aria2ByGid.get(motrixSavedGid)
    assert.ok(
      matched,
      `motrix-turbo would have failed to match motrix.db.engineTaskId=${motrixSavedGid} against aria2's task list — this is what makes it fall through to reAddOrMarkError`
    )
    assert.equal(matched.gid, motrixSavedGid, 'gid hex must match exactly')
    console.log(
      `[diag] matched OK: gid=${matched.gid} status=${matched.status}`
    )
  })
})
