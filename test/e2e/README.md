# aria2_motrix sqlite3-persistence e2e tests

End-to-end suite that verifies the SQLite3-Persistence path of `aria2_motrix`
through the BT pause / resume / restart cycle that Motrix Turbo users hit.

## What it covers

For each of two torrent layouts × two pause-timing variants:

| Torrent           | Files | Pause variant       |
| ----------------- | ----- | ------------------- |
| `sample.torrent`  | 1     | partial-piece pause |
| `sample.torrent`  | 1     | full-piece pause    |
| `multi.torrent`   | 2     | partial-piece pause |
| `multi.torrent`   | 2     | full-piece pause    |

Each test runs the lifecycle:

1. start a tiny `node:http` web-seed server (BEP-19) for the fixture data
2. patch the .torrent in memory to inject a top-level `url-list` pointing at it
3. spawn `aria2c` (built from this tree) with **the same parameter envelope as
   `motrix-turbo/src/core/engine/aria2/Aria2ConfigBuilder.ts`** — including
   both `--save-session` *and* `--enable-sqlite3-persistence=true`
4. `aria2.addTorrent` and wait until the task is `active` with the required
   number of completed pieces
5. `aria2.forcePause` → `aria2.unpause` → `aria2.forcePause` (state-machine
   sanity)
6. `aria2.saveSession` + sleep > `--save-session-interval` to flush sqlite
7. `SIGTERM` aria2; on exit re-spawn aria2 with the **same** sqlite db /
   session file paths
8. assert the task reappears with `status=paused`, the bittorrent block is
   present, and `numPieces` matches the torrent metadata
9. `aria2.changeOption` to lift the throttle, `aria2.unpause`, and poll for
   up to 30 s — assert `status` never becomes `error` and `errorCode` stays
   0 (or absent), eventually reaching `complete`

## Running

```bash
# 1. build aria2 once (deps are cached after the first run)
./scripts/rebuild-static-osx.sh --no-deps

# 2. from the repo root
node --test --test-timeout=180000 test/e2e/persistence-restart.e2e.test.mjs
```

The suite picks up the binary at `build-release/aria2.<arch>/aria2c`.
Override with `ARIA2_E2E_BIN=/path/to/aria2c`. Set `ARIA2_E2E_KEEP_TMP=1` to
preserve the per-test work dirs (sqlite db / logs / saved torrents) after
failures.

## Why both `--save-session` and `--enable-sqlite3-persistence`

That is the production parameter envelope used by Motrix Turbo today. The
e2e is faithful to that envelope so it reproduces the same dual-write
behaviour real users hit; flipping either off would test a configuration
nobody actually runs.

## Why web-seed instead of real peers

aria2 still drives the full BT task code path (BT context, piece bitfield,
`Sqlite3BtProgressInfoFile` updates) when fed via web-seed — only the data
channel is HTTP. This gives us deterministic, hermetic downloads with no
external trackers / DHT churn, while still exercising every sqlite write
that a peer-fed download would.

## Adding a debug log to the aria2 source

When chasing a failing case, follow this rule: every temporary
`A2_LOG_*` / `printf` you sprinkle into `src/` MUST be added to
`DEBUG_LOG_PATCHES.md` immediately, and removed (with a rebuild verification
note) before the fix is considered complete. The intent is to leave a
permanent audit trail so we never ship a release with leftover diagnostics.

## Files

```
test/e2e/
├── README.md                                this file
├── DEBUG_LOG_PATCHES.md                     audit trail of any source-level debug logs
├── persistence-restart.e2e.test.mjs         pause/resume/restart matrix (6 cases)
├── motrix-turbo-gid-match.e2e.test.mjs      gid stability across restart (1)
├── reproduce-reAdd-collision.e2e.test.mjs   errorCode=13 + check-integrity fix (1)
├── no-fk-violation-on-fresh-dir.e2e.test.mjs FK violation on fresh dir (1)
├── no-pause-exit-survives-restart.e2e.test.mjs SIGTERM no-pause survival (1)
├── helpers/
│   ├── aria2-process.mjs            CLI argv shape + spawn / stop helpers
│   ├── bencode.mjs                  minimal bencode encode/decode (Buffers)
│   ├── rpc-client.mjs               JSON-RPC client over HTTP
│   ├── torrent-patch.mjs            inject `url-list` (BEP-19) into a .torrent
│   ├── wait.mjs                     polling helpers
│   └── webseed-server.mjs           tiny static-file server with Range support
└── fixtures/
    ├── sample.torrent               multi-file format with 1 file
    ├── single.torrent               true single-file format (info has `length`)
    ├── multi.torrent                two-file multi-file torrent
    ├── ubuntu.torrent               real-world 5 GiB single-file (gated tests)
    ├── ubuntu-like.iso              data backing single.torrent
    ├── sample-data/test.bin         1 MiB
    └── multi-data/{test-a,test-b}.bin 2 × 256 KiB
```

## Background

The 2026-04-29 incident report
(`motrix-turbo/docs/specs/bt-persistence-incident-20260429.md` and its
`.zh-CN.md` counterpart) records the three independent bugs this suite
was built to lock down: `errorCode=13` after re-add, the FOREIGN-KEY
violation on a fresh `--dir`, and the `GID not found` symptom after a
no-pause SIGTERM exit. Read that document first if you are touching
this suite for the first time.
