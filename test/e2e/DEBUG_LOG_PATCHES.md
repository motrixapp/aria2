# Temporary Debug Log Patches

This file tracks every temporary `A2_LOG_*` / `printf` / `std::cerr` line
added to the aria2 source tree while diagnosing the sqlite3-persistence
restart-resume bug. **Every entry MUST be removed before the fix is
considered complete**; the final commit must contain none of them.

After removing all entries below, rebuild via:

```bash
./scripts/rebuild-static-osx.sh --no-deps
```

and re-run the e2e suite to confirm tests still pass with the production
log level only.

## Active patches

| File | Line(s) | Snippet | Added at | Removed |
|------|---------|---------|----------|---------|
| _none_ | | | | |

## Removed patches (audit trail)

| File | Snippet | Removed at | Verified rebuild green |
|------|---------|------------|------------------------|
| `src/download_helper.cc:~199` | `A2_LOG_NOTICE("[E2E-DEBUG] createBtRequestGroup metaInfoUri=%s fileEntries=%zu totalLength=%lld", ...)` (post-`bittorrent::loadFromMemory`) | 2026-04-29 | ✅ rebuilt via `./scripts/rebuild-static-osx.sh --no-deps`; e2e suite still 4/4 green |
| `src/RequestGroupMan.cc:~470` | `A2_LOG_NOTICE("[E2E-DEBUG] stopped-group cleanup gid=%s force-save=%s result=%d", ...)` inside the BT halt-cleanup branch | 2026-04-29 | ✅ rebuilt via `./scripts/rebuild-static-osx.sh --no-deps`; e2e green after the saveAllTasks live-set fix |
