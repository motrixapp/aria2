// Cloud-drive RPC clients send paths such as /album/file in `out`.
// Like upstream aria2, HTTP/FTP output paths are relative to `dir`, even
// with a leading slash (agalwood/Motrix#2502).
import { after, before, describe, it } from 'node:test'
import assert from 'node:assert/strict'
import { mkdtemp, readFile, rm } from 'node:fs/promises'
import http from 'node:http'
import os from 'node:os'
import path from 'node:path'

import { allocPorts, startInstance } from './helpers/aria2-process.mjs'
import { encode } from './helpers/bencode.mjs'
import { Aria2Rpc } from './helpers/rpc-client.mjs'
import { waitFor } from './helpers/wait.mjs'

describe('nested HTTP output paths', { timeout: 30_000 }, () => {
  const payload = Buffer.from('cloud-drive nested output\n')
  let workDir, server, handle, rpc, url, options

  before(async () => {
    workDir = await mkdtemp(path.join(os.tmpdir(), 'a2-nested-output-'))
    server = http.createServer((req, res) => {
      res.writeHead(200, { 'Content-Length': payload.length })
      res.end(req.method === 'HEAD' ? undefined : payload)
    })
    await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve))
    url = `http://127.0.0.1:${server.address().port}/file.bin`
    options = {
      ...allocPorts(),
      rpcSecret: 'nested-output',
      dir: path.join(workDir, 'downloads'),
      sessionPath: path.join(workDir, 'state', 'aria2.session'),
      sqliteDbPath: path.join(workDir, 'state', 'aria2.db'),
      logPath: path.join(workDir, 'state', 'aria2.log'),
      extra: [
        '--no-conf=true', '--no-netrc=true',
        '--enable-dht=false', '--enable-dht6=false',
        '--all-proxy=', '--http-proxy=', '--https-proxy=', '--ftp-proxy=',
      ],
    }
    handle = await startInstance(options)
    rpc = new Aria2Rpc({ port: options.rpcPort, secret: options.rpcSecret })
  })

  after(async () => {
    await handle?.stop()
    if (server) await new Promise((resolve) => server.close(resolve))
    if (workDir && !process.env.ARIA2_E2E_KEEP_TMP) {
      await rm(workDir, { recursive: true, force: true })
    }
  })

  async function expectDestination(gid, target) {
    const files = await rpc.call('aria2.getFiles', [gid])
    assert.equal(path.resolve(files[0].path), path.resolve(target))
  }

  async function complete(gid, target) {
    // Check before unpausing: a broken engine must never write outside the
    // test's chosen directory while reproducing this regression.
    await expectDestination(gid, target)
    await rpc.call('aria2.unpause', [gid])
    await waitFor(async () => {
      const status = await rpc.call('aria2.tellStatus', [gid, ['status', 'errorCode', 'errorMessage']])
      assert.notEqual(status.status, 'error', JSON.stringify(status))
      return status.status === 'complete'
    }, 10_000, 50, 'nested download to complete')
    assert.deepEqual(await readFile(target), payload)
  }

  for (const out of ['relative/album/file.bin', '/网盘/多层目录/file.bin', '//cloud/album/file.bin']) {
    it(`creates directories under the default dir for out=${out}`, async () => {
      const gid = await rpc.call('aria2.addUri', [[url], { out, pause: 'true' }])
      await complete(gid, path.join(options.dir, out))
    })
  }

  it('uses an explicit dir that does not exist yet', async () => {
    const dir = path.join(workDir, 'custom', 'nested')
    const out = '/cloud/file.bin'
    const gid = await rpc.call('aria2.addUri', [[url], { dir, out, pause: 'true' }])
    await complete(gid, path.join(dir, out))
  })

  it('keeps changed out and dir options relative before starting', async () => {
    const gid = await rpc.call('aria2.addUri', [[url], { out: 'initial.bin', pause: 'true' }])
    const out = '/changed/album/file.bin'
    await rpc.call('aria2.changeOption', [gid, { out }])
    await expectDestination(gid, path.join(options.dir, out))
    const dir = path.join(workDir, 'changed-directory')
    await rpc.call('aria2.changeOption', [gid, { dir }])
    await complete(gid, path.join(dir, out))
  })

  it('restores the relative output after a SQLite session restart', async () => {
    const out = '/restored/album/file.bin'
    const gid = await rpc.call('aria2.addUri', [[url], { out, pause: 'true' }])
    const target = path.join(options.dir, out)
    await expectDestination(gid, target)
    await handle.stop()
    handle = await startInstance(options)
    await complete(gid, target)
  })

  it('preserves absolute BitTorrent index-out mappings', async (t) => {
    const version = await rpc.call('aria2.getVersion')
    if (!version.enabledFeatures.includes('BitTorrent')) {
      t.skip('engine built without BitTorrent')
      return
    }
    const metadata = encode({ info: {
      name: 'original.bin',
      length: 1,
      'piece length': 16384,
      pieces: Buffer.alloc(20, 1),
    } })
    const target = path.join(workDir, 'mapped', 'file.bin')
    const gid = await rpc.call('aria2.addTorrent', [metadata.toString('base64'), [], {
      'index-out': `1=${target}`,
      pause: 'true',
    }])
    await expectDestination(gid, target)
    const changed = path.join(workDir, 'remapped', 'file.bin')
    await rpc.call('aria2.changeOption', [gid, { 'index-out': `1=${changed}` }])
    await expectDestination(gid, changed)
  })
})
