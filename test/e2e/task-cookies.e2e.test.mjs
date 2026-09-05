import { after, before, describe, it } from 'node:test'
import assert from 'node:assert/strict'
import { createServer } from 'node:http'
import { execFileSync } from 'node:child_process'
import { createServer as createTlsServer } from 'node:https'
import { mkdtemp, mkdir, readFile, writeFile, rm } from 'node:fs/promises'
import { tmpdir } from 'node:os'
import path from 'node:path'
import { fileURLToPath } from 'node:url'
import { allocPorts, defaultAria2Bin, spawnAria2, stopInstance } from './helpers/aria2-process.mjs'
import { Aria2Rpc } from './helpers/rpc-client.mjs'

const fixture = fileURLToPath(new URL('./fixtures/cookie-tls/', import.meta.url))
const payload = Buffer.alloc(4 * 1024 * 1024, 0x63)
const secret = 'synthetic-cookie-rpc-secret'
let root, http, other, tls, base, otherBase, tlsBase
let requests = []
let serial = 0

function handler(req, res) {
  requests.push({ path: req.url, host: req.headers.host, ...req.headers })
  const redirects = {
    '/redirect-same': '/auth',
    '/redirect-host': otherBase.replace('127.0.0.1', 'localhost') + '/auth',
    '/redirect-port': otherBase + '/auth',
    '/redirect-http': base + '/auth',
    '/set-cookie': '/auth',
  }
  if (redirects[req.url]) {
    res.writeHead(302, {
      location: redirects[req.url], 'content-length': '0',
      ...(req.url === '/set-cookie' ? { 'set-cookie': 'sid=server-only; Path=/' } : {}),
    })
    res.end()
    return
  }
  let body = req.url === '/blob' ? payload : Buffer.from(req.headers.cookie?.includes('sid=good') ? 'REAL_TEST_FILE' : 'COOKIE_REQUIRED')
  const range = /^bytes=(\d+)-(\d*)$/.exec(req.headers.range ?? '')
  if (req.url === '/blob' && !req.headers.cookie?.includes('sid=good')) {
    body = Buffer.from('COOKIE_REQUIRED')
  }
  res.setHeader('accept-ranges', 'bytes')
  if (range) {
    const first = Number(range[1])
    const last = Math.min(range[2] ? Number(range[2]) : body.length - 1, body.length - 1)
    if (first >= body.length) {
      res.writeHead(416, { 'content-range': `bytes */${body.length}` })
      res.end()
      return
    }
    res.statusCode = 206
    res.setHeader('content-range', `bytes ${first}-${last}/${body.length}`)
    body = body.subarray(first, last + 1)
  }
  res.setHeader('content-length', body.length)
  res.end(req.method === 'HEAD' ? undefined : body)
}

async function listen(server) {
  await new Promise((resolve, reject) => {
    server.once('error', reject)
    server.listen(0, '127.0.0.1', resolve)
  })
  return server.address().port
}

async function until(get, predicate, timeout = 15000) {
  const end = Date.now() + timeout
  let value
  do {
    value = await get()
    if (predicate(value)) return value
    await new Promise(resolve => setTimeout(resolve, 30))
  } while (Date.now() < end)
  assert.fail(`Condition timed out: ${JSON.stringify(value)}`)
}

async function engine(t, { directory, startupJar, restore = false } = {}) {
  const dir = directory ?? path.join(root, `run-${++serial}`)
  await mkdir(dir, { recursive: true })
  const { rpcPort } = allocPorts()
  const args = [
    '--no-conf=true', '--no-netrc=true', '--enable-rpc=true', '--rpc-listen-all=false',
    `--rpc-listen-port=${rpcPort}`, `--rpc-secret=${secret}`, '--disable-ipv6=true',
    ...(execFileSync(defaultAria2Bin(), ['--version'], { encoding: 'utf8' }).includes('BitTorrent')
      ? ['--enable-dht=false', '--enable-dht6=false', '--enable-peer-exchange=false', '--bt-enable-lpd=false'] : []),
    '--all-proxy=', '--http-proxy=', '--https-proxy=',
    '--split=1', '--max-connection-per-server=1', '--max-concurrent-downloads=8',
    '--max-tries=1', '--file-allocation=none', '--auto-file-renaming=false',
    '--allow-overwrite=true', '--check-certificate=true', '--summary-interval=0',
    `--ca-certificate=${path.join(fixture, 'cert.pem')}`, `--dir=${dir}`,
    `--log=${path.join(dir, 'engine.log')}`, '--log-level=debug',
    `--save-session=${path.join(dir, 'session.txt')}`, '--force-save=true',
    '--save-session-interval=1', '--enable-sqlite3-persistence=true',
    `--sqlite3-db-path=${path.join(dir, 'session.db')}`,
    ...(startupJar ? [`--load-cookies=${startupJar}`] : []),
    ...(restore ? [`--input-file=${path.join(dir, 'session.txt')}`, '--pause=true'] : []),
  ]
  const proc = await spawnAria2({ args })
  t.after(() => stopInstance(proc))
  const rpc = new Aria2Rpc({ port: rpcPort, secret })
  await until(async () => {
    if (proc.exitCode !== null) assert.fail(`aria2 exited: ${proc.exitCode}`)
    return rpc.call('aria2.getVersion').catch(() => null)
  }, Boolean)
  return { rpc, dir, stop: () => stopInstance(proc) }
}

function cookie(value = 'good', extra = {}) {
  return { name: 'sid', value, domain: '127.0.0.1', path: '/', ...extra }
}

async function submit(e, url, cookies, options = {}) {
  const out = options.out ?? `file-${++serial}.bin`
  const gid = await e.rpc.call('aria2.addUriWithCookies', [[url], cookies, { out, ...options }])
  return { gid, file: path.join(e.dir, out) }
}

async function finished(e, task) {
  const status = await until(() => e.rpc.tellStatus(task.gid), r => ['complete', 'error'].includes(r.status))
  assert.equal(status.status, 'complete', status.errorMessage)
  return readFile(task.file)
}

describe('task cookie and credential redirect contract', { concurrency: false }, () => {
  before(async () => {
    root = await mkdtemp(path.join(tmpdir(), 'aria2-task-cookies-'))
    other = createServer(handler)
    otherBase = `http://127.0.0.1:${await listen(other)}`
    http = createServer(handler)
    base = `http://127.0.0.1:${await listen(http)}`
    tls = createTlsServer({ key: await readFile(path.join(fixture, 'key.pem')), cert: await readFile(path.join(fixture, 'cert.pem')) }, handler)
    tlsBase = `https://127.0.0.1:${await listen(tls)}`
  })

  after(async () => {
    for (const server of [http, other, tls]) {
      server?.closeAllConnections()
      if (server) await new Promise(resolve => server.close(resolve))
    }
    if (!process.env.ARIA2_E2E_KEEP_TMP) await rm(root, { recursive: true, force: true })
  })

  it('advertises the explicit RPC methods and downloads with session cookies', async t => {
    const e = await engine(t)
    const methods = await e.rpc.call('system.listMethods')
    assert.ok(methods.includes('aria2.addUriWithCookies'))
    assert.ok(methods.includes('aria2.setTaskCookies'))
    assert.equal((await finished(e, await submit(e, base + '/auth', [cookie()]))).toString(), 'REAL_TEST_FILE')
  })

  it('keeps concurrent same-host accounts and an empty task isolated', async t => {
    const e = await engine(t)
    requests = []
    const values = ['account-A', 'account-B', null]
    await Promise.all(values.map(async (value, i) => {
      await finished(e, await submit(e, base + `/account-${i}`, value ? [cookie(value)] : []))
    }))
    values.forEach((value, i) => {
      const request = requests.find(r => r.path === `/account-${i}`)
      assert.equal(request.cookie, value ? `sid=${value};` : undefined)
    })
  })

  it('uses the structured store instead of inherited or per-task raw Cookie headers', async t => {
    const e = await engine(t)
    await e.rpc.call('aria2.changeGlobalOption', [{ header: ['Cookie: sid=ambient'] }])
    assert.equal((await finished(e, await submit(e, base + '/auth', [cookie()], { header: ['Cookie: sid=raw'] }))).toString(), 'REAL_TEST_FILE')
    requests = []
    await finished(e, await submit(e, base + '/auth', []))
    assert.equal(requests.at(-1).cookie, undefined)
  })

  it('does not inherit startup cookies and does not export task response cookies globally', async t => {
    const jar = path.join(root, 'startup.jar')
    await writeFile(jar, '# Netscape HTTP Cookie File\n127.0.0.1\tFALSE\t/\tFALSE\t0\tambient\tglobal-only\n')
    const e = await engine(t, { startupJar: jar })
    requests = []
    await finished(e, await submit(e, base + '/set-cookie', []))
    assert.equal(requests.find(r => r.path === '/auth').cookie, 'sid=server-only;')
    await finished(e, await submit(e, base + '/empty', []))
    assert.equal(requests.find(r => r.path === '/empty').cookie, undefined)
    const gid = await e.rpc.call('aria2.addUri', [[base + '/legacy'], { out: 'legacy.bin' }])
    await finished(e, { gid, file: path.join(e.dir, 'legacy.bin') })
    assert.equal(requests.find(r => r.path === '/legacy').cookie, 'ambient=global-only;')
  })

  it('matches path and host on every redirect', async t => {
    const e = await engine(t)
    requests = []
    await finished(e, await submit(e, base + '/redirect-same', [cookie()]))
    assert.ok(requests.at(-1).cookie.includes('sid=good'))
    await finished(e, await submit(e, base + '/redirect-host', [cookie()]))
    assert.equal(requests.at(-1).cookie, undefined)
    await finished(e, await submit(e, base + '/redirect-same', [cookie('good', { path: '/redirect-same' })]))
    assert.equal(requests.at(-1).cookie, undefined)
  })

  it('honors secure cookies including an HTTPS to HTTP redirect', async t => {
    const e = await engine(t)
    requests = []
    assert.equal((await finished(e, await submit(e, tlsBase + '/auth', [cookie('good', { secure: true })]))).toString(), 'REAL_TEST_FILE')
    await finished(e, await submit(e, tlsBase + '/redirect-http', [cookie('good', { secure: true })]))
    assert.ok(requests.at(-2).cookie.includes('sid=good'))
    assert.equal(requests.at(-1).cookie, undefined)
  })

  it('uses Unix milliseconds for expiry and omits expired cookies', async t => {
    const e = await engine(t)
    for (const expiresAt of [0, Date.now() - 10000, 1700000000000]) {
      assert.equal((await finished(e, await submit(e, base + '/auth', [cookie('good', { expiresAt })]))).toString(), 'COOKIE_REQUIRED')
    }
    assert.equal((await finished(e, await submit(e, base + '/auth', [cookie('good', { expiresAt: Date.now() + 60000 })]))).toString(), 'REAL_TEST_FILE')
  })

  it('strips only sensitive raw headers on cross-host, cross-port and downgrade redirects', async t => {
    const e = await engine(t)
    for (const [origin, route, keep] of [[base, '/redirect-same', true], [base, '/redirect-host', false], [base, '/redirect-port', false], [tlsBase, '/redirect-http', false]]) {
      requests = []
      const out = `header-${++serial}.bin`
      const gid = await e.rpc.call('aria2.addUri', [[origin + route], { out, header: ['cOoKiE: sid=raw', 'AUTHORIZATION: Bearer synthetic', 'X-Keep: yes'] }])
      await finished(e, { gid, file: path.join(e.dir, out) })
      assert.equal(requests.at(-1).cookie, keep ? 'sid=raw' : undefined)
      assert.equal(requests.at(-1).authorization, keep ? 'Bearer synthetic' : undefined)
      assert.equal(requests.at(-1)['x-keep'], 'yes')
    }
  })

  it('rejects malformed cookies atomically without adding a task or leaking values', async t => {
    const e = await engine(t)
    for (const invalid of [cookie('bad\r\nInjected: yes'), cookie('good', { name: 'bad;name' }), cookie('good', { domain: 'bad/domain' }), cookie('good', { domain: 'bad..domain' }), cookie('good', { path: 'relative' }), cookie('good', { secure: 'true' }), cookie('good', { expiresAt: -1 }), cookie('good', { sameSite: 'lax' }), 'bad', null]) {
      await assert.rejects(e.rpc.call('aria2.addUriWithCookies', [[base + '/auth'], [invalid], {}]))
    }
    await assert.rejects(e.rpc.call('aria2.addUriWithCookies', [['ftp://127.0.0.1/file'], [cookie()], {}]))
    assert.equal((await e.rpc.tellWaiting(0, 100)).length, 0)
    assert.equal((await e.rpc.tellActive()).length, 0)
  })

  it('requires pausing an active task before replacing its cookies and rejects legacy tasks', async t => {
    const e = await engine(t)
    const task = await submit(e, base + '/blob', [cookie()], { 'max-download-limit': '128K' })
    await until(() => e.rpc.tellStatus(task.gid), r => r.status === 'active')
    await assert.rejects(e.rpc.call('aria2.setTaskCookies', [task.gid, []]))
    await e.rpc.forcePause(task.gid)
    await until(() => e.rpc.tellStatus(task.gid), r => r.status === 'paused')
    assert.equal(await e.rpc.call('aria2.setTaskCookies', [task.gid, []]), 'OK')
    const legacy = await e.rpc.call('aria2.addUri', [[base + '/auth'], { pause: 'true' }])
    await assert.rejects(e.rpc.call('aria2.setTaskCookies', [legacy, [cookie()]]))
    await assert.rejects(e.rpc.call('aria2.setTaskCookies', ['ffffffffffffffff', [cookie()]]))
  })

  it('preserves cookies through Range downloads and pause/resume', async t => {
    const e = await engine(t)
    requests = []
    const task = await submit(e, base + '/blob', [cookie()], { split: '4', 'max-connection-per-server': '4', 'min-split-size': '1M', 'max-download-limit': '512K' })
    await until(() => e.rpc.tellStatus(task.gid), r => Number(r.completedLength) > 0)
    await e.rpc.forcePause(task.gid)
    await until(() => e.rpc.tellStatus(task.gid), r => r.status === 'paused')
    await e.rpc.call('aria2.changeOption', [task.gid, { 'max-download-limit': '0' }])
    await e.rpc.unpause(task.gid)
    assert.deepEqual(await finished(e, task), payload)
    assert.ok(requests.some(r => r.range))
    assert.ok(requests.every(r => r.cookie?.includes('sid=good')))
  })

  it('persists only the requirement marker, fails closed after restart, and can rehydrate a paused task', async t => {
    const e = await engine(t)
    const one = await submit(e, base + '/auth', [cookie('NEVER_PERSIST_THIS_COOKIE')], { pause: 'true' })
    const two = await submit(e, base + '/auth', [cookie('NEVER_PERSIST_THIS_COOKIE')], { pause: 'true' })
    await e.rpc.saveSession()
    await e.stop()
    const saved = await readFile(path.join(e.dir, 'session.txt'), 'utf8')
    assert.ok(saved.includes('require-task-cookies=true'))
    for (const filename of ['session.txt', 'session.db', 'engine.log']) {
      assert.ok(!(await readFile(path.join(e.dir, filename))).includes(Buffer.from('NEVER_PERSIST_THIS_COOKIE')))
    }
    const next = await engine(t, { directory: e.dir, restore: true })
    requests = []
    await next.rpc.unpause(one.gid)
    const failed = await until(() => next.rpc.tellStatus(one.gid), r => r.status === 'error')
    assert.match(failed.errorMessage, /cookies.*supplied again/i)
    assert.equal(requests.length, 0)
    assert.equal(await next.rpc.call('aria2.setTaskCookies', [two.gid, [cookie()]]), 'OK')
    await next.rpc.unpause(two.gid)
    assert.equal((await finished(next, two)).toString(), 'REAL_TEST_FILE')
  })
})
