// E2E for upstream #1752: an unauthenticated WebSocket RPC session must
// never receive download notifications. The ported fix hard-coded every
// protocol-level error response as AUTHORIZED, so a client that had set no
// token could flip its session to authorized just by sending malformed
// input (parse error, a bare JSON value, or an empty batch) and then
// receive every aria2.onDownload* event (GIDs, task timing).
//
// This drives the real chain a browser client hits: a live ws://.../jsonrpc
// connection, the four bypass messages, and a genuine download event. The
// ported WebSocketSessionManTest never sent a single message to a session.
//
// Negative: a session that sent only malformed messages gets no
// notification. Positive control: a session that authenticated with a
// valid token does receive onDownloadStart.

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
import { waitFor, sleep } from './helpers/wait.mjs'

const SECRET = 'e2e_secret'

// Serve a throttleable payload so the download stays active long enough for
// onDownloadStart to be delivered to connected sessions.
async function startSlowFileServer() {
  const body = Buffer.alloc(2 * 1024 * 1024, 0x61)
  const server = createServer((req, res) => {
    res.statusCode = 200
    res.setHeader('content-type', 'application/octet-stream')
    res.setHeader('content-length', String(body.length))
    res.end(body)
  })
  await new Promise((resolve, reject) => {
    server.once('error', reject)
    server.listen(0, '127.0.0.1', resolve)
  })
  const { port } = server.address()
  return {
    url: `http://127.0.0.1:${port}/payload.bin`,
    close: () =>
      new Promise((resolve) => {
        server.closeAllConnections?.()
        server.close(() => resolve())
      }),
  }
}

// Open a WS to aria2's JSON-RPC endpoint and record every text frame.
function openRecordingWs(port) {
  const ws = new WebSocket(`ws://127.0.0.1:${port}/jsonrpc`)
  const messages = []
  ws.addEventListener('message', (ev) => {
    try {
      messages.push(JSON.parse(ev.data))
    } catch {
      messages.push({ raw: ev.data })
    }
  })
  const opened = new Promise((resolve, reject) => {
    ws.addEventListener('open', () => resolve())
    ws.addEventListener('error', () => reject(new Error('ws error')))
  })
  return { ws, messages, opened }
}

const notifications = (msgs) =>
  msgs.filter((m) => typeof m.method === 'string' && m.method.startsWith('aria2.on'))

describe('unauthenticated WebSocket session receives no notifications', () => {
  let workDir, fileServer, handle, rpc, ports

  before(async () => {
    workDir = await mkdtemp(path.join(os.tmpdir(), 'a2-e2e-wsauth-'))
    fileServer = await startSlowFileServer()
    ports = allocPorts()
    handle = await startInstance({
      bin: defaultAria2Bin(),
      rpcPort: ports.rpcPort,
      rpcSecret: SECRET,
      listenPort: ports.listenPort,
      dhtListenPort: ports.dhtListenPort,
      dir: path.join(workDir, 'dl'),
      sessionPath: path.join(workDir, 'aria2.session'),
      sqliteDbPath: path.join(workDir, 'aria2.db'),
      saveSessionInterval: 1,
      logPath: path.join(workDir, 'aria2.log'),
      silent: true,
      extra: ['--max-download-limit=128K'],
    })
    rpc = new Aria2Rpc({ port: ports.rpcPort, secret: SECRET })
  })

  after(async () => {
    try { await handle?.stop('SIGTERM') } catch {}
    try { await fileServer?.close() } catch {}
    if (process.env.ARIA2_E2E_KEEP_TMP) {
      console.log(`workDir kept: ${workDir}`)
    } else if (workDir) {
      await rm(workDir, { recursive: true, force: true })
    }
  })

  it('malformed messages do not authorize; valid token does', async () => {
    // --- Attacker session: only ever sends malformed input ---
    const attacker = openRecordingWs(ports.rpcPort)
    await attacker.opened
    attacker.ws.send('{ this is not json')            // parse error
    attacker.ws.send('123')                            // valid JSON, not object/array
    attacker.ws.send('[]')                             // empty batch
    attacker.ws.send(JSON.stringify([1, 'x', null]))   // batch of non-objects

    // --- Authorized session: authenticates with the real token ---
    const legit = openRecordingWs(ports.rpcPort)
    await legit.opened
    legit.ws.send(
      JSON.stringify({
        jsonrpc: '2.0',
        id: 'auth-1',
        method: 'aria2.getVersion',
        params: [`token:${SECRET}`],
      })
    )
    // Wait until the authorized session has seen its getVersion result, so
    // it is marked authorized before the download starts.
    await waitFor(
      async () =>
        legit.messages.some((m) => m.id === 'auth-1' && m.result) ? true : null,
      8_000,
      100,
      'authorized getVersion result'
    )

    // Give the attacker's bogus messages time to be processed too.
    await sleep(500)

    // Trigger a real download event on the authenticated HTTP channel.
    const gid = await rpc.call('aria2.addUri', [[fileServer.url], {}])
    assert.ok(gid, 'download queued')

    // The authorized session must receive onDownloadStart.
    await waitFor(
      async () =>
        notifications(legit.messages).some(
          (m) => m.method === 'aria2.onDownloadStart'
        )
          ? true
          : null,
      10_000,
      100,
      'authorized session receives onDownloadStart'
    )

    // Let any (erroneously broadcast) notifications reach the attacker.
    await sleep(1_000)

    const attackerNotes = notifications(attacker.messages)
    assert.equal(
      attackerNotes.length,
      0,
      `unauthenticated session must receive no notifications, got: ${JSON.stringify(
        attackerNotes
      )}`
    )

    attacker.ws.close()
    legit.ws.close()
  })
})
