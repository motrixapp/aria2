// Read an existing .torrent, inject a top-level `url-list` (BEP-19 web seed)
// pointing at our local node:http server, and return the patched bytes.
//
// Adding `url-list` at the TOP level (outside `info`) does NOT change the
// info-hash, so the patched torrent is interchangeable with the original
// from aria2's perspective except for the additional HTTP fallback.

import { readFile } from 'node:fs/promises'
import { decode, encode } from './bencode.mjs'

export async function patchTorrentWithWebSeed(torrentPath, webSeedUrl) {
  const raw = await readFile(torrentPath)
  const dict = decode(raw)
  // url-list as a single string (aria2 supports both string and list forms,
  // but we keep it as a list to match common torrent client output).
  dict['url-list'] = [webSeedUrl]
  return encode(dict)
}

export function summarizeTorrent(buf) {
  const d = decode(buf)
  const info = d.info
  const name = info.name?.toString('utf8') ?? '<unknown>'
  let totalSize = 0
  let fileCount = 0
  if (info.files) {
    for (const f of info.files) {
      totalSize += f.length
      fileCount++
    }
  } else if (typeof info.length === 'number') {
    totalSize = info.length
    fileCount = 1
  }
  return { name, fileCount, totalSize, pieceLength: info['piece length'] }
}
