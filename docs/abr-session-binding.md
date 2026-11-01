# ABR session-binding protocol (version 2)

ABR routes use the HTTPS request's authenticated paired-client certificate UUID,
never its source address, display name, query parameters or JSON identity fields.
An unbound initial configure request binds only when exactly one eligible
RUNNING/STARTING video allocation belongs to the certificate. If overlapping
allocations make that unbound choice ambiguous, the host returns retryable
`ambiguous_session`; it never guesses which allocation is newest. A bound
request first matches both the authenticated certificate UUID and its exact
`sessionToken`, ignoring sibling allocations, then requires that exact allocation
to be RUNNING and not control-only. A fresh opaque `sessionToken` is generated
for every stream allocation, independently of the launch ID, client name and
network route. It acts as that allocation's generation identity, not as
authentication: every request must still present the owning paired certificate.

## Upgrade requirement: client first

**Upgrade to a session-binding-capable Moonlight client before upgrading to this
host.** The client must negotiate `"session_binding"`, retain the configure
response's `sessionToken`, and echo it on feedback and disable requests.

Old clients' tokenless ABR feedback and disable requests are rejected with HTTP
400 `invalid_body`. There is no compatibility fallback to source IP, display name,
caller-supplied identity or an inferred token: such a fallback could target another
client or a replacement stream. An initial configure success alone does not make
an old client's ABR feedback compatible with version 2.

## Negotiation and requests

`GET /api/abr/capabilities` returns HTTP 200 with `supported`, `version: 2`,
`features` including `"session_binding"`, `llmEnabled` and `hostMaxBitrate`.

Initial `POST /api/abr`:

```json
{"enabled":true,"minBitrate":0,"maxBitrate":80000,"mode":"balanced"}
```

`enabled` must be a boolean. Bounds are optional non-negative JSON integers
within the signed 32-bit range; zero means use effective mode/host defaults.
Decimal JSON numbers such as `20000.0` are rejected for integer fields. A positive
maximum cannot be less than the minimum. Modes are `balanced`, `quality` and
`lowLatency`. An optional `sessionToken` binds configure to that exact active
allocation.

HTTP 200 returns `success: true`, `enabled: true`, `sessionToken`, `mode`,
effective `minBitrate`/`maxBitrate`, `initialBitrate`, `requestedMaxBitrate`,
`hostMaxBitrate`, `maxBitrateCapped`, `maxBitrateInheritedFromHost`,
`bitrateApplied` and `bitrateRequested`.

`POST /api/abr/feedback` must echo the token:

```json
{"sessionToken":"opaque-allocation-token","packetLoss":6.0,"rttMs":20.0,"decodeFps":60.0,"droppedFrames":0,"currentBitrate":20000}
```

Metrics are optional (default zero), finite and non-negative. `packetLoss` is a
percentage from 0 to 100. `droppedFrames` and `currentBitrate` are signed-32-bit
non-negative integers. HTTP 200 returns `success: true`, `sessionToken`, `reason`,
`bitrateApplied`, `bitrateRequested`, and `newBitrate` only when a bitrate request
was accepted. Bitrates are the total session budget in Kbps, including FEC.

Disable uses `POST /api/abr` with `{"enabled":false,"sessionToken":"..."}`.
HTTP 200 returns `success: true`, `enabled: false` and `sessionToken`.
Disable is idempotent while that allocation is still active. If its token is
stale, stopping, stopped, control-only, or belongs to a different certificate,
the request fails with `stale_session` and cannot disable a replacement.

`bitrateApplied: true` means the request was accepted or no change was needed,
**not that the encoder committed it**. `bitrateRequested: true` means an update
was queued in the selected session's bitrate budget; it is not an encoder
acknowledgement. `newBitrate` is returned only when the feedback decision
produced a budget request. The former `bitrateApplyError` response field is no
longer emitted; host/encoder application is asynchronous and is not reported as
a synchronous HTTP failure. A configure response reports effective bounds, not
omitted zeros.

## Typed errors

All ABR handler errors have exactly this shape:

```json
{"success":false,"error_code":"session_starting","error":"Video session is starting","retryable":true}
```

| HTTP | `error_code` | Retryable |
| --- | --- | --- |
| 400 | `invalid_body` | No |
| 403 | `missing_client_identity` | No |
| 409 | `no_active_stream` | Only initial, unbound configure |
| 409 | `session_starting` | Only initial, unbound configure |
| 409 | `ambiguous_session` | Only unbound initial configure |
| 409 | `stale_session` | No |
| 409 | `abr_not_enabled` | No |
| 500 | `internal_error` | No |

The outer paired-TLS authorization policy still applies before routing.
`error` is human-readable; clients must branch on `error_code`, not its text.
Malformed JSON, non-object bodies, missing tokens on feedback/disable, wrong
field types and invalid numeric ranges return `invalid_body` without mutating ABR.
`abr_not_enabled` means ABR is not enabled for the exact allocation selected by
the request token; it does not fall back to another allocation for the same
certificate.

Clients must associate `sessionToken` with their own connection generation,
discard completions from an old generation, and never reuse a token on reconnect.
A bound request never silently rebinds to another allocation, even for the same
certificate. Only unbound initial configure discovers a unique stream; clients
must not resend old configure work after reconnect. The Moonlight client's
bounded initial-configure retry policy is five attempts within six seconds.
An old allocation can remain visible for roughly ten seconds, so retrying an
ambiguous unbound configure is best-effort and does not guarantee recovery.
Legacy clients that do not echo tokens cannot safely submit version-2 feedback
or disable; they must negotiate and implement session binding.

## Ownership and regression coverage

Each ABR request that changes the session's total budget logs one INFO line,
`ABR applied: total X->Y Kbps (reason) token_prefix=XXXXXXXX`. A configure that
leaves the total unchanged does not log one. The line records the queued
session total; `Applied video budget` reports when the encoder takes it. FEC
changes the encoder share under an unchanged total (from client loss reports
on control message `0x5510`, or the host `fec_percentage` fallback when reports
are stale), so FEC changes alone do not produce ABR-applied lines.

The immediate x0.70 emergency drop for packet loss above 5% is skipped for the
first feedback window after `enable`, which covers only stream-startup
transients; the skip is logged at INFO and sustained loss is cut on the next
window.

Encoded video frames are handed to the broadcast thread with at most 2 frames
waiting. If the broadcast thread is behind when a new frame arrives, every
waiting frame is dropped and only the newest is queued, so no standing latency
backlog can form (previously up to 32 frames could queue, then were cleared
silently). Outputs drained from AMF in one encode/collection pass (pre-analysis
or multi-HW batches) are a batch: the cap applies only to frames queued before
the batch, so a batch never supersedes its own outputs. A WARNING `Video packet queue full: dropped N superseded encoded
frame(s)...` is logged at most once per second. The client sees dropped frames
as frame loss, which can feed ABR loss reports.

Selection, token validation, lifecycle projection, controller mutation and
budget updates execute inside the streaming session-list lock. Bound requests
match the exact authenticated UUID/token pair before considering sibling
allocations; unbound configure remains fail-closed when more than one eligible
allocation exists. No selected raw pointer escapes that
critical section. Lock order is session-list lock, then ABR's controller mutex;
ABR never acquires the session-list lock. Teardown erases only its allocation's
controller entry. Existing asynchronous LLM generation checks also reject work
from an earlier controller configuration.

`abr_unit_tests` covers identity selection, lifecycle projection, request
validation, bounded rejection-log churn, controller isolation and the
applied-change log.
`abr_api_unit_tests` invokes real HTTP handlers/serialization, the production
lock-and-select helper, controller and bitrate budget with a media-free session
shim, including concurrent removal/replacement. Neither test target starts an
installed host, media stream or device.

The latest full-host CMake build and CTest evidence is recorded in the session's
`abr-review-fix-receipt.md`. Optional RTX HDR private SDK and DS5 installer assets
were unavailable; their repository availability gates remained in effect.

The host was not started. Installed-host, encoder and end-to-end Moonlight
streaming behavior remain unverified; a successful link and CTest run do not
establish live behavior.

The reported plain IPv4 / IPv4-mapped IPv6 fixture is a deterministic source
regression, not proof of the exact rejected request's candidate strings or error
body: those were absent from the retained logs. Address matching is no longer
part of ABR ownership.
