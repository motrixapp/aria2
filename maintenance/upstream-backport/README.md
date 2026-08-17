# Upstream issue backport audit

This directory records which upstream aria2 bug fixes were backported into
`aria2_motrix`, from where, and — most importantly — whether each fix is
actually **effective on this fork's production call chain**, not merely
"a regression test passes".

## Where the fixes come from

The code was ported from the `aria2-next` fork's commit `882585d7`
(`docs(maintenance): add open issue review document and summary of
findings`), which bundled verified code fixes for 44 upstream open issues
plus one partially-verified fix (#1407). That commit predates aria2-next's
CMake / spdlog / C++17 migration, so it still uses the original `A2_LOG_*`
macros and `Makefile.am` and applies cleanly onto aria2_motrix's
autotools + CppUnit tree.

`aria2-next`'s own audit matrix used `state=fixed-verified` to mean only
"a new regression test for this issue passes". That standard is not
sufficient: a regression test can exercise an isolated primitive while the
production call chain never reaches the fixed code. This audit re-checked
every fix against the real chain — and found six ineffective, two
dangerous, and three partial ports hiding behind a green `fixed-verified`.

## `matrix.csv` schema

`aria2-next`'s eight columns, plus three added here:

| column | meaning |
|--------|---------|
| `number` | upstream aria2 issue number |
| `priority` | P0/P1/P2/P3 impact on Motrix |
| `module` | subsystem grouping (from aria2-next) |
| `title` | upstream issue title |
| `upstream_state` | **aria2-next's** original verdict (`fixed-verified`, `fixed-partially-verified`) |
| `local_state` | **this fork's** re-audit conclusion (see below) |
| `root_cause_group` | shared-root-cause bucket (from aria2-next) |
| `required_action` | disposition in this backport |
| `test_ref` | the test that spans the real call chain, or `primitive-only:`/`regression:`/`none` when it does not |
| `evidence` | this fork's evidence for the `local_state` verdict |

### `local_state` values (45 issues)

| value | count | meaning |
|-------|------|---------|
| `verified-effective` | 25 | fix reaches and works on the production chain; regression + build/e2e green |
| `defective-rewritten` | 5 | ported fix was defective; **rewritten here** and verified with a chain-spanning test |
| `defective-fixed` | 4 | ported fix carried a regression/crash the audit caught; **repaired here** with a regression test |
| `ported-defective` | 2 | defective and dangerous; **left as-is this pass**, flagged for a dedicated follow-up (see below) |
| `ported-ineffective` | 5 | reaches production but is a no-op (already-fixed, or does not address the reported input); harmless |
| `needs-attention` | 3 | partially effective / scope gap / niche regression; documented, not changed |
| `ported-tests-pass` | 1 | ported unchanged, regression + full suite green, not independently deep-audited |

## The five rewritten defects (`defective-rewritten`)

| issue | defect | chain-spanning test |
|-------|--------|---------------------|
| #1752 | `createJsonRpcErrorResponse` hard-coded `AUTHORIZED`; any malformed WS message authorized the session, which then received all notifications | `RpcHelperTest.cc` (7) + `e2e/websocket-auth` |
| #2280 | `createCheckIntegrityEntry` dropped `FileOpenMode`, so the `RESTART_FROM_SCRATCH` branch was dead and a conditional 200 resumed against stale bytes | `RequestGroupTest.cc::testCreateCheckIntegrityEntryRestartFromScratch` + `e2e/conditional-get-restart` |
| #1407 | modern-Schannel AND-ed `~SP_PROT` into a zero disabled-protocols mask (stays 0 = no floor), so `--min-tls-version` was a no-op | `WinTLSProtocolsTest.cc` (3) + mingw parity |
| #1839 | 503 reset the try count on wake, so a permanently-503 server retried forever | `e2e/503-max-tries` |
| #1280 | `sqlite3_open_v2` opens lazily, so the immutable-URI fallback never triggered and Firefox WAL cookie import failed at read time | `Sqlite3CookieParserTest.cc::testMozParse_readOnlyWalSnapshot` |

Each was shown to FAIL against the pre-fix code and PASS after.

## Additional defects the re-audit found and fixed (`defective-fixed`)

- **#886/#1115/#2061** — the ported chunked-206-without-`Content-Range`
  rejection lacked the `getSegment()` guard, hard-failing a segment-less
  initial request that legitimately gets a 206 (a regression vs the
  unknown-length path). Fixed; regression test added.
- **#1471** — the ported parameterized-URI range widening added an
  RPC-reachable divide-by-zero (empty `{}` choice before a loop) and a
  `%d`/`int64_t` format mismatch. Fixed; regression test added.
- **build integration** — `WebSocketSessionManTest.cc` was added to the
  unconditional test sources, breaking `make check` under
  `--disable-websocket`; now guarded by `ENABLE_WEBSOCKET`.

## ⚠️ Dangerous ports left for follow-up (`ported-defective`)

These two are entangled with effective fixes and their real repair is
larger than a backport-audit pass; they are **documented, not changed**,
and should be handled next:

- **#1556 (WrDiskCache)** — the tested branch is production-unreachable
  dead code; the one live branch masks a **use-after-free** (a `Piece`
  destructor frees a `WrDiskCacheEntry` still held by the process-wide
  cache after a `CANNOT_RESUME` retry), downgrading a deterministic abort
  to a warning while the UAF continues. Recommend reverting to the upstream
  abort and fixing the UAF separately.
- **#1727 (Metalink/BT whole-file checksum)** — the consumer
  (`BtCheckIntegrityEntry::onDownloadFinished`) is unreachable at BT
  completion, so the checksum still never runs; and in the one path it does
  fire it **clears a piece-hash-verified bitfield, which this fork then
  persists into the SQLite3 progress row**. Recommend reverting the
  consumer scheduling (keep #2033, which is effective).

## Notable `needs-attention` items

- **#2285** — works for third-party RPC clients but is dead for Motrix
  (Turbo never sends `max-concurrent-downloads` over RPC). Fork hazard: the
  forced pause/restart can persist `state=paused` into the SQLite3 row and
  crash-recovery brings the task back paused. Coordinate with
  `motrix-turbo` before relying on it.
- **#2119** — correctly rejects the invalid IPv6 literals, but also now
  rejects RFC 6874 zone identifiers (`[fe80::1%eth0]`), which parsed before.
- **#826** — only the AAAA-only half is fixed; the parallel v4/v6 race half
  is untouched (pre-existing).

## Verification performed

- `make check` (full CppUnit suite, 1080 tests) — pass, 0 fail / 0 error.
- `test/e2e/` Node.js suite — existing 11 cases plus 3 new defect cases,
  all pass; confirms the #2280 `RequestGroup` changes did not regress the
  SQLite3 persistence hook.
- New e2e cases for #1752, #2280, #1839 each shown to FAIL against the
  pre-fix code and PASS after; #1407 pure mask unit-tested (RED vs the
  original bit logic) and `WinTLSContext.cc` shown to produce no new errors
  vs the ported original under the ubuntu mingw-w64 cross toolchain (the
  residual errors are a pre-existing `SCH_CREDENTIALS`-type gap in that
  toolchain). Full Windows compile/link is validated by the Windows CI lane.
- Independent re-audit of all 45 issues by parallel review agents; their
  per-issue findings are recorded in `matrix.csv`.
