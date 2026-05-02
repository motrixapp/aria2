// Regression for the "GID is not found" failure that surfaces when an
// active BT task is killed by SIGTERM without a prior pause.
//
// Before the fix:
//   1. Periodic SaveSessionCommand wrote the active RG into the `task`
//      table with state="waiting".
//   2. Inside DownloadEngine::onEndOfRun, removeStoppedGroup() drained
//      every active RG into `downloadResults_`, mid-shutdown.
//   3. MultiUrlRequestInfo's final `saveAllTasks(rgman)` call iterated
//      ONLY the (now empty) active+reserved lists, so liveGids was empty,
//      and `removeOrphanTasks(liveGids)` deleted EVERY task row — even
//      with `--force-save=true` set.
//   4. After restart, aria2 logged "restored 0 active task(s) from db".
//      motrix-turbo's saved gid pointed at nothing → resume → "GID is
//      not found".
//
// After the fix: `Sqlite3SessionStore::saveAllTasks` adds the gids of
// `getDownloadResults()` and `getUnfinishedDownloadResult()` into
// liveGids before orphan-removal, so the in-flight tasks halted by
// SIGTERM survive into the next aria2 process.

import { describe, it, before, after } from 'node:test'
import assert from 'node:assert/strict'
import { mkdtemp, rm, writeFile, readFile } from 'node:fs/promises'
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

describe('active task survives SIGTERM-without-pause across aria2 restart', () => {
  let workDir, webseed, downloaderHandle, rpc
  let dlDir, sessionPath, sqlitePath, logPath, ports
  let patchedTorrentPath

  before(async () => {
    workDir = await mkdtemp(path.join(os.tmpdir(), 'a2-e2e-nopause-'))
    webseed = await startWebSeedServer(FIXTURES)
    const patched = await patchTorrentWithWebSeed(
      path.join(FIXTURES, 'single.torrent'),
      webseed.url
    )
    patchedTorrentPath = path.join(workDir, 'single.torrent')
    await writeFile(patchedTorrentPath, patched)

    dlDir = path.join(workDir, 'download.motrix')
    sessionPath = path.join(workDir, 'aria2.session')
    sqlitePath = path.join(workDir, 'aria2.db')
    logPath = path.join(workDir, 'aria2.log')

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

  it('add → progress → SIGTERM (no pause) → restart preserves task and resumes cleanly', async () => {
    const buf = await readFile(patchedTorrentPath)
    const gid = await rpc.addTorrent(buf.toString('base64'), {
      dir: dlDir,
      pause: 'false',
      'seed-time': '0',
      'seed-ratio': '0.0',
      'max-download-limit': '64K',
    })
    console.log(`[diag] gid=${gid}`)

    await waitFor(
      async () => {
        const s = await rpc.tellStatus(gid, ['status', 'completedLength'])
        return s.status === 'active' && Number(s.completedLength) > 0
          ? s
          : null
      },
      30_000,
      200,
      'first progress'
    )

    // Crucial: do NOT pause. Just stop aria2 cleanly. The kernel sends
    // SIGTERM and aria2 runs onEndOfRun → removeStoppedGroup, which is
    // exactly what motrix-turbo's gracefulStop produces when the user
    // quits the app while a task is downloading.
    const exit = await downloaderHandle.stop('SIGTERM', 8_000)
    console.log(`[diag] aria2 exit:`, JSON.stringify(exit))

    // Restart with the SAME db / session paths.
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

    // The task must still be addressable — pre-fix this raised
    // "GID ... is not found". Both tellStatus and the active+waiting
    // lists must include it.
    const reloaded = await waitFor(
      async () => {
        try {
          const s = await rpc.tellStatus(gid, ['gid', 'status', 'errorCode'])
          return s
        } catch {
          return null
        }
      },
      10_000,
      200,
      'reload'
    )
    assert.equal(reloaded.gid, gid, 'gid must survive restart')
    assert.notEqual(
      reloaded.status,
      undefined,
      `task must reappear in some list, got ${JSON.stringify(reloaded)}`
    )
    console.log(
      `[diag] post-restart gid=${reloaded.gid} status=${reloaded.status}`
    )

    // The grand-prize check: motrix-turbo would call unpause to resume.
    // For an already-active reload, this returns "cannot be unpaused
    // now" which is fine; what matters is "GID not found" is impossible.
    try {
      await rpc.unpause(gid)
    } catch (err) {
      assert.doesNotMatch(
        String(err.message),
        /not found/i,
        `unpause must not raise "not found"; got: ${err.message}`
      )
    }
  })
})
