import { after, before, describe, it } from 'node:test'
import assert from 'node:assert/strict'
import { execFileSync } from 'node:child_process'
import { createServer } from 'node:http'
import { mkdtemp, mkdir, readFile, rm, truncate } from 'node:fs/promises'
import { tmpdir } from 'node:os'
import path from 'node:path'
import { DatabaseSync } from 'node:sqlite'
import { allocPorts, defaultAria2Bin, spawnAria2, stopInstance } from './helpers/aria2-process.mjs'
import { Aria2Rpc } from './helpers/rpc-client.mjs'

const payload = Buffer.alloc(4 * 1024 * 1024, 0x6d)
const secret = 'synthetic-http-retry-secret'
let root, server, base, serial = 0
const routes = new Map()
const requests = []
const cookies = [{ name: 'sid', value: 'synthetic', domain: '127.0.0.1', path: '/' }]
async function until(get, predicate, timeout = 15000) {
  const deadline = Date.now() + timeout
  let value
  do {
    value = await get()
    if (predicate(value)) return value
    await new Promise(resolve => setTimeout(resolve, 30))
  } while (Date.now() < deadline)
  assert.fail(`Timed out: ${JSON.stringify(value)}`)
}
function sql(engine, query, ...params) {
  const db = new DatabaseSync(path.join(engine.dir, 'aria2.db'))
  try {
    db.exec('PRAGMA busy_timeout=5000')
    return db.prepare(query).all(...params)
  } finally { db.close() }
}
async function engine(t, directory, extra = []) {
  const dir = directory ?? path.join(root, `engine-${++serial}`)
  await mkdir(dir, { recursive: true })
  const { rpcPort } = allocPorts()
  const proc = await spawnAria2({ args: [
    '--no-conf=true', '--no-netrc=true', '--enable-rpc=true', '--rpc-listen-all=false',
    `--rpc-listen-port=${rpcPort}`, `--rpc-secret=${secret}`, '--disable-ipv6=true',
    '--all-proxy=', '--http-proxy=', '--https-proxy=', '--summary-interval=0',
    '--split=1', '--max-connection-per-server=1', '--max-tries=1', '--retry-wait=0',
    '--file-allocation=none', '--auto-file-renaming=false', '--allow-overwrite=false',
    '--force-save=true', '--enable-sqlite3-persistence=true', '--sqlite3-history-limit=-1',
    `--sqlite3-db-path=${path.join(dir, 'aria2.db')}`, `--dir=${dir}`,
    `--save-session=${path.join(dir, 'aria2.session')}`, '--save-session-interval=1',
    ...(execFileSync(defaultAria2Bin(), ['--version'], { encoding: 'utf8' }).includes('BitTorrent')
      ? ['--enable-dht=false', '--enable-dht6=false', '--bt-enable-lpd=false'] : []), ...extra,
  ] })
  t.after(() => stopInstance(proc))
  const rpc = new Aria2Rpc({ port: rpcPort, secret })
  await until(() => rpc.call('aria2.getVersion').catch(() => null), Boolean)
  return { dir, rpc, stop: () => stopInstance(proc) }
}
async function interrupted(engine, jar = cookies, options = {}, responseOptions = {}) {
  const route = `/file-${++serial}`
  const state = { drop: true, etag: '"original-entity"', body: payload, ...responseOptions }
  routes.set(route, state)
  const file = path.join(engine.dir, `partial-${serial}.bin`)
  const uri = base + route
  const gid = await engine.rpc.call('aria2.addUriWithCookies', [[uri], jar, {
    out: path.basename(file), 'http-resume-context': 'true', ...options,
  }])
  const status = await until(() => engine.rpc.tellStatus(gid), s => s.status === 'error')
  assert.ok(Number(status.completedLength) > 0)
  assert.ok(Number(status.completedLength) < payload.length)
  const before = await readFile(file)
  return { gid, file, uri, route, state, before }
}
const availability = (e, task) => e.rpc.call('aria2.getHttpRetryStatusV1', [task.gid, task.uri, task.file])
const retry = (e, task) => e.rpc.call('aria2.retryHttpDownloadV1', [task.gid, task.uri, task.file])

describe('HTTP retry preserves the original task and resource', { concurrency: false }, () => {
  before(async () => {
    root = await mkdtemp(path.join(tmpdir(), 'aria2-http-retry-'))
    server = createServer((req, res) => {
      requests.push({ path: req.url, range: req.headers.range, cookie: req.headers.cookie })
      const state = routes.get(req.url)
      if (!state) return res.writeHead(404).end()
      if (state.redirect) return res.writeHead(302, { location: state.redirect, 'content-length': 0 }).end()
      const range = /^bytes=(\d+)-(\d*)$/.exec(req.headers.range ?? '')
      const offset = range && !state.ignoreRange ? Number(range[1]) : 0
      const end = range?.[2] && !state.ignoreRange ? Number(range[2]) : state.body.length - 1
      const body = state.body.subarray(offset, end + 1)
      res.writeHead(range && !state.ignoreRange ? 206 : 200, {
        'content-length': body.length, 'accept-ranges': 'bytes',
        ...(state.etag ? { etag: state.etag } : {}),
        ...(state.lastModified ? { 'last-modified': state.lastModified } : {}),
        ...(state.link ? { link: state.link } : {}),
        ...(range && !state.ignoreRange ? { 'content-range': `bytes ${offset}-${end}/${state.body.length}` } : {}),
      })
      if (state.drop) {
        res.write(body.subarray(0, 512 * 1024))
        setTimeout(() => res.destroy(), 100)
      } else res.end(body)
    })
    await new Promise(resolve => server.listen(0, '127.0.0.1', resolve))
    base = `http://127.0.0.1:${server.address().port}`
  })
  after(async () => {
    server.closeAllConnections()
    await new Promise(resolve => server.close(resolve))
    if (!process.env.ARIA2_E2E_KEEP_TMP) await rm(root, { recursive: true, force: true })
  })

  for (const [label, jar] of [['populated', cookies], ['empty', []]]) {
    it(`resumes ${label} cookie context after failure with the same GID`, async t => {
      const e = await engine(t)
      const task = await interrupted(e, jar)
      assert.deepEqual(await availability(e, task), { available: 'true', reason: 'ready' })
      task.state.drop = false
      const mark = requests.length
      assert.equal(await retry(e, task), task.gid)
      await until(() => e.rpc.tellStatus(task.gid), s => s.status === 'complete')
      assert.deepEqual(await readFile(task.file), payload)
      const continued = requests.slice(mark)
      assert.ok(continued.some(r => /^bytes=[1-9]\d*-/.test(r.range ?? '')))
      assert.ok(continued.every(r => jar.length ? r.cookie === 'sid=synthetic;' : r.cookie === undefined))
      assert.equal(sql(e, "SELECT count(*) n FROM download_history WHERE gid=? AND status='error'", task.gid)[0].n, 0)
      assert.equal(sql(e, 'SELECT count(*) n FROM task_cookie_context WHERE gid=?', task.gid)[0].n, 0)
    })

    it(`restores failed ${label} context paused across engine restart`, async t => {
      const first = await engine(t)
      const task = await interrupted(first, jar)
      await first.rpc.saveSession()
      await first.stop()
      task.state.drop = false
      const mark = requests.length
      const second = await engine(t, first.dir)
      assert.equal((await second.rpc.tellStatus(task.gid)).status, 'paused')
      assert.equal(requests.length, mark)
      assert.equal(await retry(second, task), task.gid)
      await until(() => second.rpc.tellStatus(task.gid), s => s.status === 'complete')
      assert.deepEqual(await readFile(task.file), payload)
    })
  }

  for (const mutation of ['etag', 'range', 'redirect']) {
    it(`refuses ${mutation} changes without altering retained bytes`, async t => {
      const e = await engine(t)
      const task = await interrupted(e)
      task.state.drop = false
      if (mutation === 'etag') {
        task.state.etag = '"changed-entity"'
        task.state.body = Buffer.alloc(payload.length, 99)
      } else if (mutation === 'range') task.state.ignoreRange = true
      else {
        const destination = task.route + '-moved'
        routes.set(destination, { ...task.state })
        task.state.redirect = base + destination
      }
      await retry(e, task)
      const status = await until(() => e.rpc.tellStatus(task.gid), s => s.status === 'error')
      assert.equal(status.errorCode, '8')
      assert.deepEqual(await readFile(task.file), task.before)
    })
  }

  it('does not invent a historical validator for an existing partial file', async t => {
    const e = await engine(t)
    const task = await interrupted(e, cookies, { 'http-resume-context': 'false' })
    assert.deepEqual(await availability(e, task), { available: 'false', reason: 'resource-identity-missing' })
    await assert.rejects(retry(e, task), /resource-identity-missing/)
    assert.deepEqual(await readFile(task.file), task.before)
  })

  it('captures one source identity for normal multi-connection downloads', async t => {
    const e = await engine(t)
    const task = await interrupted(e, cookies, { split: '16', 'max-connection-per-server': '16', 'min-split-size': '1M' })
    assert.equal((await availability(e, task)).available, 'true')
    task.state.drop = false
    await retry(e, task)
    await until(() => e.rpc.tellStatus(task.gid), s => s.status === 'complete')
    assert.deepEqual(await readFile(task.file), payload)
  })

  it('resumes the checkpoint rather than treating a preallocated file length as completed bytes', async t => {
    const e = await engine(t)
    const task = await interrupted(e, cookies, { 'file-allocation': 'prealloc' })
    // Some filesystems reserve space without extending logical length. Model
    // the full-length allocation case explicitly while keeping the checkpoint.
    await truncate(task.file, payload.length)
    const allocated = await readFile(task.file)
    assert.equal(allocated.length, payload.length)
    assert.notDeepEqual(allocated, payload)
    task.state.drop = false
    await retry(e, task)
    await until(() => e.rpc.tellStatus(task.gid), s => s.status === 'complete')
    assert.deepEqual(await readFile(task.file), payload)
  })

  it('does not let HTTP Link mirrors bypass the stored resource identity', async t => {
    const e = await engine(t)
    const task = await interrupted(e)
    task.state.drop = false
    task.state.etag = '"changed-entity"'
    task.state.link = `<${base}/unrelated-mirror>; rel=duplicate`
    const mark = requests.length
    await retry(e, task)
    await until(() => e.rpc.tellStatus(task.gid), s => s.status === 'error')
    assert.deepEqual(await readFile(task.file), task.before)
    assert.ok(requests.slice(mark).every(r => r.path === task.route))
  })

  it('checks the actual paused source before sending any retained credentials', async t => {
    const first = await engine(t)
    const task = await interrupted(first, cookies, { header: 'Authorization: Bearer synthetic' })
    await first.stop()
    const second = await engine(t, first.dir)
    await second.rpc.call('aria2.changeUri', [task.gid, 1, [task.uri], [base + '/different-source']])
    const mark = requests.length
    await assert.rejects(retry(second, task), /binding-mismatch/)
    assert.equal(requests.length, mark)
    // Ordinary unpause must enforce the same binding before sending credentials.
    await second.rpc.unpause(task.gid)
    await until(() => second.rpc.tellStatus(task.gid), s => s.status === 'error')
    assert.equal(requests.length, mark)
    assert.deepEqual(await readFile(task.file), task.before)
  })

  it('requires a checkpoint for restored paused downloads too', async t => {
    const first = await engine(t)
    const task = await interrupted(first)
    await first.stop()
    const second = await engine(t, first.dir)
    sql(second, 'DELETE FROM task_progress')
    await assert.rejects(retry(second, task), /checkpoint-missing/)
    assert.deepEqual(await readFile(task.file), task.before)
  })

  it('constrains restored options and makes retry acknowledgements idempotent', async t => {
    const first = await engine(t)
    const task = await interrupted(first)
    await first.stop()
    const second = await engine(t, first.dir)
    await second.rpc.call('aria2.changeOption', [task.gid, {
      'allow-overwrite': 'true', 'auto-file-renaming': 'true', 'always-resume': 'false',
      'conditional-get': 'true', 'continue': 'true', 'max-download-limit': '256K',
      header: 'If-None-Match: "old-user-condition"',
    }])
    task.state.drop = false
    const mark = requests.length
    assert.equal(await retry(second, task), task.gid)
    assert.equal(await retry(second, task), task.gid)
    const options = await second.rpc.call('aria2.getOption', [task.gid])
    assert.equal(options['allow-overwrite'], 'false')
    assert.equal(options['auto-file-renaming'], 'false')
    assert.equal(options['always-resume'], 'true')
    assert.equal(options['conditional-get'], 'false')
    await second.rpc.call('aria2.changeOption', [task.gid, { 'max-download-limit': '0' }])
    await until(() => second.rpc.tellStatus(task.gid), s => s.status === 'complete')
    assert.deepEqual(await readFile(task.file), payload)
    assert.ok(requests.slice(mark).some(r => /^bytes=[1-9]\d*-/.test(r.range ?? '')))
    const completedRequests = requests.length
    assert.equal(await retry(second, task), task.gid)
    assert.equal(requests.length, completedRequests)
  })

  for (const changed of [false, true]) {
    it(`checks the stored Last-Modified validator when changed=${changed}`, async t => {
      const e = await engine(t)
      const task = await interrupted(e, cookies, {}, { etag: null, lastModified: 'Wed, 21 Oct 2026 07:28:00 GMT' })
      task.state.drop = false
      task.state.etag = '"newly-present-etag"'
      if (changed) task.state.lastModified = 'Thu, 22 Oct 2026 07:28:00 GMT'
      await retry(e, task)
      await until(() => e.rpc.tellStatus(task.gid), s => s.status === (changed ? 'error' : 'complete'))
      assert.deepEqual(await readFile(task.file), changed ? task.before : payload)
    })
  }

  it('rejects changed caller bindings and preserves the original credential context', async t => {
    const e = await engine(t)
    const task = await interrupted(e)
    await assert.rejects(retry(e, { ...task, file: task.file + '.other' }), /binding-mismatch/)
    await assert.rejects(retry(e, { ...task, uri: task.uri + '?other' }), /binding-mismatch/)
    assert.equal(sql(e, 'SELECT count(*) n FROM task_cookie_context WHERE gid=?', task.gid)[0].n, 1)
    assert.deepEqual(await readFile(task.file), task.before)
  })

  it('leaves failed state intact if the durable retry transaction fails', async t => {
    const e = await engine(t)
    const task = await interrupted(e)
    sql(e, "CREATE TRIGGER reject_retry BEFORE UPDATE ON task BEGIN SELECT RAISE(FAIL, 'synthetic retry failure'); END")
    await assert.rejects(retry(e, task), /UPSERT task failed/)
    assert.equal((await e.rpc.tellStatus(task.gid)).status, 'error')
    assert.equal(sql(e, 'SELECT count(*) n FROM task_cookie_context WHERE gid=?', task.gid)[0].n, 1)
    assert.deepEqual(await readFile(task.file), task.before)
    sql(e, 'DROP TRIGGER reject_retry')
    task.state.drop = false
    await retry(e, task)
    await until(() => e.rpc.tellStatus(task.gid), s => s.status === 'complete')
    assert.deepEqual(await readFile(task.file), payload)
  })

  it('reuses the registered GID after FIFO eviction and cancels only its pending history write', async t => {
    const e = await engine(t, undefined, ['--max-download-result=1', '--keep-unfinished-download-result=true'])
    sql(e, "CREATE TRIGGER reject_history BEFORE INSERT ON download_history BEGIN SELECT RAISE(FAIL, 'synthetic history failure'); END")
    const first = await interrupted(e)
    const other = await interrupted(e)
    sql(e, 'DROP TRIGGER reject_history')
    first.state.drop = false
    assert.equal((await availability(e, first)).available, 'true')
    assert.equal(await retry(e, first), first.gid)
    await until(() => e.rpc.tellStatus(first.gid), s => s.status === 'complete')
    assert.deepEqual(await readFile(first.file), payload)
    assert.equal(sql(e, "SELECT count(*) n FROM download_history WHERE gid=? AND status='error'", first.gid)[0].n, 0)
    assert.equal(sql(e, "SELECT count(*) n FROM download_history WHERE gid=? AND status='error'", other.gid)[0].n, 1)
    assert.equal(sql(e, 'SELECT count(*) n FROM task_cookie_context WHERE gid=?', other.gid)[0].n, 1)
    assert.equal(await retry(e, first), first.gid)
  })

  it('keeps durable failed contexts after FIFO eviction without an unfinished RAM cache', async t => {
    const e = await engine(t, undefined, ['--max-download-result=1', '--keep-unfinished-download-result=false'])
    const first = await interrupted(e)
    await interrupted(e)
    await e.rpc.saveSession()
    assert.equal(sql(e, 'SELECT count(*) n FROM task_cookie_context WHERE gid=?', first.gid)[0].n, 1)
    first.state.drop = false
    assert.equal(await retry(e, first), first.gid)
    await until(() => e.rpc.tellStatus(first.gid), s => s.status === 'complete')
    assert.deepEqual(await readFile(first.file), payload)
  })

  it('purges stopped contexts atomically while preserving a restored paused owner', async t => {
    const first = await engine(t)
    const paused = await interrupted(first)
    await first.stop()
    const e = await engine(t, first.dir)
    sql(e, "CREATE TRIGGER reject_history BEFORE INSERT ON download_history BEGIN SELECT RAISE(FAIL, 'synthetic history failure'); END")
    const stopped = await interrupted(e)
    sql(e, "CREATE TRIGGER reject_purge BEFORE DELETE ON task BEGIN SELECT RAISE(FAIL, 'synthetic purge failure'); END")
    await assert.rejects(e.rpc.call('aria2.purgeDownloadResult'), /stopped purge failed/)
    assert.equal((await e.rpc.tellStatus(stopped.gid)).status, 'error')
    for (const task of [paused, stopped]) assert.equal(sql(e, 'SELECT count(*) n FROM task_cookie_context WHERE gid=?', task.gid)[0].n, 1)
    sql(e, 'DROP TRIGGER reject_purge')
    await e.rpc.call('aria2.purgeDownloadResult')
    assert.equal(sql(e, 'SELECT count(*) n FROM task_cookie_context WHERE gid=?', stopped.gid)[0].n, 0)
    assert.equal(sql(e, 'SELECT count(*) n FROM task_cookie_context WHERE gid=?', paused.gid)[0].n, 1)
    assert.deepEqual(await readFile(stopped.file), stopped.before)
    sql(e, 'DROP TRIGGER reject_history')
    paused.state.drop = false
    assert.equal(await retry(e, paused), paused.gid)
    await until(() => e.rpc.tellStatus(paused.gid), s => s.status === 'complete')
    assert.deepEqual(await readFile(paused.file), payload)
    assert.equal(sql(e, 'SELECT count(*) n FROM download_history WHERE gid=?', stopped.gid)[0].n, 0)
  })

  it('does not reinsert a removed result from the pending history queue', async t => {
    const e = await engine(t)
    sql(e, "CREATE TRIGGER reject_history BEFORE INSERT ON download_history BEGIN SELECT RAISE(FAIL, 'synthetic history failure'); END")
    const removed = await interrupted(e)
    await e.rpc.removeDownloadResult(removed.gid)
    sql(e, 'DROP TRIGGER reject_history')
    removed.state.drop = false
    const other = await e.rpc.call('aria2.addUriWithCookies', [[removed.uri], [], { out: 'another-file.bin' }])
    await until(() => e.rpc.tellStatus(other), s => s.status === 'complete')
    assert.equal(sql(e, 'SELECT count(*) n FROM task WHERE gid=?', removed.gid)[0].n, 0)
    assert.equal(sql(e, 'SELECT count(*) n FROM download_history WHERE gid=?', removed.gid)[0].n, 0)
    assert.deepEqual(await readFile(removed.file), removed.before)
  })
})
