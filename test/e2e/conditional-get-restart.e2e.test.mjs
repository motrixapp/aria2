// E2E for upstream #2280: when a conditional request (If-Modified-Since)
// is answered with a fresh 200 instead of 304, the locally present bytes
// are stale and the download must restart from scratch. aria2-next wired
// RESTART_FROM_SCRATCH only into a guard and dropped it before the actual
// loadAndOpenFile() calls, so the restart branch was dead: aria2 treated
// the existing file as already complete and kept the stale content.
//
// This drives the real chain HttpRequestCommand (emits If-Modified-Since)
// -> 200 -> HttpResponseCommand::handleDefaultEncoding ->
// createCheckIntegrityEntry(RESTART_FROM_SCRATCH) -> loadAndOpenFile, which
// no CppUnit test can span end to end.
//
// Scenario: a file of version A ("AAAA...") already sits in the download
// dir with an old mtime and no control file. The server always answers 200
// with the SAME-length version B ("BBBB..."). After the download the file
// must be entirely B; the pre-fix engine leaves it as stale A.

import { describe, it, before, after } from 'node:test'
import assert from 'node:assert/strict'
import { mkdtemp, rm, mkdir, writeFile, readFile, utimes } from 'node:fs/promises'
import path from 'node:path'
import os from 'node:os'
import { createServer } from 'node:http'

import { Aria2Rpc } from './helpers/rpc-client.mjs'
import {
  startInstance,
  allocPorts,
  defaultAria2Bin,
} from './helpers/aria2-process.mjs'
import { waitFor } from './helpers/wait.mjs'

const SIZE = 2048
const VERSION_B = Buffer.alloc(SIZE, 0x42) // "BBBB..."

async function startChangedResourceServer() {
  let hits = 0
  const server = createServer((req, res) => {
    hits++
    // Always answer 200 with version B, regardless of If-Modified-Since,
    // to model a resource that changed since the local copy was written.
    res.statusCode = 200
    res.setHeader('content-type', 'application/octet-stream')
    res.setHeader('content-length', String(VERSION_B.length))
    res.setHeader('last-modified', new Date().toUTCString())
    res.setHeader('accept-ranges', 'bytes')
    res.end(VERSION_B)
  })
  await new Promise((resolve, reject) => {
    server.once('error', reject)
    server.listen(0, '127.0.0.1', resolve)
  })
  const { port } = server.address()
  return {
    url: `http://127.0.0.1:${port}/payload.bin`,
    get hits() {
      return hits
    },
    close: () =>
      new Promise((resolve) => {
        server.closeAllConnections?.()
        server.close(() => resolve())
      }),
  }
}

describe('conditional GET answered with 200 restarts from scratch', () => {
  let workDir, dlDir, server, handle, rpc, ports

  before(async () => {
    workDir = await mkdtemp(path.join(os.tmpdir(), 'a2-e2e-condget-'))
    dlDir = path.join(workDir, 'dl')
    await mkdir(dlDir, { recursive: true })

    // Pre-place version A as the existing output file, with an old mtime and
    // no .aria2 control file — the exact state that makes aria2 send an
    // If-Modified-Since request under --conditional-get.
    const outFile = path.join(dlDir, 'payload.bin')
    await writeFile(outFile, Buffer.alloc(SIZE, 0x41)) // "AAAA..."
    const old = new Date('2020-01-01T00:00:00Z')
    await utimes(outFile, old, old)

    server = await startChangedResourceServer()
    ports = allocPorts()
    handle = await startInstance({
      bin: defaultAria2Bin(),
      rpcPort: ports.rpcPort,
      rpcSecret: 'e2e_secret',
      listenPort: ports.listenPort,
      dhtListenPort: ports.dhtListenPort,
      dir: dlDir,
      sessionPath: path.join(workDir, 'aria2.session'),
      sqliteDbPath: path.join(workDir, 'aria2.db'),
      saveSessionInterval: 1,
      logPath: path.join(workDir, 'aria2.log'),
      silent: true,
      extra: [
        '--conditional-get=true',
        '--continue=true',
        '--allow-overwrite=true',
      ],
    })
    rpc = new Aria2Rpc({ port: ports.rpcPort, secret: 'e2e_secret' })
  })

  after(async () => {
    try { await handle?.stop('SIGTERM') } catch {}
    try { await server?.close() } catch {}
    if (process.env.ARIA2_E2E_KEEP_TMP) {
      console.log(`workDir kept: ${workDir}`)
    } else if (workDir) {
      await rm(workDir, { recursive: true, force: true })
    }
  })

  it('replaces the stale local file with the changed remote content', async () => {
    const gid = await rpc.call('aria2.addUri', [
      [server.url],
      { out: 'payload.bin' },
    ])

    const status = await waitFor(
      async () => {
        const s = await rpc.tellStatus(gid, ['status', 'errorCode'])
        return s.status === 'complete' || s.status === 'error' ? s : null
      },
      20_000,
      200,
      'download reaches a terminal state'
    )
    assert.equal(status.status, 'complete', `download failed: ${JSON.stringify(status)}`)
    assert.ok(server.hits > 0, 'server must have been contacted')

    const finalContent = await readFile(path.join(dlDir, 'payload.bin'))
    // The restart-from-scratch fix truncates the stale A and writes B. The
    // pre-fix engine treats the existing file as already complete and this
    // assertion fails with the file still full of 0x41 ("A").
    assert.ok(
      finalContent.equals(VERSION_B),
      `file must contain the changed remote content (all 0x42); first bytes: ${finalContent
        .subarray(0, 8)
        .toString('hex')}`
    )
  })
})
