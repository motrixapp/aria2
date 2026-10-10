import assert from 'node:assert/strict'
import { mkdtemp, rm } from 'node:fs/promises'
import { tmpdir } from 'node:os'
import path from 'node:path'
import { DatabaseSync } from 'node:sqlite'
import { test } from 'node:test'
import { allocPorts, spawnAria2, stopInstance } from './helpers/aria2-process.mjs'
import { Aria2Rpc, waitForRpc } from './helpers/rpc-client.mjs'

test('Windows refuses unsupported legacy recovery without creating durable rows', {
  skip: process.platform !== 'win32',
}, async t => {
  const root = await mkdtemp(path.join(tmpdir(), 'aria2-legacy-unsupported-'))
  const { rpcPort } = allocPorts()
  const secret = 'unsupported-legacy-test'
  const database = path.join(root, 'session.db')
  const proc = await spawnAria2({ args: [
    '--no-conf=true', '--enable-rpc=true', '--rpc-listen-all=false',
    `--rpc-listen-port=${rpcPort}`, `--rpc-secret=${secret}`,
    '--enable-dht=false', '--enable-dht6=false', '--bt-enable-lpd=false',
    '--enable-sqlite3-persistence=true', `--sqlite3-db-path=${database}`,
    '--console-log-level=error',
  ] })
  t.after(async () => {
    await stopInstance(proc)
    await rm(root, { recursive: true, force: true })
  })
  const rpc = new Aria2Rpc({ port: rpcPort, secret })
  await waitForRpc(rpcPort)
  const version = await rpc.call('aria2.getVersion')
  assert.ok(!version.enabledFeatures.includes('LegacyCheckpointImportV1'))
  assert.ok(!version.enabledFeatures.includes('LegacyTorrentMetadataV1'))
  for (const method of ['inspectLegacyCheckpointV1', 'importLegacyCheckpointV1',
    'reconcileLegacyCheckpointV1', 'addLegacyTorrentV1']) {
    await assert.rejects(rpc.call(`aria2.${method}`, [{}]), /unavailable|unsupported/)
  }
  assert.deepEqual(await rpc.tellActive(), [])
  assert.deepEqual(await rpc.tellWaiting(0, 10), [])
  const db = new DatabaseSync(database, { readOnly: true })
  try {
    for (const table of ['task', 'task_progress', 'legacy_checkpoint_import',
      'legacy_checkpoint_file', 'legacy_torrent_metadata']) {
      assert.equal(db.prepare(`SELECT count(*) AS n FROM ${table}`).get().n, 0)
    }
  } finally { db.close() }
})
