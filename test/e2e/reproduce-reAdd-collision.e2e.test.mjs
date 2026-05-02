// Reproduces the user-reported `errorCode=13` directly by simulating the
// shape of failure that Motrix Turbo's `SessionManager.reAddOrMarkError`
// path can land on at startup.
//
// Background: Motrix Turbo persists its own task metadata in `motrix.db`
// (better-sqlite3) and aria2 persists its own state in `aria2.db`
// (sqlite3-persistence inside aria2_motrix). On startup, Motrix Turbo
// queries aria2's tellAll lists and tries to match each `motrix.db` row
// to an aria2 row by gid / infoHash / uriHash. If the match FAILS for any
// reason — racing the sqlite restore, gid format drift, a fresh aria2.db
// after a wipe, etc. — Motrix Turbo calls `aria2.addTorrent` to re-add the
// task, which produces a NEW gid. The new gid points at the same `dir`
// (Motrix Turbo's `.motrix` in-flight container), where the data file
// from the previous session still lives. With sqlite-persistence enabled
// the legacy `<file>.aria2` control file does not exist, and the new gid
// has no task_progress row, so aria2's safety check at
// `RequestGroup.cc:441` throws `errorCode=13` and the user sees the bug.

import { describe, it, before, after } from 'node:test'
import assert from 'node:assert/strict'
import { mkdtemp, rm, mkdir, writeFile } from 'node:fs/promises'
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

describe('motrix-turbo re-add collision (errorCode=13 reproduction)', () => {
  let workDir, webseed, downloaderHandle, rpc
  let dlDir, sessionPath, sqlitePath, logPath, ports
  let patchedTorrentPath

  before(async () => {
    workDir = await mkdtemp(path.join(os.tmpdir(), 'a2-e2e-readd-'))
    webseed = await startWebSeedServer(FIXTURES)
    // Use the true single-file torrent shape that the user hit in
    // motrix-turbo with the Ubuntu ISO.
    const patched = await patchTorrentWithWebSeed(
      path.join(FIXTURES, 'single.torrent'),
      webseed.url
    )
    patchedTorrentPath = path.join(workDir, 'single.torrent')
    await writeFile(patchedTorrentPath, patched)

    // Mirror Motrix Turbo's `.motrix` in-flight container.
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

  it('without check-integrity, second add (different gid) trips errorCode=13 — and with check-integrity, it does NOT', async () => {
    // ---- Round 1: first add, partial download, pause, SIGTERM -----------
    const fs = await import('node:fs/promises')
    const torrentBuf = await fs.readFile(patchedTorrentPath)
    const firstGid = await rpc.addTorrent(torrentBuf.toString('base64'), {
      dir: dlDir,
      pause: 'false',
      'seed-time': '0',
      'seed-ratio': '0.0',
      'max-download-limit': '64K',
    })
    console.log(`[diag] round-1 gid=${firstGid}`)

    await waitFor(
      async () => {
        const s = await rpc.tellStatus(firstGid, [
          'status',
          'completedLength',
        ])
        return s.status === 'active' && Number(s.completedLength) > 0
          ? s
          : null
      },
      30_000,
      200,
      'round-1 first progress'
    )
    await rpc.forcePause(firstGid)
    await waitFor(
      async () => (await rpc.tellStatus(firstGid, ['status'])).status === 'paused',
      5_000,
      100,
      'round-1 pause'
    )

    // SIGTERM aria2 cleanly so it persists task + task_progress for firstGid.
    try { await rpc.saveSession() } catch {}
    await sleep(1_500)
    await downloaderHandle.stop('SIGTERM', 8_000)

    // ---- Round 2: simulate motrix-turbo "re-add with new gid" -----------
    // The user's motrix-turbo scenario: on restart, motrix-turbo's
    // SessionManager could fail to match the saved metadata to aria2's
    // active list (race / gid drift / wipe) and fall through to
    // `reAddOrMarkError`. We model that failure here by ALSO wiping
    // aria2.db before restart so aria2's sqlite restore yields nothing —
    // motrix-turbo's re-add path is then forced.
    await fs.rm(sqlitePath, { force: true })
    await fs.rm(`${sqlitePath}-shm`, { force: true })
    await fs.rm(`${sqlitePath}-wal`, { force: true })

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

    // Round-2 add: NEW gid, same dir. The data file from round 1 is still
    // on disk under <dlDir>/ubuntu-like.iso.
    const secondGid = await rpc.addTorrent(torrentBuf.toString('base64'), {
      dir: dlDir,
      pause: 'false',
      'seed-time': '0',
      'seed-ratio': '0.0',
      'max-download-limit': '64K',
    })
    console.log(`[diag] round-2 gid=${secondGid} (new — same as motrix re-add)`)
    assert.notEqual(
      secondGid,
      firstGid,
      'round-2 gid MUST differ — that is what makes this a re-add'
    )

    // Watch for errorCode=13. With aria2_motrix's current safety check the
    // task is expected to fail almost immediately.
    let lastSeen = null
    const observed = []
    const deadline = Date.now() + 15_000
    while (Date.now() < deadline) {
      let s
      try {
        s = await rpc.tellStatus(secondGid, [
          'status',
          'errorCode',
          'errorMessage',
        ])
      } catch {
        const stopped = await rpc.tellStopped(0, 100, [
          'gid',
          'status',
          'errorCode',
          'errorMessage',
        ])
        const me = stopped.find((t) => t.gid === secondGid)
        if (me) {
          observed.push(`stopped:${me.status}:${me.errorCode}`)
          lastSeen = me
        }
        break
      }
      observed.push(`${s.status}:${s.errorCode ?? '-'}`)
      lastSeen = s
      if (s.status === 'error' || (s.errorCode && s.errorCode !== '0')) {
        break
      }
      if (s.status === 'complete') break
      await sleep(150)
    }
    console.log(`[diag] round-2 observed:`, observed.slice(0, 30).join(' | '))
    console.log(`[diag] round-2 final:`, JSON.stringify(lastSeen))

    // The reproduction is successful when the new gid hits errorCode=13
    // (FILE_ALREADY_EXISTS). If it does NOT, the bug pattern is something
    // else and the test will fail loudly so we can investigate further.
    assert.equal(
      lastSeen?.errorCode,
      '13',
      `expected errorCode=13 (FILE_ALREADY_EXISTS); got ${JSON.stringify(lastSeen)}`
    )

    // ---- Round 3: same setup, but with check-integrity=true ------------
    // This is the fix path Motrix Turbo's `SessionManager.reAddOrMarkError`
    // hands to aria2.addTorrent. With it set, aria2 hash-checks the data
    // file already on disk instead of bailing out, so the user-reported
    // bug NEVER fires.
    const thirdGid = await rpc.addTorrent(torrentBuf.toString('base64'), {
      dir: dlDir,
      pause: 'false',
      'seed-time': '0',
      'seed-ratio': '0.0',
      'max-download-limit': '64K',
      'check-integrity': 'true',
    })
    console.log(
      `[diag] round-3 gid=${thirdGid} (re-add with check-integrity=true)`
    )
    assert.notEqual(thirdGid, secondGid)

    const round3Observed = []
    const r3Deadline = Date.now() + 15_000
    let r3Final = null
    while (Date.now() < r3Deadline) {
      let s
      try {
        s = await rpc.tellStatus(thirdGid, [
          'status',
          'errorCode',
          'errorMessage',
          'completedLength',
        ])
      } catch {
        break
      }
      round3Observed.push(`${s.status}:${s.errorCode ?? '-'}@${s.completedLength}`)
      r3Final = s
      if (s.status === 'error' || (s.errorCode && s.errorCode !== '0')) {
        break
      }
      if (s.status === 'complete') break
      await sleep(150)
    }
    console.log(`[diag] round-3 observed:`, round3Observed.slice(0, 30).join(' | '))
    console.log(`[diag] round-3 final:`, JSON.stringify(r3Final))
    // Hash-check resume must NOT enter the error state.
    assert.notEqual(
      r3Final?.status,
      'error',
      `round-3 with check-integrity should not error; got ${JSON.stringify(r3Final)}`
    )
    assert.ok(
      r3Final?.errorCode === undefined ||
        r3Final?.errorCode === '0' ||
        r3Final?.errorCode === 0,
      `round-3 errorCode should be 0/absent; got ${r3Final?.errorCode}`
    )
  })
})
