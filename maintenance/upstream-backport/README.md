# Upstream issue backport audit

[简体中文](README.zh-CN.md)

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
The final adversarial pass reverted both dangerous ports and repaired the
remaining remotely triggerable resource-exhaustion and range-integrity gaps.

## `matrix.csv` schema

`aria2-next`'s seven columns, plus three added here:

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
| `unsafe-port-resolved` | 2 | defective and dangerous port was removed; its root cause was fixed (#1556) or the unsupported behavior was safely reverted (#1727) |
| `ported-ineffective` | 5 | reaches production but is a no-op (already-fixed, or does not address the reported input); harmless |
| `needs-attention` | 3 | partially effective / scope gap / niche regression; documented, not changed |
| `ported-tests-pass` | 1 | ported unchanged, regression + full suite green, not independently deep-audited |

## The five rewritten defects (`defective-rewritten`)

| issue | defect | chain-spanning test |
|-------|--------|---------------------|
| #1752 | `createJsonRpcErrorResponse` hard-coded `AUTHORIZED`; any malformed WS message authorized the session, which then received all notifications | `RpcHelperTest.cc` (7) + `e2e/websocket-auth` |
| #2280 | `createCheckIntegrityEntry` dropped `FileOpenMode`, so the `RESTART_FROM_SCRATCH` branch was dead and a conditional 200 resumed against stale bytes | `RequestGroupTest.cc::testCreateCheckIntegrityEntryRestartFromScratch` + `e2e/conditional-get-restart` |
| #1407 | modern-Schannel AND-ed `~SP_PROT` into a zero disabled-protocols mask (stays 0 = no floor), so `--min-tls-version` was a no-op; the final pass also made the minimum independent of machine policy by explicitly disabling PCT/SSL | `WinTLSProtocolsTest.cc` (3) + obsolete-protocol assertions + mingw parity |
| #1839 | 503 reset the try count on wake, so a permanently-503 server retried forever | `e2e/503-max-tries` |
| #1280 | `sqlite3_open_v2` opens lazily, so the immutable-URI fallback never triggered and Firefox WAL cookie import failed at read time | `Sqlite3CookieParserTest.cc::testMozParse_readOnlyWalSnapshot` |

Each was shown to FAIL against the pre-fix code and PASS after.

## Additional defects the re-audit found and fixed (`defective-fixed`)

- **#886/#1115/#2061** — the ported chunked-206-without-`Content-Range`
  rejection lacked the `getSegment()` guard, hard-failing a segment-less
  initial request that legitimately gets a 206 (a regression vs the
  unknown-length path). Fixed; the final pass also closed two corruption
  bypasses by validating transfer-encoded responses that do carry
  `Content-Range` and rejecting segmented transfer-encoded 200/206 responses
  without one. Regression tests cover all three cases.
- **#1471** — the ported parameterized-URI range widening added an
  RPC-reachable divide-by-zero (empty `{}` choice before a loop) and a
  `%d`/`int64_t` format mismatch. The final pass also capped aggregate
  expansion at 65,536 strings, so `[0-2147483647]` is rejected instead of
  attempting a multi-billion-element allocation. Fixed; regression tests
  added.
- **build integration** — `WebSocketSessionManTest.cc` was added to the
  unconditional test sources, breaking `make check` under
  `--disable-websocket`; now guarded by `ENABLE_WEBSOCKET`.

## Unsafe ports resolved (`unsafe-port-resolved`)

The final adversarial pass removed both dangerous implementations while
preserving unrelated effective fixes. This branch no longer masks a UAF or
corrupts persisted integrity state:

- **#1556 (WrDiskCache)** — removed the restore/reindex fallback. A missing
  cache entry now fails closed again, instead of re-inserting a potentially
  dangling pointer into the process-wide cache. The root cause was also fixed:
  restarting from scratch now detaches every in-flight piece's write-cache
  entry before destroying the piece. Regression tests cover both the cache
  lifecycle and the rejected update.
- **#1727 (Metalink/BT whole-file checksum)** — the consumer
  (`BtCheckIntegrityEntry::onDownloadFinished`) is unreachable at BT
  completion; in the one path where it could fire it cleared a
  piece-hash-verified bitfield that this fork persists into SQLite3. The
  checksum carryover and dead consumer scheduling were removed, while the
  effective #2033 behavior remains. A regression test verifies that the
  unsupported whole-file checksum is not carried into the BT context.

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

## Final adversarial verdict

**High: 0.** The remaining `needs-attention` entries are bounded compatibility
or product-integration gaps; none is a high-severity confidentiality,
integrity, memory-safety, authentication, or remotely-triggerable availability
finding in this branch.

## Verification performed

- `make check` (full CppUnit suite) — pass, 0 fail / 0 error.
- `test/e2e/` Node.js suite — 15 cases pass; confirms the #2280
  `RequestGroup` changes did not regress the SQLite3 persistence hook.
- Final adversarial regression set covers transfer-encoded range mismatch,
  segmented 200-without-range rejection, bounded parameter expansion,
  fail-closed write-cache updates, restart-from-scratch cache detachment,
  unsupported checksum carryover, and explicit pre-TLS Schannel protocol
  masking.
- New e2e cases for #1752, #2280, #1839 each shown to FAIL against the
  pre-fix code and PASS after; #1407 pure mask unit-tested (RED vs the
  original bit logic). The MinGW path enables `SCHANNEL_USE_BLACKLISTS`
  before including Schannel headers so both `SCH_CREDENTIALS` and
  `TLS_PARAMETERS` are available to the 32-bit and 64-bit cross compilers.
  Full Windows compile/link is validated by the release Windows lanes.
- Independent re-audit of all 45 issues by parallel review agents; their
  per-issue findings are recorded in `matrix.csv`.
