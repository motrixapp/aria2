// E2E: BT task pause / resume / restart cycle exercising the
// SQLite3-Persistence path of aria2_motrix.
//
// Scenario per case (single-file, multi-file):
//   1. Start a node:http web-seed server serving the original fixture data.
//   2. Patch the corresponding .torrent file to add a top-level `url-list`
//      pointing at the local web-seed.
//   3. Start aria2 (downloader) with motrix-turbo's parameter envelope.
//   4. addTorrent → wait until task is downloading and has > 0 bytes done.
//   5. pause → resume → pause cycle (state machine sanity).
//   6. Force aria2.saveSession + wait > save-session-interval, then SIGTERM.
//   7. Restart aria2 with the SAME --sqlite3-db-path / --save-session.
//   8. Wait for the task to reappear; assert it is paused and unchanged.
//   9. Resume; for 5 s assert `status` never becomes `error` and
//      `errorCode` stays 0.

import { describe, it, before, after } from 'node:test'
import assert from 'node:assert/strict'
import { mkdtemp, rm, mkdir, writeFile } from 'node:fs/promises'
import path from 'node:path'
import os from 'node:os'
import { fileURLToPath } from 'node:url'

import {
  patchTorrentWithWebSeed,
  summarizeTorrent,
} from './helpers/torrent-patch.mjs'
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
const STATUS_KEYS = [
  'gid',
  'status',
  'errorCode',
  'errorMessage',
  'totalLength',
  'completedLength',
  'downloadSpeed',
  'numPieces',
  'pieceLength',
  'bittorrent',
]

// aria2 omits errorCode while a task is active and only emits it once the
// task transitions to error/complete. Treat undefined / '0' as "no error".
const isErrorCodeOk = (s) =>
  s.errorCode === undefined || s.errorCode === '0' || s.errorCode === 0

// `pauseAfterPieces` controls when we issue the first forcePause:
//   0 → pause as soon as we see ANY bytes downloaded (in_flight piece only)
//   1 → pause after at least 1 complete piece (bitfield bit set)
//
// `dirSuffix` mimics Motrix Turbo's `.motrix` in-flight container — it is
// appended to the per-test work-dir's `download` to recreate the exact path
// shape that triggered the user-reported error 13 in the field.
const CASES = [
  {
    // True single-file-format torrent (info has `length`, no `files` list).
    // This is what real-world distros (Ubuntu ISO, etc.) ship and is the
    // shape that hit the user-reported error 13 in motrix-turbo.
    name: 'true single-file-format / partial-piece pause / .motrix dir',
    torrent: 'single.torrent',
    pauseAfterPieces: 0,
    dirSuffix: '.motrix',
  },
  {
    name: 'true single-file-format / full-piece pause / .motrix dir',
    torrent: 'single.torrent',
    pauseAfterPieces: 1,
    dirSuffix: '.motrix',
  },
  {
    name: 'multi-file-format-with-1-file / partial-piece pause',
    torrent: 'sample.torrent',
    pauseAfterPieces: 0,
    dirSuffix: '',
  },
  {
    name: 'multi-file-format-with-1-file / full-piece pause',
    torrent: 'sample.torrent',
    pauseAfterPieces: 1,
    dirSuffix: '',
  },
  {
    name: 'multi-file-torrent / partial-piece pause',
    torrent: 'multi.torrent',
    pauseAfterPieces: 0,
    dirSuffix: '',
  },
  {
    name: 'multi-file-torrent / full-piece pause',
    torrent: 'multi.torrent',
    pauseAfterPieces: 1,
    dirSuffix: '',
  },
]

for (const c of CASES) {
  describe(`sqlite-persistence restart cycle (${c.name})`, () => {
    let workDir
    let webseed
    let downloaderHandle
    let rpc
    let sessionPath
    let sqlitePath
    let dlDir
    let logPath
    let ports
    let patchedTorrentPath
    let summary
    let gid

    before(async () => {
      const slug = c.name.replace(/[^a-z0-9]+/gi, '-').toLowerCase()
      workDir = await mkdtemp(path.join(os.tmpdir(), `a2-e2e-${slug}-`))
      // 1. Web-seed server serves the master fixture data on a random port.
      webseed = await startWebSeedServer(FIXTURES)

      // 2. Patch the torrent to inject the web-seed URL.
      const original = path.join(FIXTURES, c.torrent)
      const patched = await patchTorrentWithWebSeed(original, webseed.url)
      patchedTorrentPath = path.join(workDir, c.torrent)
      await writeFile(patchedTorrentPath, patched)
      summary = summarizeTorrent(patched)

      // 3. Lay out per-test paths. dirSuffix lets us reproduce Motrix
      // Turbo's `.motrix` in-flight container exactly.
      dlDir = path.join(workDir, `download${c.dirSuffix ?? ''}`)
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
        // eslint-disable-next-line no-console
        console.log(`workDir kept: ${workDir}`)
      } else if (workDir) {
        await rm(workDir, { recursive: true, force: true })
      }
    })

    it('add → download → pause → resume → pause → restart → resume preserves task', async () => {
      // ---- 4. addTorrent and wait for actual progress ---------------------
      const fs = await import('node:fs/promises')
      const buf = await fs.readFile(patchedTorrentPath)
      // Throttle the download so the task stays in `active` long enough for
      // pause/resume/restart cycles to run while it is still downloading.
      // sample.torrent is 1 MB, multi.torrent is 512 KB; at 64 KB/s they
      // need ≥8 s to finish — well past our pause/resume window.
      gid = await rpc.addTorrent(buf.toString('base64'), {
        dir: dlDir,
        pause: 'false',
        'seed-time': '0',
        'seed-ratio': '0.0',
        'max-download-limit': '64K',
      })
      assert.match(gid, /^[0-9a-f]+$/)

      const minBytes =
        c.pauseAfterPieces > 0
          ? c.pauseAfterPieces * summary.pieceLength
          : 1
      const beforePause = await waitFor(
        async () => {
          const s = await rpc.tellStatus(gid, STATUS_KEYS)
          if (
            s.status === 'active' &&
            Number(s.completedLength) >= minBytes
          ) {
            return s
          }
          return null
        },
        30_000,
        200,
        `first active + completedLength>=${minBytes}`
      )
      assert.ok(
        isErrorCodeOk(beforePause),
        `no error before pause, got ${beforePause.errorCode} ${beforePause.errorMessage}`
      )
      assert.ok(
        Number(beforePause.completedLength) > 0,
        'must have downloaded some bytes via web-seed'
      )

      // ---- 5. pause / resume / pause -------------------------------------
      const preBefore = await rpc.tellStatus(gid, STATUS_KEYS)
      console.log(
        `[diag] pre-pause status=${preBefore.status} completed=${preBefore.completedLength}/${preBefore.totalLength} speed=${preBefore.downloadSpeed}`
      )
      const pauseRes = await rpc.forcePause(gid)
      console.log(`[diag] pause(${gid}) -> ${pauseRes}`)
      await waitFor(
        async () => {
          const s = await rpc.tellStatus(gid, ['status', 'errorCode', 'errorMessage'])
          if (s.status === 'paused') return s
          // Log every observation so timeout context is visible.
          console.log(`[diag] waiting paused, got=${s.status}`)
          return null
        },
        5_000,
        100,
        'first pause'
      )
      await rpc.unpause(gid)
      await waitFor(
        async () => {
          const s = (await rpc.tellStatus(gid, ['status'])).status
          return s === 'active' || s === 'waiting'
        },
        5_000,
        100,
        'first resume'
      )
      await rpc.forcePause(gid)
      const beforeRestart = await waitFor(
        async () => {
          const s = await rpc.tellStatus(gid, STATUS_KEYS)
          return s.status === 'paused' ? s : null
        },
        5_000,
        100,
        'second pause'
      )
      assert.ok(
        isErrorCodeOk(beforeRestart),
        `no error before restart, got ${beforeRestart.errorCode} ${beforeRestart.errorMessage}`
      )

      // ---- 6. flush sqlite + SIGTERM -------------------------------------
      await rpc.saveSession()
      await sleep(1500) // exceed --save-session-interval=1
      const exit = await downloaderHandle.stop('SIGTERM', 8000)
      assert.ok(
        exit.code === 0 || exit.signal === 'SIGTERM',
        `aria2 should exit cleanly, got ${JSON.stringify(exit)}`
      )

      // ---- 7. restart aria2 with the SAME db / session paths -------------
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

      // ---- 8. confirm task reloaded (paused), with same gid --------------
      const reloaded = await waitFor(
        async () => {
          // After restart aria2 may place the task in waiting list (paused)
          // or active list. tellStatus on the gid is the cheapest probe.
          try {
            const s = await rpc.tellStatus(gid, STATUS_KEYS)
            return s
          } catch {
            return null
          }
        },
        10_000,
        200,
        'task reload after restart'
      )
      console.log('[diag] post-restart tellStatus:', JSON.stringify(reloaded))
      // also dump any task in waiting/stopped lists
      const waiting = await rpc.tellWaiting(0, 50)
      const active = await rpc.tellActive()
      const stopped = await rpc.tellStopped(0, 50)
      console.log(
        '[diag] active=',
        active.length,
        'waiting=',
        waiting.length,
        'stopped=',
        stopped.length
      )
      assert.equal(reloaded.gid, gid, 'gid must match across restart')
      assert.equal(
        reloaded.status,
        'paused',
        `expected paused after restart, got ${reloaded.status} (errorCode=${reloaded.errorCode}, msg=${reloaded.errorMessage})`
      )
      assert.ok(
        isErrorCodeOk(reloaded),
        `errorCode must be 0 after reload, got ${reloaded.errorCode}: ${reloaded.errorMessage}`
      )
      // NOTE: For paused tasks reloaded from sqlite, pieceStorage_ is null
      // and RequestGroup::getTotalLength() returns 0 by design. We only
      // check that the bittorrent block was reattached and the saved
      // .torrent file is present — the post-resume polling below is what
      // actually exercises the bug we care about.
      assert.ok(
        reloaded.bittorrent,
        'bittorrent block must be present after reload'
      )
      assert.equal(
        Number(reloaded.numPieces),
        Math.ceil(summary.totalSize / summary.pieceLength),
        'numPieces must match torrent metadata after reload'
      )

      // ---- 9. resume; wait for completion. Assert never enters `error`. ---
      // Lift the throttle so the remaining ~1 MB / 0.5 MB finishes quickly.
      await rpc.call('aria2.changeOption', [
        gid,
        { 'max-download-limit': '0' },
      ])
      await rpc.unpause(gid)
      const observed = []
      const deadline = Date.now() + 30_000
      let final
      while (Date.now() < deadline) {
        let s
        try {
          s = await rpc.tellStatus(gid, [
            'status',
            'errorCode',
            'errorMessage',
            'completedLength',
            'totalLength',
          ])
        } catch (err) {
          // tellStatus throws once the task has been removed from the
          // active/waiting lists into "stopped" — capture once and break.
          observed.push(`gone:${err.aria2Code ?? '?'}`)
          break
        }
        observed.push(`${s.status}@${s.completedLength}/${s.totalLength}`)
        assert.notEqual(
          s.status,
          'error',
          `status went to error after resume: ${JSON.stringify(
            s
          )}\nobserved: ${observed.slice(-10).join(' | ')}`
        )
        assert.ok(
          isErrorCodeOk(s),
          `errorCode != 0 after resume: ${s.errorCode} ${
            s.errorMessage
          }\nobserved: ${observed.slice(-10).join(' | ')}`
        )
        if (s.status === 'complete' || s.status === 'completed') {
          final = s
          break
        }
        await sleep(200)
      }
      // Also sweep stopped list — if the task dropped out of active without
      // an `error` ping, it must be in `complete`, not in `error`.
      const stopped2 = await rpc.tellStopped(0, 100, [
        'gid',
        'status',
        'errorCode',
        'errorMessage',
      ])
      const stoppedSelf = stopped2.find((t) => t.gid === gid)
      if (stoppedSelf) {
        console.log('[diag] stopped entry:', JSON.stringify(stoppedSelf))
        assert.notEqual(
          stoppedSelf.status,
          'error',
          `task ended up in stopped:error: ${JSON.stringify(stoppedSelf)}`
        )
        assert.ok(
          isErrorCodeOk(stoppedSelf),
          `task ended up with errorCode=${stoppedSelf.errorCode}: ${stoppedSelf.errorMessage}`
        )
      }
      assert.ok(
        observed.some((st) => !st.startsWith('paused')),
        `task stayed paused after unpause: observed=${observed.join(',')}`
      )
    })
  })
}
