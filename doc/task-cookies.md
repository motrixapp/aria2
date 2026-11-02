# Task cookies RPC (Motrix fork)

[简体中文](task-cookies.zh-CN.md)

Available starting with `v1.37.0-motrix.13`. Use `system.listMethods` to
discover these methods. An older engine returns method-not-found; clients must
not fall back to an unauthenticated `addUri` or a raw `Cookie` header.

## Create a task

`aria2.addUriWithCookies(uris, cookies[, options[, position]])` returns a GID.
Authentication uses the normal RPC token parameter. URIs must all be HTTP or
HTTPS. Options and queue position have the same meaning as `aria2.addUri`.
The whole cookie array is validated before a task is created.

```json
{
  "jsonrpc": "2.0",
  "id": "download",
  "method": "aria2.addUriWithCookies",
  "params": [
    "token:YOUR_RPC_SECRET",
    ["https://downloads.example.org/files/archive.zip"],
    [{"name": "session", "value": "EXAMPLE_ONLY", "domain": "downloads.example.org", "path": "/files", "hostOnly": true, "secure": true}],
    {"out": "archive.zip"}
  ]
}
```

Cookie fields:

| Field | Type | Meaning |
| --- | --- | --- |
| `name`, `value`, `domain` | string, required | Cookie identity, content and ASCII host/domain. Use punycode for internationalized domains. |
| `path` | string | Defaults to `/`; must begin with `/`. |
| `hostOnly` | boolean | Exact-host matching. If absent, defaults to false for a leading-dot domain and true otherwise. IP addresses always match exactly. |
| `secure` | boolean | Defaults to false; when true, send only over HTTPS. |
| `httpOnly` | boolean | Defaults to false; preserved as metadata. aria2 has no script access to cookies. |
| `expiresAt` | integer | Unix **milliseconds**, from 0 through JavaScript's maximum safe integer. Omit for a session cookie; explicit zero or a past timestamp is expired. Matching uses the native store's whole-second clock. |

Unknown fields and incorrect types are rejected. In particular, `sameSite` is
not accepted: browser site/navigation policy must be applied by the caller
before exporting cookies. The caller is responsible for supplying authorized
domain scopes; aria2 does not reconstruct a browser's browsing context or
public-suffix policy. Values and paths cannot contain control characters or
semicolons; names must be HTTP tokens. An array may contain at most 300 records,
each name plus value at most 4096 bytes, domain at most 254 bytes, and path at
most 4096 bytes. The existing cookie store retains at most 50 cookies per domain.

Each task receives an independent store. An empty array explicitly creates an
empty, isolated store. Neither startup `--load-cookies` nor cookies received by
another task are inherited. Task responses update only this store.
All connections, retries, pause/resume cycles and generated descendants of the
logical task use that context. Cookie domain, path, expiry and Secure rules are
evaluated for each outgoing request, including redirects and Range requests.
Raw `Cookie` headers supplied in creation options or inherited from global
options are removed; the structured store is authoritative.

## Persistence and restart

Starting with `v1.37.0-motrix.14`, when SQLite persistence is enabled, aria2
stores each task's cookie context in dedicated `task_cookie_context` and
`task_cookie` tables. The task row and its cookie snapshot are updated in one
transaction. A context row also represents an explicitly empty jar. Cookies
received in `Set-Cookie` responses are flushed immediately, including deletions,
so a crash cannot normally roll the task back to a previously submitted value.
A transient write failure marks the task for retry during the next periodic
save. Unchanged jars are not rewritten on every periodic save. Expired
persistent-cookie rows are pruned before restoration.

Cookie values are stored as plaintext in the local SQLite database and may also
temporarily exist in its WAL. Protect the database and its parent directory with
the same user-only permissions as other Motrix application data. Deleted SQLite
records use `secure_delete=FAST`. Values are never copied into text sessions,
download history, engine logs or `--save-cookies` output.

On startup, aria2 restores the scoped jar before scheduling the task. Pause,
retry and shutdown retain it. With `--force-save=true`, failed unfinished
downloads retain their cookie context together with their task and checkpoint,
including explicitly empty jars. Normal completion, cancellation and explicit
result removal delete the cookie context; deleting the task row also cascades
to both cookie tables.

If SQLite persistence is disabled, or a marked task has no cookie context after
database loss/corruption, it fails before making a network request. Restore it
paused, call `aria2.setTaskCookies(gid, cookies)`, then call
`aria2.unpause(gid)`. `setTaskCookies` replaces the complete jar atomically,
including an empty jar, and durably updates SQLite when enabled. It is allowed
only for waiting or paused tasks carrying the requirement marker. Active,
stopped and ordinary legacy tasks are rejected. Pause an active task and wait
for `status=paused` before refreshing its context.

## Verified HTTP continuation

Discover `aria2.getHttpRetryStatusV1` and `aria2.retryHttpDownloadV1` through
`system.listMethods`; both require enabled SQLite persistence. Clients can opt
new single-source HTTP downloads into `http-resume-context=true`. Before the
first payload write, the engine binds a strong ETag (or valid Last-Modified and
length), the final response URL hash, original source hash and output path.
HTTP Link mirrors are disabled for this mode. Each subsequent payload response
must match that binding, including after an engine restart.

Both methods take `(gid, originalUri, outputPath)`. These are assertions about
the original task, not overrides. `getHttpRetryStatusV1` returns
`{ available: "true" | "false", reason: string }` without exposing cookies or
other request credentials. `retryHttpDownloadV1` returns the original GID and
continues its original file/checkpoint with its retained request and scoped jar.
It commits the resumed task and retires stale error history before queueing it.
Repeated calls acknowledge an already running or completed matching task.

Retry requires a retained historical resource identity and checkpoint. It never
uses a newly captured ETag to validate old bytes. Missing cookies, missing
checkpoints, changed source/path, changed resources and unsupported Range
responses cannot silently restart or overwrite the partial download. Old tasks
without a binding need a separately verified recovery or an explicit fresh
download. Completion and explicit task/result removal retire the binding.
Retained failed tasks are restored paused, so restarting the engine does not
automatically repeat the failed request.

## Legacy requests and redirects

`aria2.addUri` and startup cookie files keep their existing global-store behavior.
For all HTTP tasks, explicitly supplied `Cookie:` and `Authorization:` headers
are omitted when the current request differs from that URI's original scheme,
host or effective port. They remain available for same-origin requests.
Other custom headers keep their previous behavior. Structured cookies follow
their own domain/path/Secure rules: cookie scope is not an origin/port boundary.
This change concerns custom headers, not a replacement for all authentication
or proxy policy in aria2.

## Acceptance

Run `make check`, then:

```sh
ARIA2_E2E_BIN="$PWD/src/aria2c" node --test --test-concurrency=1 test/e2e/task-cookies.e2e.test.mjs test/e2e/http-retry.e2e.test.mjs
```

The contract covers task isolation, scoped redirects, expiry, HTTPS downgrade,
raw headers, malformed input, Range pause/resume, SQLite migration, restart
recovery, server cookie rotation and terminal cleanup.
Release publication runs the same contract against the six packaged macOS,
Windows and Linux x64/arm64 binaries (Windows includes ia32).
