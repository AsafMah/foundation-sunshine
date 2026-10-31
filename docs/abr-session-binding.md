# ABR session-binding protocol (version 2)

ABR routes use the HTTPS request's authenticated paired-client certificate UUID,
never its source address, display name, query parameters or JSON identity fields.
The host selects exactly one RUNNING video session owned by that UUID.
Control-only and stopped/stopping sessions are excluded. Multiple RUNNING/STARTING
video candidates for the same certificate are rejected, including reconnect overlap.

The first configure request binds to the currently unique video session. A fresh
opaque `sessionToken` is generated for every stream allocation, independently of
the launch ID, client name and network route. The token is not authentication:
every request must still present the owning paired certificate.

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

`enabled` must be a boolean. Bounds are optional non-negative integers within the
signed 32-bit range; zero means use effective mode/host defaults. A positive
maximum cannot be less than the minimum. Modes are `balanced`, `quality` and
`lowLatency`. An optional `sessionToken` must match the current allocation.

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
Disable is idempotent while that allocation is still active.

For compatibility, `bitrateApplied: true` means the request was accepted or no
change was needed, **not that the encoder committed it**. `bitrateRequested: true`
means an update was queued in the session's bitrate budget; it is not an encoder
acknowledgement. A configure response reports effective bounds, not omitted zeros.

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
| 409 | `ambiguous_session` | No |
| 409 | `stale_session` | No |
| 409 | `abr_not_enabled` | No |
| 500 | `internal_error` | No |

The outer paired-TLS authorization policy still applies before routing.
`error` is human-readable; clients must branch on `error_code`, not its text.
Malformed JSON, non-object bodies, missing tokens on feedback/disable, wrong
field types and invalid numeric ranges return `invalid_body` without mutating ABR.

Clients must discard completions from an old connection generation and must not
reuse a token on reconnect. A bound request never silently rebinds to another
allocation, even for the same certificate. Only initial configure discovers the
currently unique stream; clients must not resend old configure work after reconnect.
Legacy clients that do not echo tokens cannot safely submit version-2 feedback
or disable; they must negotiate and implement session binding.

## Ownership and regression coverage

Selection, token validation, controller mutation and budget updates execute
inside the streaming session-list lock. No selected raw pointer escapes that
critical section. Lock order is session-list lock, then ABR's controller mutex;
ABR never acquires the session-list lock. Teardown erases only its allocation's
controller entry. Existing asynchronous LLM generation checks also reject work
from an earlier controller configuration.

`abr_unit_tests` covers identity selection, request validation and controller
isolation. `abr_api_unit_tests` invokes real HTTP handlers/serialization, the
production lock-and-select helper, controller and bitrate budget with a media-free
session shim, including concurrent removal/replacement. Neither test target starts
an installed host, media stream or device.

Local focused tests and production translation-unit type-checks passed. A
subsequent repository CMake Release build compiled the current host sources and
linked `sunshine` using the required Boost 1.92. Both registered ABR CTest targets
passed (14 engine/identity/validation tests and 9 HTTP/ownership tests). The
earlier direct focused builds used Boost 1.91 and are not the evidence for this
full-build result. Optional RTX HDR private SDK and DS5 installer assets were
unavailable; their repository availability gates remained in effect.

The host was not started. Installed-host, encoder and end-to-end Moonlight
streaming behavior remain unverified; a successful link and CTest run do not
establish live behavior.

The reported plain IPv4 / IPv4-mapped IPv6 fixture is a deterministic source
regression, not proof of the exact rejected request's candidate strings or error
body: those were absent from the retained logs. Address matching is no longer
part of ABR ownership.
