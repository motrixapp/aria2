import { after, before, describe, it } from 'node:test'
import assert from 'node:assert/strict'
import { createServer } from 'node:http'
import { mkdtemp, readFile, rm } from 'node:fs/promises'
import { tmpdir } from 'node:os'
import path from 'node:path'
import { allocPorts, spawnAria2, stopInstance } from './helpers/aria2-process.mjs'
import { Aria2Rpc } from './helpers/rpc-client.mjs'
import { waitFor } from './helpers/wait.mjs'

const secret = 'redirect-policy-local-test'
const payload = Buffer.from('one stable resource, with a resource-scoped ETag')
let root, server, origin, proc, rpc, sequence = 0
const requests = []
const held = new Map()

async function start({ input, database = 'session.db' } = {}) {
  const { rpcPort, listenPort } = allocPorts()
  proc = await spawnAria2({ args: [
    '--no-conf=true', '--no-netrc=true', '--enable-rpc=true',
    '--rpc-listen-all=false', `--rpc-listen-port=${rpcPort}`, `--rpc-secret=${secret}`,
    '--disable-ipv6=true', '--enable-dht=false', '--enable-dht6=false', '--enable-peer-exchange=false',
    '--bt-enable-lpd=false', `--listen-port=${listenPort}`, '--all-proxy=',
    '--http-proxy=', '--https-proxy=', '--no-proxy=', '--max-tries=1',
    '--max-concurrent-downloads=2', '--split=1', '--file-allocation=none',
    '--auto-file-renaming=false', '--allow-overwrite=false', '--pause=true',
    '--console-log-level=error', '--summary-interval=0', `--dir=${root}`,
    `--save-session=${root}/session.txt`, '--force-save=true',
    '--enable-sqlite3-persistence=true', `--sqlite3-db-path=${root}/${database}`,
    ...(input ? [`--input-file=${input}`] : []),
  ] })
  rpc = new Aria2Rpc({ port: rpcPort, secret })
  await waitFor(() => rpc.call('aria2.getVersion'), 10_000)
}

async function add(url, options = {}) {
  return rpc.call('aria2.addUri', [[url], {
    out: `download-${++sequence}.bin`, pause: 'false',
    header: ['If-Match: "same-etag"', 'Accept-Encoding: identity', 'Cookie: '],
    ...options,
  }])
}

async function ended(gid) {
  return waitFor(async () => {
    const state = await rpc.tellStatus(gid)
    return ['complete', 'error'].includes(state.status) ? state : false
  }, 10_000, 25, gid)
}

function redirect(response, status, target) {
  response.writeHead(status, { Location: target, ETag: '"same-etag"', 'Content-Length': '0' })
  response.end()
}

describe('per-task HTTP redirect policy', { timeout: 90_000 }, () => {
  before(async () => {
    root = await mkdtemp(path.join(tmpdir(), 'aria2-http-redirect-policy-'))
    server = createServer((request, response) => {
      const url = new URL(request.url, 'http://127.0.0.1')
      requests.push(url.pathname)
      const [, route, value, id] = url.pathname.split('/')
      if (route === 'redirect-link') {
        response.writeHead(302, {
          Location: `/target/${value}`,
          Link: `<http://localhost:${server.address().port}/mirror/${value}>; rel=duplicate`,
          ETag: '"same-etag"', 'Content-Length': '0',
        })
        response.end()
      } else if (route === 'truncated-link') {
        response.writeHead(200, {
          Link: `<http://localhost:${server.address().port}/mirror/${value}>; rel=duplicate`,
          ETag: '"same-etag"', 'Content-Length': String(payload.length),
        })
        response.write(payload.subarray(0, 8))
        setTimeout(() => response.destroy(), 20)
      } else if (route === 'redirect') {
        redirect(response, Number(value), `/target/${id}`)
      } else if (route === 'chain' && Number(value) > 0) {
        redirect(response, 302, `/chain/${Number(value) - 1}/${id}`)
      } else if (route === 'held') {
        held.set(value, response)
      } else {
        const range = /^bytes=(\d+)-(\d*)$/.exec(request.headers.range ?? '')
        const start = range ? Number(range[1]) : 0
        const end = range?.[2] ? Number(range[2]) : payload.length - 1
        response.writeHead(range ? 206 : 200, {
          ETag: '"same-etag"', 'Content-Length': String(end - start + 1),
          ...(range ? { 'Content-Range': `bytes ${start}-${end}/${payload.length}` } : {}),
        })
        response.end(payload.subarray(start, end + 1))
      }
    })
    await new Promise((resolve, reject) => {
      server.once('error', reject)
      server.listen(0, '127.0.0.1', resolve)
    })
    origin = `http://127.0.0.1:${server.address().port}`
    await start()
  })

  after(async () => {
    if (proc) await stopInstance(proc)
    if (server) {
      server.closeAllConnections()
      await new Promise(resolve => server.close(resolve))
    }
    if (root) await rm(root, { recursive: true, force: true })
  })

  it('advertises the implemented policy and SQLite checkpoint support', async () => {
    const version = await rpc.call('aria2.getVersion')
    assert.ok(version.enabledFeatures.includes('HttpRedirectPolicyV1'))
    assert.ok(version.enabledFeatures.includes('LegacyCheckpointImportV1'))
  })

  it('keeps port 443 in the HTTP Host authority used for a validated resource', async () => {
    // Use a local forward proxy so this regression requires neither DNS nor a
    // privileged listener. A virtual host can serve different bytes at the same
    // path and ETag when the Host authority changes between validation/resume.
    const seen = []
    const proxy = createServer((request, response) => {
      seen.push({ url: request.url, host: request.headers.host })
      response.writeHead(200, { ETag: '"same-etag"', 'Content-Length': String(payload.length) })
      response.end(request.headers.host === 'legacy.example:443' ? payload : Buffer.alloc(payload.length))
    })
    await new Promise((resolve, reject) => {
      proxy.once('error', reject)
      proxy.listen(0, '127.0.0.1', resolve)
    })
    try {
      const url = 'http://legacy.example:443/resource'
      const gid = await add(url, {
        'http-proxy': `http://127.0.0.1:${proxy.address().port}`,
        'max-http-redirect': '0',
      })
      const state = await ended(gid)
      assert.equal(state.status, 'complete', state.errorMessage)
      assert.deepEqual(seen, [{ url, host: 'legacy.example:443' }])
      assert.deepEqual(await readFile(state.files[0].path), payload)
    } finally {
      proxy.closeAllConnections()
      await new Promise(resolve => proxy.close(resolve))
    }
  })

  for (const status of [300, 301, 302, 303, 307, 308]) {
    it(`blocks ${status} before any redirect target request when the limit is zero`, async () => {
      const id = `zero-${status}`
      const gid = await add(`${origin}/redirect/${status}/${id}`, { 'max-http-redirect': '0' })
      const state = await ended(gid)
      assert.equal(state.status, 'error')
      assert.equal(state.errorCode, '23')
      assert.equal((await rpc.call('aria2.getOption', [gid]))['max-http-redirect'], '0')
      assert.ok(requests.includes(`/redirect/${status}/${id}`))
      assert.ok(!requests.includes(`/target/${id}`))
    })
  }

  it('cannot escape a rejected Location through a server-advertised Link mirror', async () => {
    const gid = await add(`${origin}/redirect-link/blocked`, { 'max-http-redirect': '0' })
    assert.equal((await ended(gid)).errorCode, '23')
    assert.ok(!requests.includes('/target/blocked'))
    assert.ok(!requests.includes('/mirror/blocked'))
  })

  it('does not admit a Link fallback from an initial response whose body fails', async () => {
    const gid = await add(`${origin}/truncated-link/blocked`, { 'max-http-redirect': '0' })
    assert.equal((await ended(gid)).status, 'error')
    assert.ok(!requests.includes('/mirror/blocked'))
    const compatible = await add(`${origin}/truncated-link/default`)
    const completed = await ended(compatible)
    assert.equal(completed.status, 'complete', JSON.stringify({ completed, requests }))
    assert.ok(requests.includes('/mirror/default'))
  })

  it('keeps the default limit at twenty and follows ordinary redirects', async () => {
    const gid = await add(`${origin}/redirect/302/default`, { pause: 'true' })
    assert.equal((await rpc.call('aria2.getOption', [gid]))['max-http-redirect'], '20')
    await rpc.unpause(gid)
    const state = await ended(gid)
    assert.equal(state.status, 'complete', state.errorMessage)
    assert.ok(requests.includes('/target/default'))
  })

  it('enforces a positive limit on each chain without raising the original maximum', async () => {
    const allowed = await add(`${origin}/chain/1/limit-one`, { 'max-http-redirect': '1' })
    assert.equal((await ended(allowed)).status, 'complete')
    const denied = await add(`${origin}/chain/2/limit-one-blocked`, { 'max-http-redirect': '1' })
    assert.equal((await ended(denied)).errorCode, '23')
    assert.ok(!requests.includes('/chain/0/limit-one-blocked'))
    const twenty = await add(`${origin}/chain/20/twenty`)
    assert.equal((await ended(twenty)).status, 'complete')
    const twentyOne = await add(`${origin}/chain/21/twenty-one`)
    assert.equal((await ended(twentyOne)).errorCode, '23')
    assert.ok(!requests.includes('/chain/0/twenty-one'))
  })

  it('rejects invalid values through addUri and changeOption', async () => {
    for (const value of ['-1', '21', '1.5', 'invalid']) {
      await assert.rejects(() => add(`${origin}/target/invalid`, { 'max-http-redirect': value }))
    }
    const gid = await add(`${origin}/redirect/302/changed`, { pause: 'true' })
    await rpc.call('aria2.changeOption', [gid, { 'max-http-redirect': '0' }])
    await assert.rejects(() => rpc.call('aria2.changeOption', [gid, { 'max-http-redirect': '21' }]))
    assert.equal((await rpc.call('aria2.getOption', [gid]))['max-http-redirect'], '0')
    await rpc.unpause(gid)
    assert.equal((await ended(gid)).errorCode, '23')
    assert.ok(!requests.includes('/target/changed'))
  })

  it('applies changeOption to an active request before its redirect arrives', async () => {
    const gid = await add(`${origin}/held/active`)
    const response = await waitFor(() => held.get('active'), 10_000, 25)
    await rpc.call('aria2.changeOption', [gid, { 'max-http-redirect': '0' }])
    redirect(response, 302, '/target/active')
    assert.equal((await ended(gid)).errorCode, '23')
    assert.ok(!requests.includes('/target/active'))
  })

  it('saves and restores the zero policy through an ordinary session file', async () => {
    const gid = await add(`${origin}/redirect/302/restored`, { pause: 'true', 'max-http-redirect': '0' })
    await rpc.saveSession()
    assert.match(await readFile(`${root}/session.txt`, 'utf8'), /max-http-redirect=0/)
    await stopInstance(proc)
    await start({ input: `${root}/session.txt`, database: 'restored.db' })
    assert.equal((await rpc.call('aria2.getOption', [gid]))['max-http-redirect'], '0')
    await rpc.unpause(gid)
    assert.equal((await ended(gid)).errorCode, '23')
    assert.ok(!requests.includes('/target/restored'))
  })
})
