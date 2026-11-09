// Exercise DNS timers through the real select event loop, including on hosts
// which normally choose epoll or kqueue. All DNS and HTTP traffic stays on
// loopback; a bound, silent UDP server models packet loss without ICMP errors.
import { describe, it } from 'node:test'
import assert from 'node:assert/strict'
import { execFile, execFileSync } from 'node:child_process'
import { createSocket } from 'node:dgram'
import { mkdtemp, readFile, rm } from 'node:fs/promises'
import { createServer } from 'node:http'
import { tmpdir } from 'node:os'
import path from 'node:path'
import { defaultAria2Bin } from './helpers/aria2-process.mjs'

const binary = defaultAria2Bin()
const version = execFileSync(binary, ['--version'], { encoding: 'utf8' })
const payload = Buffer.from('select DNS timer regression\n'.repeat(100))

async function dnsServer(t, shouldReply, dualStackIPv4) {
  const socket = createSocket('udp4')
  const queries = []
  const errors = []
  let answeredAAAA = false
  const waitingForAAAA = []
  socket.on('error', error => errors.push(error))
  socket.on('message', (packet, peer) => {
    try {
      assert.equal(packet.readUInt16BE(4), 1, 'expected one DNS question')
      let end = 12
      const labels = []
      while (packet[end] !== 0) {
        const length = packet[end++]
        assert.ok(length > 0 && length <= 63 && end + length < packet.length)
        labels.push(packet.toString('ascii', end, end + length))
        end += length
      }
      end++
      const type = packet.readUInt16BE(end)
      assert.ok(type === 1 || (dualStackIPv4 && type === 28), 'unexpected DNS record type')
      assert.equal(packet.readUInt16BE(end + 2), 1, 'expected Internet class')
      end += 4
      queries.push(labels.join('.').toLowerCase())
      if (!shouldReply(queries.length)) return

      const header = Buffer.alloc(12)
      header.writeUInt16BE(packet.readUInt16BE(0), 0)
      header.writeUInt16BE(0x8180, 2)
      header.writeUInt16BE(1, 4)
      const addresses = type === 28
        ? [Buffer.from('00000000000000000000000000000001', 'hex')]
        : (dualStackIPv4 ?? ['127.0.0.1']).map(ip => Buffer.from(ip.split('.').map(Number)))
      header.writeUInt16BE(addresses.length, 6)
      const answers = addresses.flatMap(addr => [
        Buffer.from([0xc0, 0x0c, 0, type, 0, 1, 0, 0, 0, 1, 0, addr.length]), addr,
      ])
      const send = () => socket.send(Buffer.concat([header, packet.subarray(12, end), ...answers]),
        peer.port, peer.address, error => { if (error) errors.push(error) })
      // The engine may proceed immediately on an A answer. Deliver AAAA
      // first in dual-stack tests, then assert its actual connection order.
      if (dualStackIPv4 && type === 1 && !answeredAAAA) waitingForAAAA.push(send)
      else {
        send()
        if (type === 28) {
          answeredAAAA = true
          for (const pending of waitingForAAAA.splice(0)) pending()
        }
      }
    } catch (error) {
      errors.push(error)
    }
  })
  await new Promise((resolve, reject) => {
    socket.once('error', reject)
    socket.bind(0, '127.0.0.1', resolve)
  })
  t.after(() => new Promise(resolve => socket.close(resolve)))
  t.after(() => assert.deepEqual(errors, [], 'DNS fixture errors'))
  return { address: `127.0.0.1:${socket.address().port}`, queries }
}

const dualStackListener = process.platform === 'win32' ? '127.0.0.2' : '127.0.0.1'

async function download(t, servers, dualStack = false, reachable = true) {
  const dir = await mkdtemp(path.join(tmpdir(), 'aria2-select-dns-'))
  t.after(() => rm(dir, { recursive: true, force: true }))
  let httpRequests = 0
  const http = createServer((_req, res) => {
    httpRequests++
    res.writeHead(200, { 'content-length': payload.length })
    res.end(payload)
  })
  await new Promise((resolve, reject) => {
    http.once('error', reject)
    http.listen(0, dualStack ? dualStackListener : '127.0.0.1', resolve)
  })
  const port = http.address().port
  const close = () => new Promise(resolve => {
    http.closeAllConnections()
    http.close(resolve)
  })
  if (reachable) t.after(close)
  else await close()

  const env = Object.fromEntries(Object.entries(process.env).filter(([key]) =>
    !/^(?:http_proxy|https_proxy|ftp_proxy|all_proxy|no_proxy|localdomain)$/i.test(key)))
  // Only the child gets shorter c-ares timers. Production defaults stay intact.
  env.RES_OPTIONS = 'timeout:1 attempts:2'
  env.LC_ALL = 'C'
  const args = [
    '--no-conf=true', '--no-netrc=true', '--event-poll=select',
    '--async-dns=true', `--async-dns-server=${servers.map(server => server.address).join(',')}`,
    `--disable-ipv6=${!dualStack}`, '--all-proxy=', '--http-proxy=', '--https-proxy=',
    ...(version.includes('BitTorrent') ? ['--enable-dht=false', '--enable-dht6=false'] : []),
    '--max-tries=1', '--retry-wait=0', '--split=1', '--max-connection-per-server=1',
    '--connect-timeout=5', '--timeout=5', '--file-allocation=none', '--summary-interval=0',
    `--console-log-level=${dualStack ? 'info' : 'notice'}`, '--out=payload.bin', `--dir=${dir}`,
    `http://select-dns.test.:${port}/payload.bin`,
  ]
  const result = await new Promise(resolve => {
    execFile(binary, args, { env, encoding: 'utf8', timeout: 15000 }, (error, stdout, stderr) => {
      resolve({ error, output: stdout + stderr })
    })
  })
  assert.ok(!result.error?.killed,
    `DNS did not finish within 15 seconds; queries=${JSON.stringify(servers.map(server => server.queries))}\n${result.output}`)
  return { ...result, dir, httpRequests }
}

describe('select asynchronous DNS timers', { concurrency: false }, () => {
  it('retransmits a lost DNS request and completes the download', async t => {
    const dns = await dnsServer(t, count => count > 1)
    const result = await download(t, [dns])
    assert.ifError(result.error)
    assert.ok(dns.queries.length >= 2, 'the dropped query must be retransmitted')
    assert.ok(dns.queries.every(name => name === 'select-dns.test'))
    assert.equal(result.httpRequests, 1)
    assert.deepEqual(await readFile(path.join(result.dir, 'payload.bin')), payload)
  })

  it('switches from a silent primary DNS server to a working backup', async t => {
    const primary = await dnsServer(t, () => false)
    const backup = await dnsServer(t, () => true)
    const result = await download(t, [primary, backup])
    assert.ifError(result.error)
    assert.ok(primary.queries.length >= 1, 'the primary must be tried')
    assert.ok(backup.queries.length >= 1, 'the backup must be tried after the primary times out')
    assert.equal(result.httpRequests, 1)
    assert.deepEqual(await readFile(path.join(result.dir, 'payload.bin')), payload)
  })

  it('reports a DNS error when every server is silent, before the outer download timeout', async t => {
    const primary = await dnsServer(t, () => false)
    const backup = await dnsServer(t, () => false)
    const result = await download(t, [primary, backup])
    assert.equal(result.error?.code, 19, result.output)
    assert.match(result.output, /Name resolution .* failed:Timeout while contacting DNS servers/)
    assert.ok(primary.queries.length >= 2, 'the primary must be retried')
    assert.ok(backup.queries.length >= 2, 'the backup must be retried')
    assert.equal(result.httpRequests, 0, 'HTTP must not start without a DNS answer')
  })
})

if (process.env.ARIA2_TEST_DUAL_STACK === '1') {
  describe('select connect address failover', () => {
    for (const reachable of [true, false]) {
      it(reachable ? 'tries the remaining IPv4 address in one URI attempt' :
        'reports a network error when all connection candidates fail', async t => {
        // The first IPv4 candidate is also closed: a racing backup attempt
        // cannot mask failure to rotate the main connection's cached address.
        const addresses = reachable && process.platform === 'win32'
          ? ['127.0.0.1', dualStackListener] : ['127.0.0.1']
        const dns = await dnsServer(t, () => true, addresses)
        const result = await download(t, [dns], true, reachable)
        const tried = [...result.output.matchAll(/ - Connecting to (.+):\d+/g)].map(m => m[1])
        assert.deepEqual(tried, ['::1', ...addresses], result.output)
        if (reachable) {
          assert.ifError(result.error)
          assert.equal(result.httpRequests, 1)
          assert.deepEqual(await readFile(path.join(result.dir, 'payload.bin')), payload)
        } else {
          assert.equal(result.error?.code, 6, result.output)
          assert.equal(result.httpRequests, 0)
        }
      })
    }
  })
}
