// Minimal bencode encoder/decoder. Operates on Buffers because torrent
// payloads are binary (info dict pieces, raw bytestrings) and JSON-style
// strings would corrupt non-UTF-8 bytes.
//
// Decoder returns:
//   integer  -> Number
//   string   -> Buffer (caller decides whether to .toString('utf8'))
//   list     -> Array of decoded values
//   dict     -> Object of decoded values, keyed by Buffer.toString('utf8')
//
// Encoder accepts the same shapes, plus plain strings (encoded as utf8).

export function decode(buf) {
  if (!Buffer.isBuffer(buf)) {
    throw new TypeError('decode requires a Buffer')
  }
  const state = { buf, pos: 0 }
  const value = decodeValue(state)
  if (state.pos !== buf.length) {
    throw new Error(`trailing bytes after bencode value at ${state.pos}`)
  }
  return value
}

function decodeValue(state) {
  const c = state.buf[state.pos]
  if (c === 0x69) return decodeInt(state) // 'i'
  if (c === 0x6c) return decodeList(state) // 'l'
  if (c === 0x64) return decodeDict(state) // 'd'
  if (c >= 0x30 && c <= 0x39) return decodeString(state) // '0'..'9'
  throw new Error(`unexpected bencode marker 0x${c.toString(16)} at ${state.pos}`)
}

function decodeInt(state) {
  state.pos++ // consume 'i'
  const end = state.buf.indexOf(0x65, state.pos) // 'e'
  if (end < 0) throw new Error('unterminated bencode integer')
  const num = Number(state.buf.subarray(state.pos, end).toString('utf8'))
  if (!Number.isFinite(num)) throw new Error('invalid bencode integer')
  state.pos = end + 1
  return num
}

function decodeString(state) {
  const colon = state.buf.indexOf(0x3a, state.pos) // ':'
  if (colon < 0) throw new Error('unterminated bencode string length')
  const len = Number(state.buf.subarray(state.pos, colon).toString('utf8'))
  if (!Number.isInteger(len) || len < 0) throw new Error('invalid string length')
  state.pos = colon + 1
  const out = state.buf.subarray(state.pos, state.pos + len)
  state.pos += len
  return Buffer.from(out) // detach from source buffer view
}

function decodeList(state) {
  state.pos++ // consume 'l'
  const out = []
  while (state.buf[state.pos] !== 0x65) {
    if (state.pos >= state.buf.length) throw new Error('unterminated list')
    out.push(decodeValue(state))
  }
  state.pos++
  return out
}

function decodeDict(state) {
  state.pos++ // consume 'd'
  const out = {}
  while (state.buf[state.pos] !== 0x65) {
    if (state.pos >= state.buf.length) throw new Error('unterminated dict')
    const key = decodeString(state).toString('utf8')
    out[key] = decodeValue(state)
  }
  state.pos++
  return out
}

export function encode(value) {
  const parts = []
  encodeValue(value, parts)
  return Buffer.concat(parts)
}

function encodeValue(v, parts) {
  if (Buffer.isBuffer(v)) {
    parts.push(Buffer.from(`${v.length}:`, 'utf8'), v)
    return
  }
  if (typeof v === 'string') {
    const b = Buffer.from(v, 'utf8')
    parts.push(Buffer.from(`${b.length}:`, 'utf8'), b)
    return
  }
  if (typeof v === 'number') {
    if (!Number.isInteger(v)) throw new Error('bencode supports integers only')
    parts.push(Buffer.from(`i${v}e`, 'utf8'))
    return
  }
  if (Array.isArray(v)) {
    parts.push(Buffer.from('l', 'utf8'))
    for (const item of v) encodeValue(item, parts)
    parts.push(Buffer.from('e', 'utf8'))
    return
  }
  if (v && typeof v === 'object') {
    parts.push(Buffer.from('d', 'utf8'))
    // Bencode dicts must be sorted by raw byte order of the key.
    const keys = Object.keys(v).sort()
    for (const k of keys) {
      const kb = Buffer.from(k, 'utf8')
      parts.push(Buffer.from(`${kb.length}:`, 'utf8'), kb)
      encodeValue(v[k], parts)
    }
    parts.push(Buffer.from('e', 'utf8'))
    return
  }
  throw new TypeError(`unsupported bencode value: ${typeof v}`)
}
