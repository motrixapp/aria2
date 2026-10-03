# Legacy checkpoint import protocol v1

This protocol imports bounded, unmodified version 1 `.aria2` snapshots into the
SQLite checkpoint store. It creates no engine task, starts no download, does not
unpause or seed, and never opens payloads for writing. All RPCs use the existing
RPC token authorization. The current implementation advertises the runtime
`LegacyCheckpointImportV1` feature only with active SQLite persistence, a
configured nonempty RPC secret, SHA-256, and POSIX `openat`/`O_NOFOLLOW` support.
Windows and non-SQLite builds fail closed. Native version 0 files are refused
because their host-endian encoding is ambiguous across platforms.

The application must verify the capability through `aria2.getVersion` before
using these methods. Its own offline source snapshot, backup, consent, payload
ownership and HTTP resource-validator policies remain necessary. A valid
checkpoint reports recorded ranges; it does not establish that historical HTTP
bytes match the current remote resource. BT activation must verify pieces.

## Methods

Every method takes one object after the existing `token:<rpc secret>` parameter.
All numeric fields are canonical decimal strings within signed 64-bit range.
The method names and keys below are final for this version.

`aria2.inspectLegacyCheckpointV1({controlFile})` accepts canonical base64 and
returns:

```json
{
  "version": "1",
  "format": "aria2-v1",
  "kind": "http",
  "totalLength": "4194304",
  "pieceLength": "1048576",
  "infoHash": "",
  "uploadLength": "0",
  "completedLength": "1638400",
  "controlDigest": "40a6f7572e913ad63f0b2e867f26367e30c0cad754389963cb2f2645088dc7c7",
  "ranges": [{"offset": "0", "length": "1638400"}]
}
```

Inspection parses bytes only. It does not inspect paths, write SQLite rows, open
payload handles, or dispatch network work. `kind` is `http` for the native
non-BT format; that format alone cannot distinguish HTTP from FTP. `infoHash`
is empty for non-BT snapshots and 40 lowercase hexadecimal characters for BT.
Ranges include native completed pieces and completed 16 KiB blocks within
in-flight pieces; preallocated file length contributes no progress.

`aria2.importLegacyCheckpointV1` accepts:

```json
{
  "token": "application_unique_import_token",
  "gid": "0123456789abcdef",
  "controlFile": "<canonical base64 bytes>",
  "controlDigest": "<SHA-256 of decoded bytes>",
  "targetPath": "/confirmed/downloads/partial.bin",
  "expected": {
    "kind": "http", "totalLength": "4194304", "pieceLength": "1048576", "infoHash": ""
  },
  "files": [{
    "path": "/confirmed/downloads/partial.bin",
    "offset": "0", "length": "4194304",
    "identity": {
      "device": "1", "inode": "42", "size": "1638400",
      "mtimeNs": "1790956800000000000", "ctimeNs": "1790956800000000000"
    }
  }]
}
```

`targetPath` is the native `DownloadContext::getBasePath()` key. HTTP requires
one file equal to that path. BT accepts files equal to the base path or beneath
its directory; the file map covers the complete torrent logical length in
order, including unselected files. Every file currently must exist, belong to
the engine's effective user, have one hard link, and have no group/other write
permission. File paths must be canonical absolute POSIX paths. Symlinks in any
path component, traversal and aliased inode maps are refused. Canonicalize
platform paths before requesting import (e.g. macOS `/tmp` to `/private/tmp`).
Payloads may be shorter than their logical file lengths, but every checkpoint
range must fit within the existing file extent.

Expected metadata must exactly match the decoded snapshot. The engine checks
active/reserved/stopped task ownership, SQLite GIDs, historical file paths,
existing checkpoints and earlier import claims. It refuses every unrelated
existing checkpoint; it never overwrites one. Import commits native progress,
the receipt and file identity claims in a single SQLite transaction. Holding
read-only handles and reopening paths before commit detects replacement and
modification during validation. No source control-file path is passed to the
engine: the caller owns the protected snapshot bytes.

A successful first call returns:

```json
{
  "version": "1", "status": "created", "token": "application_unique_import_token",
  "gid": "0123456789abcdef", "targetPath": "/confirmed/downloads/partial.bin",
  "controlDigest": "<SHA-256 of decoded bytes>"
}
```

The request fingerprint covers source digest, GID, target, metadata and complete
file identities/map. An identical token/input replay returns the receipt and
never inserts a second checkpoint. Changed input with the same token fails.
Receipts are retained as tombstones even if checkpoints are removed or changed.
A replay whose checkpoint was consumed, changed, or removed returns
`status: "consumed"` and never recreates progress, even if payloads changed.

`aria2.reconcileLegacyCheckpointV1({token,targetPath})` reconciles the
receipt and current checkpoint/payload identities. It atomically persists a
terminal invalidation tombstone when previously created state is no longer
intact; it never modifies checkpoint or payload bytes. It returns the same created/consumed
receipt above, or `{version:"1",status:"absent",token,targetPath}`. A token bound
to another target fails. Consumed state is monotonic even if a removed checkpoint or payload later
returns to its original state. This resolves a lost import response or restart without
retrying a mutation blindly. `created` includes proof of the original GID,
metadata, bitfields and native trailer still matching the control snapshot.

`aria2.addLegacyTorrentV1` activates a created BT receipt with a durable,
read-only reference to the application's authorized torrent backup. It requires
both runtime capabilities, `LegacyCheckpointImportV1` and
`LegacyTorrentMetadataV1`. The latter also requires a usable SQLite task store
and a BitTorrent-enabled build. This method accepts:

```json
{
  "token": "application_unique_import_token",
  "targetPath": "/confirmed/downloads/fixture-bundle",
  "metadataFile": "/application/backups/authorized-torrents/example.torrent",
  "metadataDigest": "<SHA-256 of the complete torrent bytes>",
  "metadata": "<canonical base64 torrent bytes>",
  "options": {
    "gid": "0123456789abcdef",
    "dir": "/confirmed/downloads",
    "pause": "true",
    "check-integrity": "true",
    "bt-seed-unverified": "false",
    "select-file": "1"
  }
}
```

It returns the reserved GID string. The token, target, GID, decoded torrent,
piece geometry and complete native file map must match the intact created
receipt. `pause=true`, `check-integrity=true`, and `bt-seed-unverified=false`
are mandatory. The engine forces `rpc-save-upload-metadata=false`; it neither
saves uploaded metadata in the payload directory nor changes the payload base
path. The backup uses the same canonical-path, ownership, regular-file,
single-link and no-follow checks as payloads. Its digest must match both the
read-only file snapshot and the RPC bytes. A metadata inode cannot alias any
imported payload. Torrent bytes are bounded to 64 MiB, subject to the configured
RPC request-size limit.

Before returning, one SQLite transaction commits the immutable metadata path,
digest and file identity together with the paused task row. This survives
SIGKILL without a session-save interval or explicit `saveSession`. Repeating
the activation after a lost response fails without creating another task or
replacing the grant; reconcile the receipt and query its GID instead. Ordinary
`addTorrent` with `rpc-save-upload-metadata=false` has no durable metadata source
and is unsuitable for this activation protocol. Never silently fall back to it
when the new runtime capability is absent.

## Native restoration and recovery

The checkpoint survives session saves and engine restart without a task row
while its payload base path exists. The caller can then add a task with the
reserved GID and `pause=true`. Before native restoration, the engine rechecks
the GID, kind/infoHash, total and piece lengths, complete native file map and
persisted file identities. A failed check preserves the original checkpoint and payload; routine
or terminal task saves cannot overwrite its original checkpoint. Reconciliation
records a terminal invalidation if payload identities changed. Successful
restoration atomically consumes the receipt before normal file opening and
transfer. Reconciliation then reports `consumed` independently of later progress
or completion cleanup. Imported checkpoints are not resumable under another
GID or with changed metadata. No special startup flag beyond the existing
SQLite/RPC configuration is required.

Every durable legacy BT task is restored **paused**, including a task that was
active before shutdown. On restart the engine verifies the backup's persisted
identity and whole-file SHA-256, then parses verified bytes from RAM. Metadata
replacement, mutation, removal or a symlink invalidates the grant permanently;
the task is not queued and never falls back to the ordinary torrent-path
loader. The invalidated grant and its task row remain as durable evidence across
routine task snapshots. Restoring the old file later cannot revive activation.
The application must query the restored GID, reconcile the receipt and request
explicit unpause after successful recovery; native integrity checking precedes
transfer. `consumed` can mean either successful native restoration or terminal
invalidation, so it alone does not prove a resumable task exists.

Receipt claims deliberately remain conservative after consumption. There is
no cleanup/undo RPC in this version. Cross-platform moves, absent unselected BT
files, payload relocation, directory output remapping outside the base, and
Windows ACL/handle support require a later explicitly versioned extension.
This protocol does not claim a file-lock guarantee against arbitrary concurrent
external writers; the application must establish that the old engine is closed.

## Bounds and validation

- Decoded control file: 64 MiB; RPC callers must also honor the configured RPC
  request-size budget (the default is smaller).
- Pieces: 16,777,216; in-flight pieces: 100,000; merged ranges: 100,000.
- Files: 10,000 (OS descriptor limits can cause a safe import failure sooner).
- Token: 16–128 ASCII letters, digits, `_` or `-`; GID: 16 lowercase hex digits,
  nonzero; paths: at most 4096 bytes, no control characters or dot segments.
- Piece length: 1 through `INT32_MAX`; total/upload lengths: 0 through
  `INT64_MAX`. Unknown extension flags, malformed/truncated/trailing bytes,
  invalid lengths/padding bits, duplicate or completed in-flight indices,
  wrong final-piece sizes and malformed infoHash are refused.

Native `.aria2` saving shares the extracted v1 serializer with the importer.
The SQLite progress layout and in-flight blob remain the native loader's layout.
Database schema 4 adds import receipts/file claims while preserving schema 3
checkpoints. Schema 5 adds durable BT metadata grants while preserving existing
tasks, checkpoints and receipt claims. Older binaries refuse the newer schema
rather than silently using it without the restoration checks.

## Local verification

```sh
autoreconf -fi
mkdir build-legacy-import
cd build-legacy-import
../configure --with-openssl --without-gnutls --without-appletls --disable-nls
make -C src -j8
make -C test -j8 aria2c
(cd test && ./aria2c)
cd ..
ARIA2_E2E_BIN="$PWD/build-legacy-import/src/aria2c" \
  node --test test/e2e/legacy-checkpoint-import.e2e.test.mjs
```

The engine fixtures under `test/e2e/fixtures/legacy-v1/` are copied unchanged
from the actual `aria2 1.36.0` bundled in Motrix `v1.8.19` commit
`a0a1fe90f7e9f6d305ed2b512f62c8d36c2fb95a`; provenance records the binary and
fixture hashes. The HTTP test decompresses the exact old partial payload,
restores its native partial-block checkpoint and requires both the suffix Range
and trusted final SHA-256. The BT test restores the actual multi-file checkpoint,
keeps selection paused until explicit activation, verifies existing pieces and
checks payload preservation. Synthetic codec/security tests cover malformed
input, transactions, token conflicts, restart, replacement, aliases and ownership.
Durable metadata tests preserve the ordinary-add restart failure as a regression
baseline, drop a real activation RPC response, restart after graceful shutdown
and SIGKILL, and require the same paused GID without a duplicate. They also cover
atomic task/grant rollback, changed or replaced backups, missing files, symlinks,
schema upgrades and an older schema-4 binary refusing schema 5.
