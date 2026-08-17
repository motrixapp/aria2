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

`aria2-next`'s own audit matrix lives at
`aria2-next/docs/maintenance/upstream-issue-review/matrix.csv`. Its
`state=fixed-verified`標准 was only "a new regression test for this issue
passes". That standard is not sufficient: a regression test can exercise
an isolated primitive while the production call chain never reaches the
fixed code. This audit re-checks every fix against the real chain.

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
| `test_ref` | the test that spans the real call chain (CppUnit method or e2e file), or `regression-only:<ref>` when only the upstream primitive test exists |
| `evidence` | this fork's evidence for the `local_state` verdict |

### `local_state` values

| value | meaning |
|-------|---------|
| `verified-effective` | fix reaches and works on the production call chain; regression + build/e2e green |
| `defective-rewritten` | ported fix was defective (dead code / wrong semantics / security hole); rewritten here and verified with a chain-spanning test |
| `ported-tests-pass` | ported unchanged; regression test and full `make check` + e2e suite pass, production-path effectiveness accepted from that evidence |

## The four rewritten defects

Four ported fixes were found defective under adversarial review and
**rewritten** rather than trusted (a fifth, #1280, was found defective
while running `make check` and also rewritten):

| issue | defect | rewrite | chain-spanning test |
|-------|--------|---------|---------------------|
| #1752 | `createJsonRpcErrorResponse()` hard-coded `AUTHORIZED`; any malformed WS message (parse error, non-object, empty batch, control frame) marked the session authorized and it then received all notifications | error responses now report `NOTAUTHORIZED`; added `all_authorized()` with a non-empty guard for batches; both WS and HTTP batch paths use it | `test/RpcHelperTest.cc` (7 cases) + `test/e2e/websocket-auth.e2e.test.mjs` |
| #2280 | `createCheckIntegrityEntry()` took `FileOpenMode` but dropped it before the `loadAndOpenFile()` calls, so the `RESTART_FROM_SCRATCH` branch was dead; a changed-remote 200 resumed against stale bytes | thread `FileOpenMode` through all four `loadAndOpenFile()` calls and guard both the resume and finished-by-length short-circuits; `HttpResponseCommand` passes `RESTART_FROM_SCRATCH` on a conditional 200 | `test/RequestGroupTest.cc::testCreateCheckIntegrityEntryRestartFromScratch` + `test/e2e/conditional-get-restart.e2e.test.mjs` |
| #1407 | modern-Schannel path AND-ed `~PROTO` into a zero `grbitDisabledProtocols`, which stays zero for every version, so `--min-tls-version` was a no-op | extracted the black-list mask into a pure, host-testable `winTLSDisabledProtocols()`; WinTLSContext uses it | `test/WinTLSProtocolsTest.cc` (3 cases) + mingw cross-compile parity check |
| #1839 | 503 retries reset the try count on every wake, so a permanently-503 server retried forever (max-tries never reached) | keep "503 does not consume the permanent budget", add an independent consecutive-503 counter capped at `max-tries` | `test/e2e/503-max-tries.e2e.test.mjs` |
| #1280 | `sqlite3_open_v2()` opens lazily and returns `SQLITE_OK` for a read-only WAL db; the "open failed → retry immutable URI" fallback never triggered, so Firefox WAL cookie import failed at read time | probe with a trivial read after opening and fall back to the immutable URI when the probe fails | `test/Sqlite3CookieParserTest.cc::testMozParse_readOnlyWalSnapshot` |

## Verification performed

- `make check` (full CppUnit suite, 1078 tests) — pass, 0 fail / 0 error.
- `test/e2e/` Node.js suite (existing 11 cases) — pass; confirms the #2280
  `RequestGroup` changes did not regress the SQLite3 persistence hook.
- New e2e cases for #1752, #2280, #1839 — each shown to FAIL against the
  pre-fix code (attacker receives the GID notification; the stale file is
  retained; the 503 download never terminates) and PASS after the fix.
- #1407: pure mask unit-tested (shown to fail against the original bit
  logic); WinTLSContext.cc produces no new errors vs. the ported original
  under the ubuntu mingw-w64 cross toolchain (the residual errors are a
  pre-existing `SCH_CREDENTIALS`-type gap in that toolchain, not this
  change). Full Windows compile/link is validated by the Windows CI lane.
