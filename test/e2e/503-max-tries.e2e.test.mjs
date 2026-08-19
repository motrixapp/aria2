// E2E for upstream #1839: a server that always answers HTTP 503 must not
// retry forever. A 503 retry deliberately does not consume the permanent
// max-tries budget (the request is re-pooled and its try count reset on
// wake to preserve parallelism after a transient outage), but that made
// the loop unbounded. The fix adds an independent consecutive-503 counter
// capped at max-tries, so the download aborts instead of hammering the
// server indefinitely.
//
// This spans the real chain (HttpResponseCommand -> DlRetryEx ->
// AbstractCommand::execute retry logic -> FileEntry request pool), which
// the ported FileEntryTest primitive never exercised. Against the pre-fix
// binary this test times out (the task never leaves active/waiting);
// against the fixed binary the task reaches `error` within a few seconds.

import { describe, it, before, after } from 'node:test'
import assert from 'node:assert/strict'
import { mkdtemp, rm } from 'node:fs/promises'
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

async function startAlways503Server() {
  let requestCount = 0
  const server = createServer((req, res) => {
    requestCount++
    res.statusCode = 503
    res.setHeader('retry-after', '1')
    res.setHeader('content-length', '0')
    res.end()
  })
  await new Promise((resolve, reject) => {
    server.once('error', reject)
    server.listen(0, '127.0.0.1', resolve)
  })
  const { port } = server.address()
  return {
    url: `http://127.0.0.1:${port}/always-503.bin`,
    get requestCount() {
      return requestCount
    },
    close: () =>
      new Promise((resolve) => {
        server.closeAllConnections?.()
        server.close(() => resolve())
      }),
  }
}

describe('persistent HTTP 503 is bounded by max-tries', () => {
  let workDir, server, handle, rpc, ports

  before(async () => {
    workDir = await mkdtemp(path.join(os.tmpdir(), 'a2-e2e-503-'))
    server = await startAlways503Server()
    ports = allocPorts()
    handle = await startInstance({
      bin: defaultAria2Bin(),
      rpcPort: ports.rpcPort,
      rpcSecret: 'e2e_secret',
      listenPort: ports.listenPort,
      dhtListenPort: ports.dhtListenPort,
      dir: path.join(workDir, 'dl'),
      sessionPath: path.join(workDir, 'aria2.session'),
      sqliteDbPath: path.join(workDir, 'aria2.db'),
      saveSessionInterval: 1,
      logPath: path.join(workDir, 'aria2.log'),
      silent: true,
      extra: ['--max-tries=2', '--retry-wait=1'],
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

  it('aborts a always-503 download instead of retrying forever', async () => {
    const gid = await rpc.call('aria2.addUri', [[server.url], {}])

    // With --max-tries=2 and --retry-wait=1 the fixed engine aborts on the
    // second consecutive 503, i.e. within a few seconds. Poll for the
    // terminal `error` state; a pre-fix binary would keep this `active`
    // or `waiting` forever and this waitFor would throw on timeout.
    const status = await waitFor(
      async () => {
        const s = await rpc.tellStatus(gid, ['status', 'errorCode'])
        return s.status === 'error' ? s : null
      },
      25_000,
      250,
      'download reaches error state'
    )

    assert.equal(status.status, 'error', 'download must terminate in error')
    // Sanity on the retry bound: two attempts against the server, not an
    // ever-growing number. Allow a little slack for connection retries.
    assert.ok(
      server.requestCount <= 6,
      `expected a bounded number of 503 hits, got ${server.requestCount}`
    )
  })
})
