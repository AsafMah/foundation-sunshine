/**
 * @file src/display_device/topology_coordinator.h
 * @brief Single serialized owner of every monitor-changing action.
 *
 * The coordinator wraps one `topology_command_queue_t` worker. Every route that
 * may change the host's monitors submits a command of a fixed `command_kind_e`;
 * the coordinator evaluates its admission policy eagerly at submit (fast reject,
 * zero platform calls) and again on the worker immediately before the body runs.
 *
 * Launch ownership ("the lease") is tracked per launch transaction:
 *   preparing -> bound (ticket registered) -> active (managed only, stream connected).
 * While any launch is preparing every other launch is rejected. A legacy lease ends
 * when its ticket is activated or disappears. A managed lease retains independent
 * owner/join references until terminal teardown or ticket abandonment; restoration
 * waits for all references and video work, or the all-active idle grace period.
 */
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "topology_capability.h"
#include "topology_command_queue.h"

namespace display_device::topology {

  enum class coordinator_phase_e : std::uint8_t {
    not_started,
    recovering,
    running,
    draining,
    stopped,
  };

  enum class reject_reason_e : std::uint8_t {
    none,
    not_started,
    recovering,
    draining,
    stopped,
    owner_busy,
    managed_session,
    recovery_required,
    legacy_restore_pending,
    fence_active,
    stale_transaction,
  };

  enum class outcome_e : std::uint8_t {
    completed,
    completed_late,
    rejected,
    expired_before_start,
    expired_while_running,
    cancelled,
    stopped,
    reentrant,
    failed,
  };

  enum class launch_flavor_e : std::uint8_t {
    legacy,
    managed,
  };

  enum class lease_stage_e : std::uint8_t {
    preparing,
    bound,
    active,
  };

  std::string_view
  to_string(coordinator_phase_e phase) noexcept;

  std::string_view
  to_string(reject_reason_e reason) noexcept;

  std::string_view
  to_string(outcome_e outcome) noexcept;

  std::string_view
  to_string(lease_stage_e stage) noexcept;

  struct command_result_t {
    outcome_e outcome { outcome_e::completed };
    reject_reason_e reason { reject_reason_e::none };
    std::exception_ptr exception;

    bool
    completed() const noexcept {
      return outcome == outcome_e::completed;
    }

    /**
     * @brief Whether the body started (and so may have changed host state).
     */
    bool
    body_started() const noexcept {
      return outcome == outcome_e::completed || outcome == outcome_e::completed_late ||
             outcome == outcome_e::expired_while_running || outcome == outcome_e::failed;
    }
  };

  struct posted_command_t {
    command_handle_t handle;
    reject_reason_e reason { reject_reason_e::none };
    std::shared_future<command_result_t> completion;
  };

  struct launch_request_t {
    std::uint32_t launch_session_id {};
    /** Stable client identity; empty means "never the same client". */
    std::string client_key;
  };

  struct launch_token_t {
    std::uint64_t transaction {};
    std::uint32_t launch_session_id {};
    launch_flavor_e flavor { launch_flavor_e::legacy };
    /** Informational: the coordinator enforces the deadline it recorded at admission. */
    monotonic_time_t setup_deadline {};
    /** Zero identifies the topology owner; nonzero identifies a managed join reference. */
    std::uint64_t attachment_serial {};
  };

  /** Display a managed lease prepared; every reference captures exactly this one. */
  struct managed_target_t {
    /** OS display id accepted by capture (CCD device id). */
    std::string display;
    /** Whether the target is a virtual head (vsync override applies). */
    bool virtual_head {};

    friend bool
    operator==(const managed_target_t &, const managed_target_t &) = default;
  };

  /** Result of a managed launch preparation: `display` is set exactly when it succeeded. */
  struct native_prepared_t {
    std::optional<managed_target_t> display;
    /** Why there is no display. */
    std::string reason;
  };

  struct launch_admission_t {
    std::optional<launch_token_t> token;
    reject_reason_e reason { reject_reason_e::none };

    explicit
    operator bool() const noexcept {
      return token.has_value();
    }
  };

  struct lease_snapshot_t {
    std::uint64_t transaction {};
    std::uint32_t launch_session_id {};
    launch_flavor_e flavor { launch_flavor_e::legacy };
    lease_stage_e stage { lease_stage_e::preparing };
    std::size_t attachments {};
    bool retiring {};
  };

  struct native_health_t {
    bool journal_present {};
    bool in_conflict {};
    std::string reason;
  };

  struct coordinator_status_t {
    coordinator_phase_e phase { coordinator_phase_e::not_started };
    bool managed_mode {};
    std::optional<lease_snapshot_t> lease;
    native_health_t native;
    bool fence_active {};
    std::uint64_t native_restores_posted {};
    /** Commands handed to the worker queue so far (including startup recovery). */
    std::uint64_t submitted_commands {};
    bool startup_restore_pending {};
    /** Startup is suspended behind an unsettled external actor. */
    bool external_recovery_pending {};
  };

  using command_body_t = std::function<void(const mutation_capability_t &, const command_context_t &)>;

  /** Shared lifetime token: no coordinator access is needed when the last holder releases it. */
  class admission_fence_t {
  public:
    ~admission_fence_t() = default;
    admission_fence_t(const admission_fence_t &) = delete;
    admission_fence_t &
    operator=(const admission_fence_t &) = delete;

  private:
    friend class coordinator_t;
    admission_fence_t() = default;
  };

  /** Excludes queued work and current-body raw mutations until an external actor settles. */
  class external_mutation_fence_t {
  public:
    ~external_mutation_fence_t();
    external_mutation_fence_t(const external_mutation_fence_t &) = delete;
    external_mutation_fence_t &
    operator=(const external_mutation_fence_t &) = delete;

  private:
    friend class coordinator_t;
    external_mutation_fence_t(std::shared_ptr<std::atomic_bool> blocked, std::shared_ptr<const dispatch_hold_t> dispatch);
    std::shared_ptr<std::atomic_bool> blocked_;
    std::shared_ptr<const dispatch_hold_t> dispatch_;
    bool armed_ {};
  };

  /**
   * Monitor-side handle of a startup suspended behind an external actor. The
   * coordinator owns the exclusion and continuation; dropping every handle never
   * settles, so unresolved or uninspectable actors stay excluded until stop.
   */
  class external_recovery_t {
  public:
    ~external_recovery_t() = default;
    external_recovery_t(const external_recovery_t &) = delete;
    external_recovery_t &
    operator=(const external_recovery_t &) = delete;

    /**
     * The actor has settled: resume startup ahead of every queued command.
     * Idempotent and callable from any thread, also after stop or destruction
     * of the coordinator, where it does nothing. Never call it while holding a
     * lock that a coordinator probe, observer or worker body may need.
     */
    void
    settle();

    /**
     * Whether startup still waits for this settlement: false once settled and
     * resumed, or once the coordinator is stopping or destroyed. A watcher stops
     * polling when this turns false. Same locking rule as settle().
     */
    bool
    waiting();

  private:
    friend class coordinator_t;
    friend class external_cleanup_t;
    struct link_t {
      std::mutex mutex;
      coordinator_t *owner {};
    };
    explicit external_recovery_t(std::shared_ptr<link_t> link):
        link_ { std::move(link) } {}
    std::shared_ptr<link_t> link_;
  };

  /** Returns true only once the obligation is durably discharged. */
  using external_cleanup_body_t = std::function<bool(const mutation_capability_t &, const command_context_t &)>;

  /**
   * Cleanup obligation of one external actor launched from a worker command.
   * The coordinator keeps a strong reference, so dropping every handle never
   * releases the exclusion: until `queue_first` succeeds, dispatch stays held
   * until stop. Non-cleanup admission stays fenced until a queued cleanup
   * returns true; failure, cancellation or stop leave it fenced.
   */
  class external_cleanup_t {
  public:
    ~external_cleanup_t() = default;
    external_cleanup_t(const external_cleanup_t &) = delete;
    external_cleanup_t &
    operator=(const external_cleanup_t &) = delete;

    /** A copy for the actor's supervisor. Dispatch stays held while any copy lives. */
    std::shared_ptr<void>
    exclusion() const;

    /**
     * Queue `cleanup` ahead of every pending command while dispatch is still held,
     * then release this handle's exclusion. Once, from any thread, also after the
     * coordinator is gone. Refused (keeping the exclusion) when `cleanup` is empty,
     * already queued, or the coordinator is stopping or gone. Never call it while
     * holding a lock that a coordinator probe, observer or worker body may need.
     */
    bool
    queue_first(external_cleanup_body_t cleanup);

    bool
    discharged() const noexcept;

  private:
    friend class coordinator_t;
    explicit external_cleanup_t(std::shared_ptr<external_recovery_t::link_t> link):
        link_ { std::move(link) } {}
    std::shared_ptr<external_recovery_t::link_t> link_;
    /** Read by the admission fence under the coordinator lock. */
    std::shared_ptr<std::atomic_bool> discharged_ { std::make_shared<std::atomic_bool>(false) };
    /** Guarded by the link mutex. */
    std::shared_ptr<const external_mutation_fence_t> exclusion_;
    bool queued_ {};
  };

  /** Shared admission and dispatch exclusion for a separately supervised encoder probe. */
  class probe_fence_t {
  public:
    ~probe_fence_t() = default;
    probe_fence_t(const probe_fence_t &) = delete;
    probe_fence_t &
    operator=(const probe_fence_t &) = delete;

    bool
    on_worker_thread() const noexcept;

  private:
    friend class coordinator_t;
    probe_fence_t() = default;
    const std::thread::id worker_ { std::this_thread::get_id() };
    std::shared_ptr<const dispatch_hold_t> dispatch_;
  };

  /**
   * @brief Read-only probes. They are always called without the coordinator lock held
   *        and must not call back into the coordinator.
   */
  struct coordinator_probes_t {
    /** Whether launch ticket `id` is still registered (pending or claimed). */
    std::function<bool(std::uint32_t)> ticket_present;
    /** Number of live video sessions; required in managed mode. */
    std::function<std::size_t()> active_streams;
    /**
     * Whether the launched application still runs (managed only, optional). A running
     * application holds an idle managed lease, like a paused legacy stream keeps its
     * display, and lets a resume join it; its exit starts the release.
     */
    std::function<bool()> app_running;
    /** Whether a legacy restore is still deferred (for example until unlock). */
    std::function<bool()> legacy_restore_pending;
    /**
     * Whether NVHTTP preparation, pending/claimed tickets and all startup/active
     * sessions are quiescent. Required when registering a startup continuation;
     * video count alone is insufficient. Queried on the worker under admission
     * exclusion, but without the coordinator mutex. Teardown must remain fenced.
     */
    std::function<bool()> startup_restore_quiescent;
  };

  struct coordinator_options_t {
    topology_command_queue_t::clock_function_t clock;
    bool managed_mode {};
    std::chrono::milliseconds poll_interval { 50 };
    std::chrono::milliseconds setup_timeout { 60000 };
    std::chrono::milliseconds native_idle_grace { 10000 };
    std::size_t max_managed_attachments { 16 };
    /** Interval of the internal tick thread; zero disables it (tests call `tick()`). */
    std::chrono::milliseconds tick_interval { 0 };
    coordinator_probes_t probes;
    /**
     * Body posted exactly once whenever a managed lease ends. Required in managed mode.
     * If it throws, native health becomes In conflict before any later command runs.
     */
    command_body_t native_restore;
    /** Observer for every accepted posted command, including cancellation before start. */
    std::function<void(command_kind_e, const command_result_t &)> posted_observer;
  };

  class coordinator_t {
    friend class external_recovery_t;
    friend class external_cleanup_t;

  public:
    /**
     * @throws std::invalid_argument in managed mode without a `native_restore` body,
     *         a video-quiescence probe, or capacity for the owner reference.
     */
    explicit coordinator_t(coordinator_options_t options = {});
    ~coordinator_t();

    coordinator_t(const coordinator_t &) = delete;
    coordinator_t &
    operator=(const coordinator_t &) = delete;

    /**
     * @brief Start the worker and submit startup recovery as the first command.
     * @details Recovery runs even if `begin_draining()` is called before it starts, so
     *          shutdown cleanup always runs after it.
     * @return The recovery result (`stopped` if `stop()` came first); every later command
     *         runs after it.
     */
    std::shared_future<command_result_t>
    start(command_body_t recovery);

    /**
     * Register one immutable legacy restore continuation during initial startup.
     * It owns all captured state until restored, explicitly forgotten, or stopped.
     * Normal return means fully restored; an exception retains the obligation.
     * Only the initial startup worker/capability can register it, once.
     */
    void
    defer_startup_restore(const mutation_capability_t &capability, command_body_t restore);

    /**
     * Retry only the registered continuation, with narrowly scoped authority.
     * An in-flight attempt is coalesced. Pending restore blocks managed admission,
     * not the approved legacy merge; an executing attempt fences all admission
     * through its posted observer. Never wait for its receipt on the worker.
     */
    posted_command_t
    retry_startup_restore();

    /** Explicit reset only: forget the obligation without performing restoration. */
    void
    forget_startup_restore(const mutation_capability_t &capability);

    /**
     * @brief Submit a command and wait for it.
     * @throws std::invalid_argument for `startup_recovery`, `legacy_startup_restore`
     *         and `launch_prepare`, which have dedicated entry points.
     */
    command_result_t
    run(command_kind_e kind, command_body_t body);

    /**
     * @brief Submit a command without waiting. Allowed from the worker thread.
     * @return `none` when accepted, otherwise the eager rejection reason.
     * @throws std::invalid_argument for kinds with dedicated entry points.
     */
    reject_reason_e
    post(command_kind_e kind, command_body_t body);

    /**
     * @brief Post with a per-command receipt, for restore watchdogs and retry generations.
     * @details The future completes on every terminal path, including eager rejection,
     *          worker rejection and shutdown cancellation. No waiter thread is created.
     *          Never wait on it from the worker. Observer exceptions are reported as
     *          failed results rather than leaving the receipt unresolved.
     */
    posted_command_t
    post_tracked(command_kind_e kind, command_body_t body);

    launch_admission_t
    admit_launch(const launch_request_t &request);

    /** Reserve a reference to an existing managed topology without preparing it again. */
    launch_admission_t
    admit_managed_join(const launch_request_t &request);

    /**
     * Record the display a fully prepared managed owner will stream, once, before bind.
     * Returns false for joiners, legacy, unprepared or non-preparing leases, or empty displays.
     */
    bool
    record_managed_display(const launch_token_t &token, managed_target_t display);

    /** The lease's recorded display, for any live reference of the current managed lease. */
    std::optional<managed_target_t>
    managed_display(const launch_token_t &token);

    /** Consume a bound reference, under the ticket lock before removing its ticket. */
    bool
    launch_activated(const launch_token_t &token);

    /**
     * End a bound/active managed reference after ticket revocation and quiescence of
     * its claimants, startup and streaming work. Preparing references use abort_launch.
     * Returns whether the reference ended, not whether restoration has completed.
     * The restore obligation survives remaining references and live-video probes.
     * When supplied, restoration is cleared, then filled for the final reference
     * before probing or queue submission (both may throw). It resolves with the
     * actual restore/observer outcome or stopped, never merely with submission.
     * A caller retaining it across an exception may retry tick, not this token.
     */
    bool
    end_managed_launch(const launch_token_t &token, command_kind_e kind = command_kind_e::stream_restore,
      std::shared_future<command_result_t> *restoration = nullptr);

    /**
     * @brief Run the launch's prepare body with the setup deadline recorded at admission.
     * @details On deadline the route stops waiting: a command that has not started is
     *          abandoned (`expired_before_start`, its body never runs); a running command
     *          is cancelled (`expired_while_running`) and keeps running until it observes
     *          `may_continue()`, so cleanup posted by `abort_launch` runs after it.
     * @param legacy_expiry_cleanup Cleanup used if tick/admission/bind expires this
     *        transaction before its handler aborts it. Invoked only if the prepare body
     *        started. Capture shared state, not handler stack references; the body may
     *        outlive the handler. Managed launches always use the native restore.
     */
    command_result_t
    run_launch_prepare(const launch_token_t &token, command_body_t body, command_body_t legacy_expiry_cleanup = {});

    /**
     * Run one mutation in a staged setup, using the original deadline and owner token.
     * Steps must complete successfully and sequentially; they cannot mix with one-shot
     * preparation or resume after sealing. Off-worker probes hold a probe fence between
     * steps. Supply legacy cleanup only on the first step; it remains the rollback
     * obligation for abort/expiry if any step started, even when a later step did not.
     * @throws std::invalid_argument if a continuation supplies replacement cleanup.
     */
    command_result_t
    run_launch_step(const launch_token_t &token, command_body_t body, command_body_t legacy_expiry_cleanup = {});

    /** Seal successful staged preparation after probes end, before atomic ticket binding. */
    bool
    finish_launch_prepare(const launch_token_t &token);

    /**
     * Lift the setup deadline of a fully prepared, still-preparing launch whose owner
     * guarantees bind or abort (legacy parity: app start had no deadline; the ticket
     * handshake window governs after bind). Returns false for stale, unprepared,
     * expired or non-preparing leases.
     */
    bool
    hold_prepared_launch(const launch_token_t &token);

    /** Recorded absolute monotonic deadline for a live preparing owner; reaps expiry. */
    std::optional<monotonic_time_t>
    launch_setup_deadline(const launch_token_t &token);

    /**
     * @brief Mark the launch's ticket as registered. Called once the handler succeeded.
     * @return false unless `token` identifies a preparing reference, its recorded deadline
     *         has not expired, and the coordinator is running. An owner also needs a
     *         completed prepare; joiners share the already-prepared topology.
     */
    bool
    bind_launch(const launch_token_t &token);

    /**
     * @brief Release a preparing launch and post its cleanup atomically.
     * @details Cancels the launch's own prepare command if it is still queued or running;
     *          the cleanup runs after it. A one-shot managed lease always posts the native
     *          restore (a no-op when nothing was applied). A legacy lease posts `legacy_cleanup`
     *          when it is set.
     *          Staged setup instead uses its first step's registered cleanup, and only
     *          restores if at least one step started; pass an empty legacy_cleanup.
     * @return false (and nothing posted) unless `token` is the current preparing launch.
     * @throws std::invalid_argument for replacement cleanup on a staged transaction.
     */
    bool
    abort_launch(const launch_token_t &token, command_body_t legacy_cleanup);

    /**
     * @brief Re-check lease liveness (setup deadline, abandoned tickets, idle managed streams).
     */
    void
    tick();

    /**
     * @brief Enter shutdown: reject new non-cleanup work and cancel the preparing launch.
     */
    void
    begin_draining();

    /**
     * @brief Stop the worker. Pending commands complete as `stopped`.
     * @details Concurrent calls are serialized: every call returns only after the worker
     *          and tick threads have finished.
     * @throws std::logic_error when called from the worker or tick thread.
     */
    void
    stop();

    /**
     * @brief Add a fence rejecting non-cleanup commands until `settled()` returns true.
     * @details Fences are independent. `settled` is evaluated under the coordinator lock;
     *          it must not block. An empty predicate is invalid.
     */
    void
    install_fence(std::function<bool()> settled);

    /**
     * Acquire before launching an adapter restart; retain until it actually settles.
     * Requires this coordinator's worker capability with adapter-restart permission.
     * Unlike ordinary fences, this blocks cleanup dispatch and later mutations in
     * the current command. Failed acquisition leaves existing exclusion untouched.
     */
    std::shared_ptr<const external_mutation_fence_t>
    hold_external_mutation(const mutation_capability_t &capability);

    /**
     * As hold_external_mutation, for an actor whose cleanup this coordinator must
     * own: acquire before launching it and give `exclusion()` to its supervisor.
     * Also fences non-cleanup admission until the cleanup is discharged. Only one
     * undischarged obligation may exist; another throws std::logic_error.
     */
    std::shared_ptr<external_cleanup_t>
    begin_external_cleanup(const mutation_capability_t &capability);

    /**
     * Adopt an existing or possibly existing actor during startup recovery.
     * Requires this worker's exact startup kind and native-recovery permission;
     * it never grants permission to launch another actor. Adoption may occur
     * while draining or after stop moved the queue but is still joining startup.
     * Retain through settlement; durable recovery and cleanup belong to the caller.
     */
    std::shared_ptr<const external_mutation_fence_t>
    hold_external_recovery(const mutation_capability_t &capability);

    /**
     * Suspend startup behind an existing or possibly existing external actor.
     * Call before any fallible inspection or monitor publication. The coordinator
     * itself retains the exclusion: the current startup body loses raw authority,
     * and neither its return nor an exception ends startup or permits legacy
     * operation. After `settle()`, and once the body returned, `continuation` runs
     * with exact startup authority ahead of all queued work (including shutdown
     * cleanup queued while draining); its result and any earlier startup failure
     * complete the `start()` future. Same requirements as hold_external_recovery;
     * once per coordinator. Never grants permission to launch another actor.
     */
    std::shared_ptr<external_recovery_t>
    suspend_startup_for_external_recovery(const mutation_capability_t &capability, command_body_t continuation);

    /**
     * @brief Hold an admission fence until every copy of the returned token is released.
     * @details For teardown, acquire before removing a session from the visible registry;
     *          keep it through join and submission of all stream/app cleanup. Cleanup may
     *          execute while fenced; subsequent prepares are ordered behind it by the queue.
     */
    std::shared_ptr<const admission_fence_t>
    hold_fence();

    /**
     * Acquire on the worker before finishing the mutation preceding an off-worker probe.
     * Unlike a teardown fence, this also queues cleanup without executing it. The probe
     * owns a copy through encoder teardown; cancellation must not release its last copy.
     * Enqueue required cleanup before releasing admission, and never wait for queued
     * work while holding this fence. Stop cancels queued work without waiting for it.
     * This guards topology only; callers still own probe supervision and resource lifetime.
     * @throws std::logic_error off this worker or after draining/stop begins.
     */
    std::shared_ptr<const probe_fence_t>
    hold_probe_fence();

    void
    publish_native_health(native_health_t health);

    coordinator_status_t
    status() const;

    bool
    managed_mode() const noexcept;

    bool
    on_worker_thread() const noexcept;

  private:
    struct command_state_t;
    struct startup_signal_t;
    struct startup_restore_t;

    enum class prepare_mode_e {
      not_submitted,
      one_shot,
      staged,
    };

    struct attachment_t {
      std::uint64_t serial {};
      std::uint32_t launch_session_id {};
      lease_stage_e stage { lease_stage_e::preparing };
      monotonic_time_t setup_deadline {};
    };

    struct lease_t {
      std::uint64_t transaction {};
      std::uint32_t launch_session_id {};
      launch_flavor_e flavor { launch_flavor_e::legacy };
      lease_stage_e stage { lease_stage_e::preparing };
      std::string client_key;
      monotonic_time_t setup_deadline {};
      prepare_mode_e prepare_mode { prepare_mode_e::not_submitted };
      bool step_completed {};
      bool prepare_started {};
      std::optional<command_handle_t> prepare_handle;
      std::shared_ptr<command_state_t> prepare_state;
      std::shared_ptr<command_state_t> restore_state;
      command_body_t legacy_expiry_cleanup;
      bool prepared {};
      std::optional<monotonic_time_t> idle_since;
      std::vector<attachment_t> attachments;
      std::uint64_t next_attachment_serial { 1 };
      std::uint64_t activity_generation {};
      std::optional<command_kind_e> restore_kind;
      std::optional<managed_target_t> display;
      /** Last tick saw no references while the application ran: resume may join. */
      bool app_held {};
    };

    using submission_t = command_submission_t;

    command_result_t
    run_prepare(const launch_token_t &token, command_body_t body, command_body_t legacy_expiry_cleanup, prepare_mode_e mode);

    reject_reason_e
    evaluate_locked(command_kind_e kind, std::optional<std::uint64_t> transaction, bool legacy_restore_pending);

    reject_reason_e
    admission_reason_locked();

    bool
    fence_active_locked();

    bool
    external_fence_active_locked();

    std::shared_ptr<const external_mutation_fence_t>
    hold_external_actor(const mutation_capability_t &capability, bool recovering);

    topology_command_queue_t::task_t
    make_task(command_kind_e kind, std::shared_ptr<command_state_t> state, command_body_t body,
      std::optional<std::uint64_t> transaction);

    command_result_t
    wait(submission_t &submission, const std::shared_ptr<command_state_t> &state, monotonic_time_t deadline);

    static command_result_t
    map_completion(const command_completion_t &completion, const command_state_t &state, bool stopping);

    void
    cancel_handle(const command_handle_t &handle);

    /** Submit to the worker queue; caller holds `mutex_` and has checked `queue_`. */
    submission_t
    submit_locked(command_kind_e kind, const std::shared_ptr<command_state_t> &state, command_body_t body,
      std::optional<std::uint64_t> transaction, monotonic_time_t deadline, bool posted);

    posted_command_t
    submit_posted_locked(command_kind_e kind, command_body_t body, std::shared_ptr<command_state_t> state = {});

    void
    release_lease_locked(bool post_native_restore, command_kind_e restore_kind);

    /** Cancel and reap an expired preparing lease, enqueuing cleanup before admission. */
    bool
    expire_preparing_locked();

    attachment_t *
    find_attachment_locked(const launch_token_t &token);

    bool
    matches_owner_locked(const launch_token_t &token) const;

    void
    touch_managed_locked();

    void
    remove_attachment_locked(std::uint64_t serial, command_kind_e restore_kind);

    void
    expire_joins_locked();

    /** `options_.native_restore`, marking native health In conflict if it throws. */
    command_body_t
    guarded_native_restore();

    void
    notify_posted(command_kind_e kind, const command_result_t &result) const;

    void
    finish_recovery(bool succeeded);

    void
    complete_startup(command_result_t result);

    struct external_startup_t;

    /** From external_recovery_t::settle(); the link mutex is held, mutex_ is not. */
    void
    settle_external_startup();

    /** From external_recovery_t::waiting(); the link mutex is held, mutex_ is not. */
    bool
    external_startup_waiting();

    /**
     * Caller holds mutex_. Once settled and the suspended body returned, put the
     * continuation at the queue front while dispatch is still held, and hand back
     * the exclusion for release after unlocking.
     */
    std::shared_ptr<const external_mutation_fence_t>
    resume_external_startup_locked();

    /** From external_cleanup_t::queue_first(); the link mutex is held, mutex_ is not. */
    bool
    queue_external_cleanup(external_cleanup_t &cleanup, external_cleanup_body_t body);

    void
    tick_loop();

    monotonic_time_t
    now() const;

    coordinator_options_t options_;
    topology_command_queue_t::clock_function_t clock_;

    /** Serializes start() and stop(); never taken by the worker or tick thread. */
    std::mutex lifecycle_mutex_;
    std::shared_ptr<startup_signal_t> startup_;

    mutable std::mutex mutex_;
    coordinator_phase_e phase_ { coordinator_phase_e::not_started };
    bool stopping_ {};
    std::shared_ptr<startup_restore_t> startup_restore_;
    std::shared_ptr<startup_restore_t> startup_restore_inflight_;
    bool startup_restore_attempt_active_ {};
    std::unique_ptr<topology_command_queue_t> queue_;
    std::uint64_t queue_generation_ {};
    std::optional<lease_t> lease_;
    std::vector<std::function<bool()>> fences_;
    std::shared_ptr<std::atomic_bool> external_mutation_ { std::make_shared<std::atomic_bool>(false) };
    std::shared_ptr<external_startup_t> external_startup_;
    /** The undischarged obligation until its cleanup is queued; released at stop. */
    std::shared_ptr<external_cleanup_t> external_cleanup_;
    /** Discharge flag of the latest obligation, until it is discharged. */
    std::shared_ptr<std::atomic_bool> external_cleanup_discharged_;
    /** Shared with every external_recovery_t; cleared under its mutex before teardown begins. */
    std::shared_ptr<external_recovery_t::link_t> recovery_link_;
    native_health_t native_;
    std::uint64_t next_transaction_ { 1 };
    std::uint64_t native_restores_posted_ {};
    std::uint64_t submitted_commands_ {};

    std::mutex tick_mutex_;
    std::condition_variable tick_wake_;
    bool tick_stop_ {};
    std::thread tick_thread_;
  };

}  // namespace display_device::topology
