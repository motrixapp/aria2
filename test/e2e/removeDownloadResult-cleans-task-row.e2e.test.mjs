// Regression for the "phantom Error row after restart" failure that
// motrix-turbo's finalizeBt path triggered.
//
// Before the fix:
//   1. motrix-turbo addTorrent → aria2 marks G1 active, downloads.
//   2. Periodic SaveSessionCommand upserts G1 into `task` table.
//   3. motrix-turbo finalizeBt fires forceRemove(G1) — only sets
//      haltRequested; the active→stopped transition runs on the next
//      event-loop iteration, going through ProcessStoppedRequestGroup.
//      That path SKIPS deleteTask when `--force-save=true` (the
//      product-contract invariant in motrix-turbo's Aria2ConfigBuilder).
//   4. motrix-turbo finalizeBt fires removeDownloadResult(G1).
//      RequestGroupMan::removeDownloadResult cleared in-memory
//      downloadResults_ and download_history, but NEVER touched the
//      `task` table.
//   5. The G1 row stays in `task`. saveAllTasks's orphan-removal
//      should clean it up at the next periodic save (15s default),
//      but if the user quits within that window (or the racing
//      transition adds G1 back into downloadResults_, where the
//      saveAllTasks fix puts it into liveGids), G1 survives across
//      restart.
//   6. Restart: aria2 reads G1 from `task` table, fails to load
//      (data dir was renamed away by motrix-turbo's finalize), enters
//      error state. tellStopped reports G1 with no infoHash.
//      motrix-turbo's restore() can't match G1 to any sidecar via
//      gid/infoHash/uriHash → adoptTask() creates a phantom Error row.
//      The user sees Error + Seeding instead of just Seeding.
//
// After the fix:
//   • RequestGroupMan::removeDownloadResult also calls
//     Sqlite3SessionStore::deleteTask(gid).
//   • ProcessStoppedRequestGroup also deletes the row when
//     `--force-save=true` AND haltReason == USER_REQUEST (i.e., the
//     user's RPC remove/forceRemove caused the transition).

import { describe, it, before, after } from 'node:test'
import assert from 'node:assert/strict'
import { mkdtemp, rm, writeFile, readFile } from 'node:fs/promises'
import path from 'node:path'
import os from 'node:os'
import { fileURLToPath } from 'node:url'
import { execFileSync } from 'node:child_process'

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

function sqliteQuery(dbPath, sql) {
  // -readonly avoids any accidental write lock that could collide with
  // aria2's own connection. Aria2's WAL mode lets concurrent readers in.
  return execFileSync('sqlite3', ['-readonly', dbPath, sql], {
    encoding: 'utf8',
  }).trim()
}

function countTaskRows(sqlitePath, gid) {
  const out = sqliteQuery(
    sqlitePath,
    `SELECT COUNT(*) FROM task WHERE gid = '${gid}';`
  )
  return Number(out)
}

function listAllTaskRows(sqlitePath) {
  const out = sqliteQuery(sqlitePath, 'SELECT gid, state FROM task;')
  if (!out) return []
  return out.split('\n').map((line) => {
    const [gid, state] = line.split('|')
    return { gid, state }
  })
}

describe('removeDownloadResult deletes the persisted `task` row', () => {
  let workDir, webseed, downloaderHandle, rpc
  let dlDir, sessionPath, sqlitePath, logPath, ports
  let patchedTorrentPath

  before(async () => {
    workDir = await mkdtemp(path.join(os.tmpdir(), 'a2-e2e-rmresult-'))
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
      // Long save-session-interval so the periodic saveAllTasks
      // orphan-removal pass cannot mask a missing deleteTask:
      // a passing test must therefore demonstrate that
      // removeDownloadResult itself cleans the row.
      saveSessionInterval: 600,
      logPath,
      silent: true,
    })
    rpc = new Aria2Rpc({ port: ports.rpcPort, secret: 'e2e_secret' })
  })

  after(async () => {
    try {
      await downloaderHandle?.stop('SIGTERM')
    } catch {}
    try {
      await webseed?.close()
    } catch {}
    if (process.env.ARIA2_E2E_KEEP_TMP) {
      console.log(`workDir kept: ${workDir}`)
    } else if (workDir) {
      await rm(workDir, { recursive: true, force: true })
    }
  })

  it('forceRemove + removeDownloadResult purges the `task` row even with --force-save=true', async () => {
    const buf = await readFile(patchedTorrentPath)
    const gid = await rpc.addTorrent(buf.toString('base64'), {
      dir: dlDir,
      pause: 'false',
      'seed-time': '0',
      'seed-ratio': '0.0',
      'max-download-limit': '64K',
    })
    console.log(`[diag] added gid=${gid}`)

    // Wait until the gid is actually active (was on reserved list at
    // first), then force one explicit saveAllTasks via saveSession RPC.
    // We deliberately disabled the periodic save (interval=600s) so the
    // delete assertion below cannot be masked by orphan-removal — only
    // removeDownloadResult itself can erase the row.
    await waitFor(
      async () => {
        const s = await rpc.tellStatus(gid, ['status'])
        return s.status === 'active' ? s : null
      },
      10_000,
      200,
      'task active'
    )
    await rpc.saveSession()
    await waitFor(
      () => (countTaskRows(sqlitePath, gid) === 1 ? true : null),
      5_000,
      100,
      'task row upserted by saveSession RPC'
    )
    console.log(`[diag] task row present pre-remove`)

    // Mirror motrix-turbo's finalizeBt cleanup sequence:
    //   forceRemove(gid)        — sets haltRequested = USER_REQUEST
    //   removeDownloadResult    — purges the gid
    await rpc.forceRemove(gid)
    // Brief sleep to let the active→stopped transition land. Without
    // this, removeDownloadResult below can race ahead of the transition
    // and the phantom-row scenario surfaces *after* this test exits.
    // The fix targets BOTH orderings (race-before and race-after) — we
    // assert one ordering here and let the SIGTERM-restart phase catch
    // any regression in the other.
    await sleep(200)
    try {
      await rpc.removeDownloadResult(gid)
    } catch (err) {
      // OK if the result row is already gone (idempotent).
      assert.match(
        String(err.message),
        /Could not remove download result|not found/i,
        `unexpected RPC error: ${err.message}`
      )
    }

    // The bug surface: with `--force-save=true` the row would survive
    // here. After the fix it must be gone — and because we disabled the
    // periodic saveAllTasks (interval=600s), no orphan-removal pass can
    // mask a missing deleteTask. The pre-fix binary fails this assertion.
    const remaining = countTaskRows(sqlitePath, gid)
    assert.equal(
      remaining,
      0,
      `task row must be deleted by removeDownloadResult itself; ` +
        `still ${remaining} row(s); table=${JSON.stringify(listAllTaskRows(sqlitePath))}`
    )
    console.log(`[diag] task row cleaned post-remove`)

    // Belt-and-suspenders: the `--force-save=true` orphan also cannot
    // resurrect on restart. Stop and start aria2 with the same db, then
    // assert the gid is genuinely gone (tellStatus must throw).
    await downloaderHandle.stop('SIGTERM', 8_000)

    downloaderHandle = await startInstance({
      bin: defaultAria2Bin(),
      rpcPort: ports.rpcPort,
      rpcSecret: 'e2e_secret',
      listenPort: ports.listenPort,
      dhtListenPort: ports.dhtListenPort,
      dir: dlDir,
      sessionPath,
      sqliteDbPath: sqlitePath,
      saveSessionInterval: 600,
      logPath: logPath + '.2',
      silent: true,
    })
    rpc = new Aria2Rpc({ port: ports.rpcPort, secret: 'e2e_secret' })

    // tellStatus must report "GID not found": the row in `task` is
    // gone, no resurrection.
    await assert.rejects(
      rpc.tellStatus(gid),
      /not found/i,
      'restored gid must not exist after removeDownloadResult'
    )

    const tableAfterRestart = listAllTaskRows(sqlitePath)
    assert.equal(
      tableAfterRestart.length,
      0,
      `task table must be empty after restart, got: ${JSON.stringify(tableAfterRestart)}`
    )
  })
})
