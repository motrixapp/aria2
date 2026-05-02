// Tiny static-file HTTP server used as a BitTorrent web seed (BEP-19).
// aria2 maps `url-list = http://host:port/` + torrent name `foo` + path
// `bar.bin` into `GET http://host:port/foo/bar.bin`.

import { createServer } from 'node:http'
import { stat, createReadStream } from 'node:fs'
import path from 'node:path'

function statAsync(p) {
  return new Promise((res, rej) =>
    stat(p, (err, s) => (err ? rej(err) : res(s)))
  )
}

export async function startWebSeedServer(rootDir, opts = {}) {
  const server = createServer(async (req, res) => {
    try {
      const urlPath = decodeURIComponent((req.url ?? '/').split('?')[0])
      const safe = path
        .normalize(urlPath)
        .replace(/^(\/|\\)+/, '')
        .replaceAll('..', '')
      const fsPath = path.join(rootDir, safe)
      const st = await statAsync(fsPath)
      if (!st.isFile()) { res.statusCode = 404; res.end(); return }
      const range = req.headers.range
      let start = 0, end = st.size - 1, status = 200
      if (range) {
        const m = /bytes=(\d+)-(\d*)/.exec(range)
        if (m) { start = Number(m[1]); end = m[2] ? Number(m[2]) : st.size - 1; status = 206 }
      }
      res.statusCode = status
      res.setHeader('content-type', 'application/octet-stream')
      res.setHeader('accept-ranges', 'bytes')
      res.setHeader('content-length', String(end - start + 1))
      if (status === 206) res.setHeader('content-range', `bytes ${start}-${end}/${st.size}`)
      // createReadStream from fs auto-manages the fd lifecycle on
      // close/error/end — no manual FileHandle bookkeeping required.
      const stream = createReadStream(fsPath, { start, end })
      stream.on('error', () => { try { res.destroy() } catch {} })
      res.on('close', () => stream.destroy())
      stream.pipe(res)
    } catch (err) {
      res.statusCode = err?.code === 'ENOENT' ? 404 : 500
      res.end()
    }
  })
  await new Promise((resolve, reject) => {
    server.once('error', reject)
    server.listen(opts.port ?? 0, '127.0.0.1', resolve)
  })
  const { port } = server.address()
  return {
    url: `http://127.0.0.1:${port}/`,
    port,
    close: () => new Promise((resolve) => {
      server.closeAllConnections?.()
      server.close(() => resolve())
    }),
  }
}
