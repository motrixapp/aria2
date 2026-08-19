#!/usr/bin/env node

import { spawn } from 'node:child_process'
import { createHash } from 'node:crypto'
import { once } from 'node:events'
import { mkdtemp, readFile, rm } from 'node:fs/promises'
import { createServer } from 'node:http'
import { tmpdir } from 'node:os'
import path from 'node:path'
import { performance } from 'node:perf_hooks'
import { setTimeout as delay } from 'node:timers/promises'

const SIZE_MIB = Number.parseInt(process.env.ARIA2_BENCH_SIZE_MIB ?? '16', 10)
if (!Number.isSafeInteger(SIZE_MIB) || SIZE_MIB <= 0) {
  throw new Error('ARIA2_BENCH_SIZE_MIB must be a positive integer')
}
const TOTAL_LENGTH = SIZE_MIB * 1024 * 1024
const WRITE_CHUNK_LENGTH = 256 * 1024
const STALL_WRITE_LENGTH = 64 * 1024
const BYTE_VALUE = 0x5a
const PROCESS_TIMEOUT_MS = 45_000
const KEEP_TEMP = process.env.ARIA2_BENCH_KEEP_TMP === '1'
const STALL_ENABLED = process.env.ARIA2_BENCH_STALL !== '0'
const MIBPS_PER_CONNECTION = Number(process.env.ARIA2_BENCH_MIBPS_PER_CONNECTION ?? '0')
if (!Number.isFinite(MIBPS_PER_CONNECTION) || MIBPS_PER_CONNECTION < 0) {
  throw new Error('ARIA2_BENCH_MIBPS_PER_CONNECTION must be a non-negative number')
}
const BYTES_PER_MS = MIBPS_PER_CONNECTION * 1024 * 1024 / 1000

function parseBinaries(argv) {
  const binaries = []
  for (let i = 0; i < argv.length; ++i) {
    if (argv[i] !== '--bin' || !argv[i + 1]) {
      throw new Error('Usage: http-tail-reclaim.bench.mjs --bin label=/path/to/aria2c [...]')
    }
    const value = argv[++i]
    const separator = value.indexOf('=')
    if (separator <= 0 || separator === value.length - 1) {
      throw new Error(`Invalid --bin value: ${value}`)
    }
    binaries.push({ label: value.slice(0, separator), bin: value.slice(separator + 1) })
  }
  if (binaries.length === 0) {
    throw new Error('At least one --bin label=/path/to/aria2c argument is required')
  }
  return binaries
}

function parseRange(value) {
  const match = /^bytes=(\d+)-(\d*)$/.exec(value ?? '')
  if (!match) return { start: 0, end: TOTAL_LENGTH - 1, partial: false }
  const start = Number(match[1])
  const requestedEnd = match[2] ? Number(match[2]) : TOTAL_LENGTH - 1
  return {
    start,
    end: Math.min(requestedEnd, TOTAL_LENGTH - 1),
    partial: true,
  }
}

async function writeBytes(response, length) {
  const chunk = Buffer.alloc(WRITE_CHUNK_LENGTH, BYTE_VALUE)
  const startedAt = performance.now()
  let remaining = length
  let written = 0
  while (remaining > 0) {
    const size = Math.min(chunk.length, remaining)
    if (!response.write(size === chunk.length ? chunk : chunk.subarray(0, size))) {
      await once(response, 'drain')
    }
    remaining -= size
    written += size
    if (BYTES_PER_MS > 0 && remaining > 0) {
      const waitMs = written / BYTES_PER_MS - (performance.now() - startedAt)
      if (waitMs > 0) await delay(waitMs)
    }
  }
  response.end()
}

async function startRangeServer() {
  let stalledOnce = false
  const requests = []
  const sockets = new Set()
  let startedAt = performance.now()
  const server = createServer(async (request, response) => {
    const { start, end, partial } = parseRange(request.headers.range)
    if (start >= TOTAL_LENGTH || end < start) {
      response.writeHead(416, { 'Content-Range': `bytes */${TOTAL_LENGTH}` })
      response.end()
      return
    }

    const length = end - start + 1
    const headers = {
      'Accept-Ranges': 'bytes',
      'Content-Length': String(length),
      'Content-Type': 'application/octet-stream',
      ETag: '"aria2-tail-reclaim-benchmark"',
    }
    if (partial) headers['Content-Range'] = `bytes ${start}-${end}/${TOTAL_LENGTH}`

    if (request.method === 'HEAD') {
      requests.push({
        atMs: Math.round(performance.now() - startedAt),
        method: 'HEAD',
        start,
        end,
        action: 'headers',
      })
      response.writeHead(200, headers)
      response.end()
      return
    }

    const shouldStall = STALL_ENABLED && partial && start > 0 && !stalledOnce
    requests.push({
      atMs: Math.round(performance.now() - startedAt),
      method: request.method,
      start,
      end,
      action: shouldStall ? 'stall' : 'serve',
    })
    response.writeHead(partial ? 206 : 200, headers)
    if (shouldStall) {
      stalledOnce = true
      response.write(Buffer.alloc(Math.min(STALL_WRITE_LENGTH, length), BYTE_VALUE))
      return
    }

    try {
      await writeBytes(response, length)
    } catch (error) {
      if (error?.code !== 'ECONNRESET' && error?.code !== 'EPIPE') throw error
    }
  })
  server.on('connection', (socket) => {
    sockets.add(socket)
    socket.on('close', () => sockets.delete(socket))
  })
  server.listen(0, '127.0.0.1')
  await once(server, 'listening')
  const address = server.address()
  return {
    url: `http://127.0.0.1:${address.port}/payload.bin`,
    requests,
    resetMeasurements() {
      requests.length = 0
      stalledOnce = false
      startedAt = performance.now()
    },
    async close() {
      for (const socket of sockets) socket.destroy()
      server.close()
      await once(server, 'close')
    },
  }
}

function runProcess(bin, args) {
  return new Promise((resolve, reject) => {
    const child = spawn(bin, args, { stdio: ['ignore', 'pipe', 'pipe'] })
    let stdout = ''
    let stderr = ''
    child.stdout.on('data', (chunk) => { stdout += chunk })
    child.stderr.on('data', (chunk) => { stderr += chunk })
    const timeout = setTimeout(() => child.kill('SIGKILL'), PROCESS_TIMEOUT_MS)
    child.once('error', reject)
    child.once('exit', (code, signal) => {
      clearTimeout(timeout)
      if (code === 0) resolve({ stdout, stderr })
      else reject(new Error(`${bin} exited with code=${code} signal=${signal}\n${stdout}\n${stderr}`))
    })
  })
}

async function benchmark({ label, bin }, expectedHash) {
  const workDir = await mkdtemp(path.join(tmpdir(), `aria2-tail-${label}-`))
  const outputPath = path.join(workDir, 'payload.bin')
  const logPath = path.join(workDir, 'aria2.log')
  const rangeServer = await startRangeServer()
  const warmup = await fetch(rangeServer.url, { headers: { Range: 'bytes=0-65535' } })
  await warmup.arrayBuffer()
  rangeServer.resetMeasurements()
  const args = [
    '--no-conf=true',
    '--allow-overwrite=true',
    '--auto-file-renaming=false',
    '--file-allocation=none',
    '--split=4',
    '--max-connection-per-server=4',
    '--min-split-size=1M',
    '--piece-length=1M',
    '--connect-timeout=5',
    '--timeout=20',
    '--max-tries=3',
    '--retry-wait=0',
    '--summary-interval=0',
    '--console-log-level=error',
    '--enable-color=false',
    `--dir=${workDir}`,
    '--out=payload.bin',
    rangeServer.url,
  ]
  if (STALL_ENABLED) {
    args.splice(-1, 0, `--log=${logPath}`, '--log-level=debug')
  }

  const startedAt = performance.now()
  try {
    await runProcess(bin, args)
    const elapsedMs = performance.now() - startedAt
    const payload = await readFile(outputPath)
    const hash = createHash('sha256').update(payload).digest('hex')
    if (payload.length !== TOTAL_LENGTH || hash !== expectedHash) {
      throw new Error(`${label} produced invalid output: length=${payload.length}, sha256=${hash}`)
    }
    const log = STALL_ENABLED ? await readFile(logPath, 'utf8') : ''
    return {
      label,
      elapsedMs: Math.round(elapsedMs),
      requestCount: rangeServer.requests.length,
      stalledRequest: rangeServer.requests.find((entry) => entry.action === 'stall'),
      requests: rangeServer.requests,
      reclaimLogCount: (log.match(/Reclaiming stalled HTTP tail segment/g) ?? []).length,
      workDir: KEEP_TEMP ? workDir : undefined,
    }
  } finally {
    await rangeServer.close()
    if (!KEEP_TEMP) await rm(workDir, { recursive: true, force: true })
  }
}

const binaries = parseBinaries(process.argv.slice(2))
const expectedHash = createHash('sha256')
  .update(Buffer.alloc(TOTAL_LENGTH, BYTE_VALUE))
  .digest('hex')
const results = []
for (const binary of binaries) results.push(await benchmark(binary, expectedHash))

console.table(results.map(({ label, elapsedMs, requestCount, reclaimLogCount }) => ({
  label,
  elapsedMs,
  requestCount,
  reclaimLogCount,
})))
console.log(JSON.stringify({
  scenario: STALL_ENABLED ? 'stalled-tail' : 'healthy-throughput',
  totalLength: TOTAL_LENGTH,
  mibpsPerConnection: MIBPS_PER_CONNECTION || undefined,
  results,
}, null, 2))
