// Minimal aria2 JSON-RPC over HTTP client. We don't use WebSocket so
// reconnect-after-restart is just "fetch the new port"; no socket state to
// reset.

let nextId = 1

export class Aria2Rpc {
  constructor({ port, secret }) {
    this.url = `http://127.0.0.1:${port}/jsonrpc`
    this.secret = secret
  }

  async call(method, params = []) {
    const body = {
      jsonrpc: '2.0',
      id: `e2e-${nextId++}`,
      method,
      params: [`token:${this.secret}`, ...params],
    }
    const res = await fetch(this.url, {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify(body),
    })
    const json = await res.json()
    if (json.error) {
      const err = new Error(
        `aria2 RPC ${method} failed: ${json.error.code} ${json.error.message}`
      )
      err.aria2Code = json.error.code
      throw err
    }
    return json.result
  }

  // ---- BT task lifecycle ----
  addTorrent(metadataBase64, opts = {}) {
    return this.call('aria2.addTorrent', [metadataBase64, [], opts])
  }
  pause(gid) {
    return this.call('aria2.pause', [gid])
  }
  forcePause(gid) {
    return this.call('aria2.forcePause', [gid])
  }
  unpause(gid) {
    return this.call('aria2.unpause', [gid])
  }
  remove(gid) {
    return this.call('aria2.remove', [gid])
  }
  forceRemove(gid) {
    return this.call('aria2.forceRemove', [gid])
  }
  removeDownloadResult(gid) {
    return this.call('aria2.removeDownloadResult', [gid])
  }
  tellStatus(gid, keys) {
    return this.call('aria2.tellStatus', keys ? [gid, keys] : [gid])
  }
  tellActive(keys) {
    return this.call('aria2.tellActive', keys ? [keys] : [])
  }
  tellWaiting(offset = 0, num = 100, keys) {
    return this.call(
      'aria2.tellWaiting',
      keys ? [offset, num, keys] : [offset, num]
    )
  }
  tellStopped(offset = 0, num = 100, keys) {
    return this.call(
      'aria2.tellStopped',
      keys ? [offset, num, keys] : [offset, num]
    )
  }
  saveSession() {
    return this.call('aria2.saveSession', [])
  }
  shutdown() {
    return this.call('aria2.shutdown', [])
  }
  forceShutdown() {
    return this.call('aria2.forceShutdown', [])
  }
}

export async function waitForRpc(port, timeoutMs = 8000) {
  const url = `http://127.0.0.1:${port}/jsonrpc`
  const start = Date.now()
  let lastErr
  while (Date.now() - start < timeoutMs) {
    try {
      const res = await fetch(url, {
        method: 'POST',
        headers: { 'content-type': 'application/json' },
        body: JSON.stringify({
          jsonrpc: '2.0',
          id: 'probe',
          method: 'system.listMethods',
          params: [],
        }),
      })
      if (res.ok) return
    } catch (err) {
      lastErr = err
    }
    await new Promise((r) => setTimeout(r, 100))
  }
  throw new Error(
    `aria2 RPC :${port} not ready after ${timeoutMs}ms${
      lastErr ? `: ${lastErr.message}` : ''
    }`
  )
}
