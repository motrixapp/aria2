# HTTP redirect policy

`max-http-redirect` limits each HTTP redirect chain to 0–20 redirects. The
default remains 20. Setting it to 0 rejects the first redirect with error code
23 before requesting its target. This includes HTTP 300, 301, 302, 303, 307 and
308 responses with a Location header. A zero limit also disables admission of
HTTP Link mirrors, which could otherwise bypass a rejected redirect or retry
against a different resource after a response fails.

Set it on the command line or per download through `aria2.addUri`. It supports
`aria2.changeOption`, `aria2.getOption`, global options and session persistence.

Clients that rely on this restriction must first check that live
`aria2.getVersion` results include `HttpRedirectPolicyV1`. Older engines may
silently ignore unknown RPC options. A strong ETag alone cannot establish
identity across different redirect targets, so downloads that reuse verified
local bytes should pin this option to 0.
