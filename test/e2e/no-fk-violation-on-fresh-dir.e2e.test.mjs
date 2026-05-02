// Regression for the FOREIGN KEY violation that surfaces when
// `aria2.addTorrent` is given a `dir` that does not yet exist on disk.
//
// Before the fix:
//   1. aria2 calls `util::saveAs(<dir>/<sha1>.torrent, ...)` which fails
//      because the parent directory does not exist.
//   2. The task is created with a data-only `MetadataInfo`.
//   3. `Sqlite3SessionStore::upsertTask` invokes `SessionSerializer::renderOne`
//      which short-circuits to empty when `mi->dataOnly()` is true, so no
//      row ever lands in the `task` table.
//   4. The user pauses the task, `Sqlite3BtProgressInfoFile::save` runs,
//      and the UPSERT into `task_progress` violates the FOREIGN KEY:
//      `task_progress.gid → task.gid` (no parent row).
//
// After the fix: `AddTorrentRpcMethod::process` mkdirs `--dir` before the
// torrent metadata save, so saveAs succeeds, the task gets a proper
// `MetadataInfo`, and the periodic sqlite save writes both `task` and
// `task_progress` rows in the correct order.

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

describe('no FK violation on addTorrent + pause when dir does not pre-exist', () => {
  let workDir, webseed, downloaderHandle, rpc
  let dlDir, sessionPath, sqlitePath, logPath, ports
  let patchedTorrentPath

  before(async () => {
    workDir = await mkdtemp(path.join(os.tmpdir(), 'a2-e2e-fk-'))
    webseed = await startWebSeedServer(FIXTURES)
    const patched = await patchTorrentWithWebSeed(
      path.join(FIXTURES, 'single.torrent'),
      webseed.url
    )
    patchedTorrentPath = path.join(workDir, 'single.torrent')
    await writeFile(patchedTorrentPath, patched)

    // Crucial: `dlDir` does NOT exist when aria2 starts. Mirrors Motrix
    // Turbo's `<finalPath>.motrix` which is constructed as a string and
    // not pre-created prior to addTorrent.
    dlDir = path.join(workDir, 'ubuntu-like.iso.motrix')
    sessionPath = path.join(workDir, 'aria2.session')
    sqlitePath = path.join(workDir, 'aria2.db')
    logPath = path.join(workDir, 'aria2.log')
    // NOTE: `dlDir` intentionally NOT mkdired.

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

  it('addTorrent → progress → pause does NOT raise UPSERT task_progress FK error', async () => {
    const buf = await readFile(patchedTorrentPath)
    const gid = await rpc.addTorrent(buf.toString('base64'), {
      dir: dlDir,
      pause: 'false',
      'seed-time': '0',
      'seed-ratio': '0.0',
      'max-download-limit': '64K',
    })
    console.log(`[diag] gid=${gid} dir=${dlDir} (did not pre-exist)`)

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
    await rpc.forcePause(gid)
    await waitFor(
      async () =>
        (await rpc.tellStatus(gid, ['status'])).status === 'paused',
      5_000,
      100,
      'paused'
    )

    // Give aria2's periodic SaveSessionCommand and the pause-time
    // saveControlFile a moment to flush, then read the log.
    await sleep(2_000)
    const logText = await readFile(logPath, 'utf8')
    const fkLine = logText
      .split(/\r?\n/)
      .find((line) => line.includes('FOREIGN KEY constraint failed'))
    assert.equal(
      fkLine,
      undefined,
      `FK violation in aria2 log: ${fkLine ?? '(none)'}`
    )

    // Also unpause to confirm the task is in a valid state and can resume.
    await rpc.unpause(gid)
    const after = await waitFor(
      async () => {
        const s = await rpc.tellStatus(gid, [
          'status',
          'errorCode',
          'errorMessage',
        ])
        return s.status === 'active' || s.status === 'waiting' ? s : null
      },
      5_000,
      100,
      'resumed after pause'
    )
    assert.notEqual(after.status, 'error')
  })
})
