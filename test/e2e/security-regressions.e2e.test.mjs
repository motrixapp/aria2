import { after, before, it } from 'node:test'
import assert from 'node:assert/strict'
import { execFile } from 'node:child_process'
import { mkdtemp, readFile, rm, writeFile } from 'node:fs/promises'
import { createServer } from 'node:https'
import { tmpdir } from 'node:os'
import path from 'node:path'
import { promisify } from 'node:util'
import { gzipSync } from 'node:zlib'
import { allocPorts, defaultAria2Bin, spawnAria2, stopInstance } from './helpers/aria2-process.mjs'
import { Aria2Rpc, waitForRpc } from './helpers/rpc-client.mjs'
import { encode } from './helpers/bencode.mjs'

const exec = promisify(execFile)
const bin = path.resolve(defaultAria2Bin())
const secret = 'security-regression-rpc-secret'
let root, features

const common = [
  '--no-conf=true', '--no-netrc=true', '--check-certificate=true',
  '--all-proxy=', '--http-proxy=', '--https-proxy=', '--ftp-proxy=',
  '--console-log-level=error', '--summary-interval=0', '--max-tries=1',
]

function torrent(pieceLength, length = 1) {
  return encode({ info: {
    name: 'security-test.bin', length, 'piece length': pieceLength,
    pieces: Buffer.alloc(length ? 20 : 0, 0x78),
  } })
}

function run(args) {
  return exec(bin, [...common, ...args], { cwd: root, timeout: 15000 })
}

before(async () => {
  root = await mkdtemp(path.join(tmpdir(), 'aria2-security-'))
  features = (await exec(bin, ['--version'])).stdout
})

after(async () => {
  if (root) await rm(root, { recursive: true, force: true })
})

it('CVE-2026-75421: NUL-prefixed input files exit normally', async () => {
  const content = Buffer.from('\0\n# ignored\n\n\0')
  const inputs = [['input.txt', content]]
  if (features.includes('GZip')) inputs.push(['input.gz', gzipSync(content)])
  for (const [name, data] of inputs) {
    const input = path.join(root, name)
    await writeFile(input, data)
    const result = await run([`--input-file=${input}`])
    assert.doesNotMatch(result.stderr, /Sanitizer|runtime error:/)
  }
})

it('CVE-2026-71832: invalid torrent piece lengths return CLI parse errors', async t => {
  if (!features.includes('BitTorrent')) return t.skip('BitTorrent disabled')
  for (const pieceLength of [0, -1, 2 ** 31, 2 ** 32]) {
    const input = path.join(root, `invalid-${pieceLength}.torrent`)
    await writeFile(input, torrent(pieceLength))
    await assert.rejects(run(['--show-files=true', `--torrent-file=${input}`]), error => {
      assert.equal(error.signal, null, error.stderr)
      assert.equal(error.code, 26, error.stderr)
      assert.match(error.stderr + error.stdout, /errorCode=26/)
      assert.doesNotMatch(error.stderr, /Sanitizer|runtime error:/)
      return true
    })
  }
})

it('invalid RPC torrents leave the engine alive and able to accept valid torrents', async t => {
  if (!features.includes('BitTorrent')) return t.skip('BitTorrent disabled')
  const { rpcPort } = allocPorts()
  const proc = await spawnAria2({ args: [
    ...common, '--enable-rpc=true', '--rpc-listen-all=false',
    `--rpc-listen-port=${rpcPort}`, `--rpc-secret=${secret}`, `--dir=${root}`,
    '--enable-dht=false', '--enable-dht6=false', '--enable-peer-exchange=false',
    '--bt-enable-lpd=false',
  ] })
  t.after(() => stopInstance(proc))
  await waitForRpc(rpcPort)
  const rpc = new Aria2Rpc({ port: rpcPort, secret })
  for (const pieceLength of [0, -1, 2 ** 31, 2 ** 32]) {
    await assert.rejects(rpc.addTorrent(torrent(pieceLength).toString('base64'), {
      pause: 'true',
    }), { aria2Code: 1 })
    assert.ok((await rpc.call('aria2.getVersion')).version)
  }
  const gid = await rpc.addTorrent(torrent(16384).toString('base64'), {
    pause: 'true',
  })
  assert.equal((await rpc.tellStatus(gid)).status, 'paused')
})

it('CVE-2026-8367: trusted clientAuth-only server certificates are rejected', async t => {
  if (!features.includes('HTTPS')) return t.skip('HTTPS disabled')
  const openssl = process.env.OPENSSL_BIN || 'openssl'
  const cert = name => path.join(root, name)
  const opensslRun = args => exec(openssl, args, { cwd: root, timeout: 15000 })
  await opensslRun([
    'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '2',
    '-subj', '/CN=aria2 security test CA', '-keyout', 'ca.key', '-out', 'ca.pem',
    '-addext', 'basicConstraints=critical,CA:TRUE',
    '-addext', 'keyUsage=critical,keyCertSign,cRLSign',
  ])
  await opensslRun([
    'req', '-new', '-newkey', 'rsa:2048', '-nodes', '-subj', '/CN=localhost',
    '-keyout', 'server.key', '-out', 'server.csr',
  ])
  for (const purpose of ['serverAuth', 'clientAuth']) {
    await writeFile(cert(`${purpose}.ext`), [
      'basicConstraints=critical,CA:FALSE',
      'keyUsage=critical,digitalSignature,keyEncipherment',
      `extendedKeyUsage=${purpose}`,
      'subjectAltName=DNS:localhost,IP:127.0.0.1',
    ].join('\n'))
    await opensslRun([
      'x509', '-req', '-in', 'server.csr', '-CA', 'ca.pem', '-CAkey', 'ca.key',
      '-set_serial', purpose === 'serverAuth' ? '1' : '2', '-days', '2',
      '-extfile', `${purpose}.ext`, '-out', `${purpose}.pem`,
    ])
    let requests = 0
    const server = createServer({
      key: await readFile(cert('server.key')),
      cert: await readFile(cert(`${purpose}.pem`)),
    }, (req, res) => {
      requests++
      res.end('verified TLS download')
    })
    server.on('tlsClientError', () => {})
    t.after(() => new Promise(resolve => server.close(resolve)))
    await new Promise((resolve, reject) => {
      server.once('error', reject)
      server.listen(0, '127.0.0.1', resolve)
    })
    const args = [
      `--ca-certificate=${cert('ca.pem')}`, `--dir=${root}`, `--out=${purpose}.txt`,
      '--file-allocation=none', `https://127.0.0.1:${server.address().port}/`,
    ]
    if (purpose === 'serverAuth') {
      await run(args)
      assert.equal(await readFile(cert(`${purpose}.txt`), 'utf8'), 'verified TLS download')
      assert.ok(requests > 0)
    } else {
      await assert.rejects(run(args), error => {
        assert.equal(error.signal, null, error.stderr)
        assert.equal(error.code, 1, error.stderr)
        assert.match(error.stderr + error.stdout, /errorCode=1.*SSL|certificate/i)
        assert.doesNotMatch(error.stderr, /Sanitizer|runtime error:/)
        return true
      })
      assert.equal(requests, 0)
    }
  }
})
