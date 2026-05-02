// Polling helpers shared across the e2e suite.

export async function sleep(ms) {
  await new Promise((r) => setTimeout(r, ms))
}

export async function waitFor(fn, timeoutMs, intervalMs = 100, label = '') {
  const start = Date.now()
  let last
  while (Date.now() - start < timeoutMs) {
    try {
      const v = await fn()
      if (v !== null && v !== undefined && v !== false) return v
      last = v
    } catch (err) {
      last = err
    }
    await sleep(intervalMs)
  }
  const msg = label ? `waitFor[${label}]` : 'waitFor'
  throw new Error(
    `${msg} timed out after ${timeoutMs}ms (last value: ${formatLast(last)})`
  )
}

function formatLast(v) {
  if (v instanceof Error) return v.message
  if (typeof v === 'object') {
    try {
      return JSON.stringify(v)
    } catch {
      return String(v)
    }
  }
  return String(v)
}
