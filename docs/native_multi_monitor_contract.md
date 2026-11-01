# Native multi-monitor contracts (P0)

**Unimplemented product design.** P0 supplied executable, standalone contracts
and fixtures without changing production call sites. The user reviewed G0 and
authorized P1. P1 is in progress; its serialized command-queue foundation is
not yet connected to production display operations. This does **not** enable
native multi-monitor streaming, new configuration, a driver interface, or a
protocol.

The reviewed architecture's P0 scope (section 9.1) is the boundary of this
document. Its later phases are not implementation authorization.

## Boundaries and ownership

The eventual design is one Sunshine process, one logical session with
monitor-scoped video tracks, and one audio/control/input path. Source topology,
stream topology, and client presentation are separate. Fullscreen/windowed
presentation must not change host geometry or recreate a virtual display.
Physical displays are borrowed read-only by default. Normal shared host cursor
movement is retained; no confinement policy is introduced here.

There is one topology-changing owner, including pending startup, reconnect
grace, and unresolved recovery. Observers require explicit view/input grants;
they never acquire topology ownership through connection, expiry, or owner
departure. Principal UUIDs and event authorization flags in these pure types
are **trusted coordinator inputs, not authentication or client-supplied proof**.
P1 must derive them from current pairing/policy authority.

P0 contains no CCD, DXGI, SetupAPI, VDD IOCTL, GPU, capture, encoder, filesystem
journal, network, or input-injection operations. P1 currently adds a serialized
command queue with generation invalidation, per-command cancellation handles,
and deadlines, plus standalone fake-clock tests. Per-command handles let launch
expiry cancel only its associated monitor operation. The queue is not yet wired
into production and does not authorize platform operations. There are no changes
to active display routing: the launch helper now accepts shared requests and an
optional staged adapter, but its production callers still use the direct path.
Cross-GPU transfer is excluded, not an automatic fallback.

## Code and existing-type adoption

| File under `src/display_device/` | Pure responsibility |
|---|---|
| `topology.h`, `topology.cpp` | Identities; observed, desired and applied values; exact resolution; validation; request history; one-way legacy synthesis |
| `topology_reservations.h` | Generic revisioned reservation ledger and P1 resource keys |
| `topology_lifecycle.h`, `topology_lifecycle.cpp` | Lease reducer; fixed-slot capacity snapshots; exact owned-head reuse check |
| `topology_journal.h`, `topology_journal.cpp` | In-memory versioned journal schema, validation and JSON round-trip |

Existing `display_device.h` types are embedded directly: `resolution_t`,
`refresh_rate_t`, `display_mode_t`, `device_info_t`, `device_state_e`,
`hdr_state_e`, and `active_topology_t`. Legacy intent embeds the existing
`parsed_config_t` and its preparation enums/optional values. Only their headers
are consumed; existing production functions are neither called nor linked.

Adoption boundaries for G0:

| Existing surface | P0 treatment and intended adapter |
|---|---|
| `device_info_map_t` / string OS IDs | Reuse `device_info_t`, wrap the existing ID string explicitly. Inventory is a vector so duplicate identity evidence remains representable rather than disappearing in a map. P1 adapts successful enumeration and preserves ambiguity. |
| `active_topology_t` | Use directly for unordered clone/extend groups. Do not encode navigation order or coordinates in its vector ordering. |
| `parsed_config_t` | Embed its value data, not its production parser or static conversion methods. Preserve original scalar selection separately from a parser-resolved fallback. |
| `enum_available_devices_checked()` and Windows ID derivation | P1 supplies observed snapshots. A failed observation is `available == false`, never a successful empty desktop. EDID/instance/path-derived IDs remain observed evidence, not portable UUIDs. |
| VDD aliases and lifecycle settings | P1 passes the existing parser's `use_vdd` classification, raw `output_name`, and keep-enabled/headless-create/reuse values. The alias lives in production globals; P0 does not link it or reimplement VDD detection by friendly name. |
| Windows LUID / full head correlation | P1/D provide explicit full-width LUID bits and proven OS-device/head associations. No dimensions, EDID serial, connector arithmetic, or live Windows discovery in P0. |
| Frame/capture contracts | P2 adapts source incarnation and adapter identities to existing frame-domain/generation contracts. P0 has no frame or encoder consumer. |

## Identity, profiles, and resolution

Identity wrappers have explicit construction and no cross-domain conversions.
Profile and monitor UUIDs, OS display IDs, full head GUIDs, driver-instance IDs,
adapter LUIDs, head/source incarnations, track IDs/epochs, and topology,
inventory, and ledger revisions are distinct types. UUID storage retains all
128 bits; incarnations and epochs are 64-bit. Identity generation and
cross-restart persistence are outside P0.

A persisted virtual selector identifies a full logical head GUID, owner and
adapter. Its resolved runtime token additionally identifies the driver
instance, head incarnation, slot, OS display ID and source incarnation. A
recreated head can retain logical selection while producing a different bound
runtime identity. Missing/ambiguous correlation is not continuity.

`resolve_profile()` returns an explicit result per entry:

| Outcome | Meaning |
|---|---|
| `bound` | One exact, active OS source; current source incarnation; known matching adapter; full owned-head correlation when virtual |
| `unavailable` | Failed inventory, unavailable applied source, missing virtual head, unproven ownership, or adapter mismatch |
| `needs_rebind` | Missing/ambiguous physical ID, ambiguous head identity, or inconsistent OS/head association |

Friendly names and display numbers are labels only. Two equal-sized virtual
heads whose GUIDs share the EDID `Data1` portion remain different sources.
Unknown occupied heads cannot be bound as owned.

Profile entry storage, explicit navigation order, fallback/preferred monitor,
and OS-primary intent are separate. Geometry is **already oriented
host-desktop pixels**: signed position plus the existing mode's width/height,
not DIPs or unrotated panel dimensions. A portrait rotation must not swap these
extents a second time. Negative origins, gaps, and a selected subset without
the actual OS primary are valid.

`validate_profile()` rejects duplicate logical/source identities, invalid
navigation sets, zero dimensions/rates, unsupported rotation, coordinate or
pixel-rate overflow, exceeded supplied limits, stale topology revisions,
unauthorized edits, unsupported declared modes, clone-source duplication,
partial clone-group edits, and overlap. It checks selected geometry against
unmanaged active displays too. Existing unedited clones are not treated as
ordinary extended-layout overlap.

Limits are **caller-supplied policy**, not a claim about hardware support.
Rational pixel rates round upward for conservative accounting. This is not the
P2 hardware/session/VRAM/network/decoder admission model. Missing selections
can remain desired configuration; successful structural validation does not
turn an unavailable resolver result into an admitted source. Known adapter,
ownership and ambiguous-binding errors remain explicit refusals.

## Requested and applied state

`requested_topology_t` includes owner/request identity, base topology revision,
inventory revision and the complete profile. `applied_topology_t` is a separate
value: actual source/layout/primary readback, newer topology revision, and an
explicit applied/adjusted/unavailable/conflict outcome for every entry.

Successful applied entries require actual identity and geometry, valid mode
limits, and the requested physical or full virtual source/adapter. They cannot
substitute an unrelated display. Adjustments need prior permission and a
reason. Required entries cannot silently disappear; optional degradation
requires prior policy and an explicit reason.

These checks validate the record, not the truth of an OS operation. P1 still
must obtain actual inventory/readback, revalidate host-wide constraints and
external drift, and publish only after required readiness. There is no P0
platform executor or media-ready claim.

Request identity is `(owner, request ID)`. An exact full-body replay returns
the original receipt/result, even if the current topology revision has moved.
A changed body under the same identity conflicts. Applied completion is
idempotent only for the exact same result.

**User-selected replay policy:** request and reservation histories never evict
accepted identities. At the caller-supplied history bound, new requests fail
with `history_full`; retained retries still work. There is no implicit
deadline renewal, ID reuse, retention epoch, compaction, or cross-restart
guarantee. Those require a P1-reviewed retirement/persistence design.

## Lease reducer and reservations

The reducer accepts explicit state, event, monotonic time, and the reservation
ledger. It returns a new state/disposition and, when appropriate, a release
intent. Invalid transitions return typed errors without modifying inputs;
obsolete operation tokens return unchanged state with `ignored_stale`.

The normal path is:

```text
idle -> reserved -> preparing -> applying -> settling -> active
     -> draining -> restoring -> idle
```

`aborted`, explicitly permitted `degraded`, and `recovery_required` are separate
outcomes. Lease tokens retain owner, transaction, base revision and generation;
an unrelated global ledger revision does not invalidate an operation token.

Before any possible side effect, **including VDD creation while preparing**:

1. Reserve owner and required free-slot capacity.
2. P1 must preserve the first baseline and durably record intent.
3. Commit the protected owner/slot hold.
4. Authorize mutation through the reducer.

Before mutation authorization, ticket expiry/cancellation can abort and request
release. After authorization, it retains a recovery fence until restoration
is acknowledged. A late relevant completion may add recovery evidence but
cannot activate the transaction. A released hold cannot publish late success.
The reducer does not itself persist a journal or execute its release intent.
That intent refers to the input state's `protected_hold`; the P1 queue must
retain that exact token while applying the returned state, which clears it.
It must inspect that reservation to roll back a pending hold, release a
committed hold, or preserve an already-terminal record without another spend
or release.

Disconnect disables input immediately and starts finite owner grace.
Repeated disconnect does not renew it. Reconnect revalidates the same
principal/lease and current authorization; revoked input does not revive.
Observers have explicit subject, owner generation, view/input and expiry
fields. There is one effective input controller. Revocation/expiry does not
automatically hand input or topology ownership to another principal.

The generic ledger reserves complete resource sets atomically, counts pending
and committed holdings, checks admission revisions, and checks expiry on
commit even before an expiry sweep. Commit/rollback/release are exactly-once
state changes; repeats return `already_finalized`, not another release.
Pending expiry is explicit. **Committed holdings never expire automatically.**
The P1 queue must sweep pending tickets and execute validated release intents.

P0 instantiates only:

- one topology-owner resource;
- bounded pending launch-ticket capacity;
- fixed `(driver instance, adapter, slot)` VDD capacity.

Every observed occupied slot has zero free capacity, including unknown owners.
Exact proven-owned reuse is checked separately and does not allocate the
occupied slot a second time. Rolling back a new reservation cannot free an
observed occupied slot. A new driver instance at the same numeric slot is not
the old head. Exhaustion returns a typed refusal, never reload/global destroy.
GPU/source/memory/network/decode accounting, replacement headroom and failure
cooldowns remain P2.

## In-memory recovery journal

Schema version 1 records:

- the first full observed baseline, including unmanaged displays, clone groups
  and known/unknown audio endpoint;
- owner/request/transaction/base revision;
- exact managed source identities and baseline/intended/confirmed field values;
- owned full runtime head tokens;
- ordered planned/attempted/observed/failed/conflict operation records and
  observed snapshots;
- separately managed audio endpoint baseline/intended/confirmed values.

Retried prepare for the same transaction keeps the first baseline. Another
transaction cannot replace it. Active unknown baseline geometry is refused.
Managed baseline fields and source incarnation must match the first observed
state; confirmed fields cannot expand the managed scope. Full baseline data
is evidence, **not whole-desktop restoration authority**. Head intent GUIDs
are not authority to delete a current head.

The decoder rejects unknown schema versions, invalid/truncated identities,
invalid numeric/enumerated values and inconsistent records. Its in-memory
schema bounds are 1 MiB and 4,096 records per array, not monitor support limits.
There is no disk access, migration, selective-restore executor or crash-replay
implementation. P1 must conditionally restore only still-matching managed
values/current identities, retain external-edit conflicts, and never invoke
legacy whole-desktop restoration for a managed transaction.

## Legacy synthesis

`synthesize_legacy()` is one-way and pure. The caller supplies durable profile,
monitor and (for VDD) head/owner identities. It produces exactly one ordered,
required entry with matching fallback/preferred IDs, and preserves the
existing parsed configuration and lifecycle flags as explicit legacy intent.
It neither rewrites configuration nor provides a reverse serializer.

Empty `output_name` stays symbolic legacy-primary intent. An unknown named
selection retains its original ID; it is not rebound by size/name/default.
The existing `use_vdd` classification produces a virtual selector with its own
full GUID. Destructive legacy preparation enums and keep-enabled flags are
retained, not converted into implicit native permission grants.

**P1 compatibility obligation:** today's unknown host selector can fall back
to the primary display, unlike a client-named missing selection. P0 records
that unresolved compatibility intent without changing existing behavior.
P1 must explicitly adapt/test the old single-track policy; it must not route
legacy requests through the strict native resolver and accidentally change
that behavior. Raw aliases, parser policy and lifecycle settings remain owned
by existing production code until that adapter is reviewed.

## Fixtures, evidence, and G0

`display_topology_unit_tests` is registered in `tests/CMakeLists.txt`; its
sources and fixture header are excluded from the aggregate suite. It links
only the new pure sources, GoogleTest, JSON, Boost headers and project test
options. Test support is inventory builders, a deterministic clock, and
assertion helpers, not fake OS/driver/media implementations.

The fixtures cover full-width identity separation; two physical and mixed
physical/two-identical-virtual resolution; reordered/same-name/ambiguous
inventory; unknown occupancy and adapter mismatch; geometry/clone/permission/
revision errors; requested/applied distinction and exact replay; table-driven
lease ownership, expiry, grace and late completions; atomic reservations and
exact-once finalization; journal round-trip/first baseline/version rejection;
and complete golden legacy profiles.

With the repository's normal Windows build dependencies installed:

```powershell
cmake --preset test-win
cmake --build --preset test-win --target display_topology_unit_tests --parallel 2
ctest --preset test-win -R "^display_topology_unit_tests$" --output-on-failure
```

G0 validation additionally exercises the existing `video_probe_unit_tests`,
`frame_contract_unit_tests` and `config_overrides_unit_tests`. The implementation
session uses the exact registered target declarations in a focused CMake
harness rather than configuring unrelated host/driver dependencies. This is
not a full host build, aggregate-suite run or a live Sunshine test.

Every new test must also fail against a deliberately broken rule before its
G0 evidence is accepted. The session's mutation harness compiles temporary
copies, uses the actual target's compiler commands, and accepts only normal
GoogleTest failures inside the selected test body. Build errors, crashes,
setup failures and untouched tests do not count. Source-hashed receipts are
kept with the implementation session; mutants are not shipping switches.

The G0 implementation receipt is **59 passing contract tests, 75 killed rule
mutations covering every new test, and three passing existing standalone
suites**. Six deliberate cross-domain/implicit-conversion compile probes were
rejected, with valid explicit construction passing under the same compiler
flags. Each accepted mutation produced an assertion in its selected test body;
the final source hashes match the receipt and generated mutant sources,
objects and executables were removed. The mutation pass also added an absent-
inventory duplicate-source case: the bound-source check had masked removal of
the distinct desired-selector check.

Independent pre-implementation critique was incorporated: fence potential
effects before mutation, retain exact replay without eviction (user choice),
distinguish late recovery evidence from obsolete tokens, preserve actual
baseline knownness, validate unmanaged host geometry, separate occupied-slot
reuse from free capacity, bind observer grants to subject/generation/lifetime,
and keep baseline evidence separate from restoration authority.

| Gate | Required before proceeding |
|---|---|
| **G0 / P1** | User reviewed these P0 contracts and authorized P1; G1 remains pending |
| **GD / driver D** | User/maintainer decides upstream/fork, signing, distribution and minimum driver capability/version policy |
| **G1 / P2** | P1 ownership/journal/bypass-closure and unchanged N=1 evidence |
| **G2 / P3** | Distinct source/capture and P2 aggregate admission/replacement evidence |
| **GW / production wire** | Binding spike and reviewed protocol/KDF/nonce design |
| **GH / any real hardware run** | Separate user consent and a dedicated fixture with baseline and cleanup |
| **G3 / server support claim** | One physical plus two virtual sources concurrently delivered from one process/session |
| **G4 / end-user UX claim** | Real client presentation/input evidence with zero host writes on presentation toggles |

P0 does not choose history retirement, implement the legacy adapter, establish
Windows full-head correlation, decide driver distribution, or qualify hardware
limits. Those remain explicit later-gate obligations, not silent fallbacks.

## P1 implementation status

The standalone coordinator and native-executor targets exercise single-worker
FIFO execution, worker-only operation capabilities, admission and lease policy,
per-command cancellation, setup deadlines, durable file journal storage,
strict one-monitor profile parsing and ownership-aware recovery using fake
platform operations. Native replay preserves unmanaged fields and fails closed
on unknown ownership. These implementations are still unwired.

Native platform mutation entries now require the original live capability and
are final, checked wrappers over nonpublic backend hooks. Creation uses native
preparation authority; same-command failed-create rollback remains possible.
Startup can replay fields, but cannot create or remove heads; P1 preparation
cannot write host fields. Executor helpers preserve the same capability through
creation, rollback, restore and recovery, without changing ownership proofs.
Four fake-backed fixtures cover all 18 command kinds, three mutation surfaces
and both modes, off-worker rejection, absence of ungated public overloads and
actual executor forwarding. They fail against the former ungated interface and
under 13 deliberate faults.

An attempted native create remains unresolved even when inventory is empty:
the write-ahead image also survives a crash or failure to persist the result,
and a head may still appear later. Recovery and restoration preserve that
journal and block field writes until explicit reset. Settled or unstarted
creation without a head can still clear; interrupted field-only writes still
replay selectively. An incompletely persisted rollback likewise cannot be
settled merely by seeing the other displays return to their baseline. Three
regressions fail on the former recovery path; five deliberate mutations cover
restart, mixed-step journals and the clearable/replay boundaries. All 87 native
suite cases pass after byte-for-byte restoration.
The concrete Windows native backend and shared raw-setter gates remain pending.

Posted commands now offer individual completion receipts, including eager
rejection, worker-side rejection, failure and shutdown cancellation before a
body starts. Retry timers can associate receipts with their own generations;
restore callers can keep a watchdog armed until completion without putting a
waiter thread on every command. Accepted commands notify the observer exactly
once, including commands whose body never ran. Observer failures are exposed
through the receipt. A launch-ticket presence query is also implemented and
tested.

Deferred legacy startup restoration has a dedicated, restricted continuation.
Only the initial startup worker can register its immutable callback; ordinary
run/post routes cannot supply a replacement body. It can restore legacy state,
not configure displays, invoke native recovery or create/destroy heads. Pending
restoration blocks managed admission, while preserving the approved legacy
locked-desktop baseline merge. An attempt excludes admission before an unlocked
probe of all preparation/ticket/startup/session activity, then rechecks ownership,
phase and independent fences. Exclusion and coalesced receipts last through the
posted observer, including observer failure after successful restoration.
Failure retains the obligation; explicit reset invalidates queued attempts.
Callback captures and previous failed receipts are released outside the mutex.

Ten new fake-backed cases bring the coordinator target to 138 tests. Twenty-seven
deliberate faults fail the intended assertions, including two bounded deadlock
mutations. The expanded native-boundary matrix rejects the continuation at all
three native mutation surfaces in both modes and catches deliberately broadened
authority. These checks do not wire production startup recovery or its retry
timer: the runtime must supply the complete activity probe and preserve the
teardown fence from slot removal through cleanup submission.

The transitive authority table now distinguishes topology, mode, primary, HDR,
color association, SETMODES, cursor and adapter-restart writes. Its independent
matrix covers 18 command kinds, 20 operations and both modes. Actual-worker
fixtures verify restore-only authority and thread confinement, and distinguish
native creation from legacy preparation and narrower deferred retry. Eighteen
deliberate faults fail the intended assertions; the coordinator suite now has
140 cases. Managed legacy VDD routes and restoring reset are denied without
blocking native journal-first creation or proven owned removal. The table does
not yet gate the production raw signatures; journal suppression and ownership
remain separate runtime obligations.

The neutral retry scheduler now pairs each attempt with a distinct generation,
waits for its receipt, and re-arms failed/rejected work unless shutdown prevents
another attempt. Replacing or disabling a timer invalidates old completions.
An interleaving fixture exercises an accepted tick rejected later by the real
coordinator worker. The production timer has not yet been converted to this
scheduler.

The coordinator enforces its recorded setup deadline through successful ticket
bind, not merely through display preparation. Tick, bind and subsequent admission
reap expired preparing leases and cancel only their command. Cleanup is enqueued
before a replacement may mutate, and only if the expired prepare body started.
Legacy callers can register their expiry cleanup with preparation; managed
leases use native restore. A successful bind switches to ticket liveness rather
than continuing to apply the setup deadline. Binding after draining/stop is
refused. Bounding application prep and rolling back published tickets/apps are
still production-wiring work, not established by these isolated tests.

Staged preparation now supports separate configure/probe/fallback steps under
one owner token and the original deadline. It cannot mix with the one-shot API,
overlap steps, continue after failure/sealing, or bind before explicit completion.
The first cleanup obligation remains registered throughout; a sticky record of
any started mutation survives later steps that never start. Abort/expiry enqueue
that cleanup before releasing ownership. Sealing and ticket binding also refuse
active probe/admission fences. These orchestration fixtures use fake mutations
and probes; the optional real-helper adapter below is not yet enabled by its
production callers.

The standalone `nvhttp_stream_start_unit_tests` target now exercises the actual
legacy startup helper with link-time fakes for display configuration, inventory
intent, VDD readiness, encoder probing, HDR adoption and touch-keyboard effects.
It preserves preflight refusals, exact/current/VDD probe targets, rich errors,
bounded deferred retries, fallback ordering, cleanup ownership, recovery-session
field propagation and temporary encoder-setting restoration. This is a
production-policy regression baseline, not evidence that launch admission,
asynchronous request lifetime or coordinator routing is connected.

Independent fences now compose instead of replacing one another. A shared
teardown token can block admission from registry removal through cleanup
submission, including asynchronous app-cancel callbacks. Releasing one holder
does not release another holder's fence. Cleanup commands remain admissible;
new prepares follow submitted cleanup in queue order. The neutral interleaving
fixtures cover this contract; RTSP has not yet been wired to use it.

Off-worker encoder probes have a separate, worker-acquired fence that combines
admission exclusion with a queue dispatch hold. It is acquired before the
preceding mutation completes, so even cleanup cannot start during the probe.
Independent holds compose and survive queue-generation changes; skipped head
commands still settle FIFO. Stop cancels queued work without waiting for an
external holder, which may safely outlive the queue/coordinator.

The neutral probe supervisor now runs a separately owned probe thread under the
recorded absolute setup deadline. An independent watchdog covers construction
and actual join acknowledgment, including thread-local teardown on success or
exception. Future readiness, even `set_value_at_thread_exit`, proved too early
on the Windows toolchain. Expired work never invokes the driver; admitted work
that exceeds the deadline invokes the fatal hook. Returning/throwing fake hooks
still join the driver and cannot convert timeout into success. Supervision on
the mutation worker is forbidden. A shared handoff transfers a fence exactly
once and rejects abandoned late publication, so stale results cannot strand
cleanup. These fixtures use no hardware. The process runtime, startup probe and
nonreturning fatal hook remain unwired; this is not a streaming claim.
Callers must still retain the fence through teardown and enqueue cleanup before
releasing admission.

The real launch helper now supports optional coordinated legacy preparation.
Configure, restore and fallback steps use one owner and its original deadline;
the worker publishes a probe fence before completion. Queued work owns its
request and result, including a separate shared VDD-recovery request, so HTTP
expiry cannot leave references to handler-local storage. Probes run on supervised
threads, recheck ownership at entry, and restore temporary encoder configuration
before rollback can execute. A failed stage queues abort before releasing its
fence. Successful preparation explicitly seals readiness for later ticket bind.
Cleanup eligibility remains supplied by the caller, preserving launch/resume's
different failure obligations. Invalid bundles are programming errors before
ownership adoption; their caller still owns the admission.

The actual-helper fake-platform target now exercises nine policy fixtures in
both direct and coordinated modes, plus seven lifetime/interleaving fixtures
(25 cases). SP1-SP14 were rechecked in both modes; SA1-SA12 exercise staged
ownership, threading, deadlines, sealing and cleanup. All deliberate faults
failed their intended cases, with failure messages inspected. The missing-fence
mutant is rejected by supervisor validation before probe entry; it is not
evidence that an unfenced graphics operation was allowed. These are software
checks, not hardware qualification. Production callers still omit the optional
adapter: enabling it also requires replacing their old restore guards and
completing runtime, ticket, raw-capability and shutdown wiring.

The lifetime layer now supplies a nonreturning explicit-exit-code termination
overload for the future fatal probe hook. It attempts native termination and,
if that returns, exits without cleanup; the existing no-argument shutdown path
preserves its selected code, including zero. Four isolated child-process tests
exercise the actual functions, including a test-only rejected native attempt,
with seven assertion-killed mutations. No Sunshine application entrypoint or
display/driver operation is executed by these children. This establishes the
termination seam, not production timeout routing or service-restart evidence;
Windows can still wait for outstanding kernel I/O during process termination.

The host VDD mutation entrypoint now accepts only closed create, destroy,
mode-list and hardware-cursor requests. All five existing send sites use these
types. Validation and serialization run before the private raw transport opens
the driver: missing requests, malformed/embedded-NUL GUIDs, nonfinite supplied
display fields, zero mode fields and excessive mode count/buffer length are
rejected explicitly. Bare creation retains its unused-parameter behavior.
Valid payloads retain the existing command syntax and result propagation;
numeric formatting is independent of the process locale. Mode order still
comes from the existing planner/formatter.

Seven actual-encoder fixtures use a link-time fake transport, with no native
driver implementation linked. Sixteen mutations fail the intended assertions,
including exact 2047-character acceptance and 2048-character rejection. This
closes the arbitrary-string API, not topology ownership: capability propagation
through VDD helpers/transport, cursor persistence and every other raw setter is
still pending. No new driver protocol or hardware operation was introduced.

The standalone Windows settings-policy target compiles the actual settings and
topology policy against fake display, session, audio and color-profile edges.
Nine fixtures cover nested topology rollback, explicit VDD-destroy suppression,
failed restoration retaining its baseline for retry, final restoration before
legacy persistence reset, and no-modification configuration cleanup. Sixteen
deliberate faults fail the intended assertions. Failure evidence checks the
requested topology and failed write as well as retained persistence; destruction
of a fake head alone must not masquerade as successful restoration.

Settings apply, revert and reset now require an explicit typed automatic-VDD
policy, with no permissive default or boolean overload. The same policy reaches
nested topology-switch rollback, no-modification cleanup and final legacy reset
restoration. Suppression prevents automatic head destruction, not topology or
field restoration; managed reset must still bypass legacy restoration completely.
Existing session callers explicitly retain their former allow/suppress choices.
The new suppression fixtures fail before propagation is added, including the
nested-error path, and the API fixture detects restored permissive defaults.
These are real-policy software checks, not proof that the future runtime selects
the restrictive policy, that raw setters are capability-gated, or that hardware
recovery works.

The ticket manager also supports a short binding check before publication or
replacement, under its mutex and after capacity checks. A refused or throwing
bind leaves the existing ticket untouched; concurrent claim/liveness readers
cannot observe the interval between binding and publication. Legacy calls omit
the hook and keep their existing behavior. Launch/resume wiring still needs to
pass the coordinator bind operation and roll back any request-owned application
on failure.

Ticket consumption has the corresponding pre-consumption activation hook.
It runs under the ticket lock before erasure; rejection or an exception preserves
the ticket and its claims. A real ticket-manager/coordinator interleaving fixture
demonstrates that consumption cannot look like abandonment before activation.
Production RTSP still needs to forward its full topology token through this hook.

Managed leases now retain independent, bounded owner/join references. Full
transaction/attachment tokens replace ID-only activation and broad lease-end
callbacks; activation requires a bound reference. Preparing joins have their
own deadlines, while published/consumed references cannot be removed by a late
HTTP abort. A terminal rollback may remove a bound reference only after its
ticket and claimants are revoked/quiescent. Remaining references protect the
original topology owner; restoration stays pending until the final reference
and video work are gone. A retiring lease refuses new joiners. Unlocked ticket
and idle probes revalidate reference stage, serial and activity generation.
Managed construction requires an explicit video-quiescence probe. These fake
interleaving fixtures do not yet establish production RTSP lifecycle wiring.

Final-reference teardown can hand off the actual native-restore receipt before
quiescence probes run, retaining the handle even if a probe throws. It remains
pending through deferred submission, restoration and observer processing;
shutdown reports an unsubmitted/skipped restore as stopped. A non-final stream
does not receive a receipt that would wait for other streams to end. Production
join watchdogs still need to retain and await these receipts.

Production `session_t`, launch/RTSP lifecycle, Windows mutation paths and the
administrator profile/status surface do not yet call the coordinator.
Coordinator routing is not enabled; invalid driver requests now fail before
transport while valid requests retain their protocol. G1 software evidence remains
incomplete, and its manual N=1 hardware check remains blocked on GH consent.
The user has authorized continuing safe software work after recording G1
evidence without waiting for synchronous review; this does not waive GH, GD,
GW or the requirement to report hardware-unverified milestones honestly.

User-confirmed P1 decisions:

- Every monitor-changing action is serialized through the one queue, not only
  stream starts. Rotation, scale, resolution, VDD create/destroy and restore
  wait their turn.
- The legacy "unknown configured output falls back to primary" behavior is
  preserved only for legacy scalar settings through an explicit compatibility
  adapter. Native multi-monitor profiles keep strict resolution.
- After a crash or interrupted change, recovery of the unfinished journaled
  change completes before any new stream start or display change is admitted.
  The approved legacy exception permits a locked-desktop launch to inherit/merge
  its pending legacy restore baseline; it does not permit managed admission
  before that legacy restoration completes.
- When a stream-start ticket expires or is cancelled, the monitor command tied
  to that ticket is cancelled by its own handle; unrelated queued work in the
  same generation continues.
