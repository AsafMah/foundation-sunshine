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
 * when its ticket is activated or disappears; a managed lease ends at stream end,
 * on abandonment (ticket gone without activation) or after the idle grace period,
 * and each end posts exactly one native restore.
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
  };

  using command_body_t = std::function<void(const mutation_capability_t &, const command_context_t &)>;

  /**
   * @brief Read-only probes. They are always called without the coordinator lock held
   *        and must not call back into the coordinator.
   */
  struct coordinator_probes_t {
    /** Whether launch ticket `id` is still registered (pending or claimed). */
    std::function<bool(std::uint32_t)> ticket_present;
    /** Number of live stream sessions. */
    std::function<std::size_t()> active_streams;
    /** Whether a legacy restore is still deferred (for example until unlock). */
    std::function<bool()> legacy_restore_pending;
  };

  struct coordinator_options_t {
    topology_command_queue_t::clock_function_t clock;
    bool managed_mode {};
    std::chrono::milliseconds poll_interval { 50 };
    std::chrono::milliseconds setup_timeout { 60000 };
    std::chrono::milliseconds native_idle_grace { 10000 };
    /** Interval of the internal tick thread; zero disables it (tests call `tick()`). */
    std::chrono::milliseconds tick_interval { 0 };
    coordinator_probes_t probes;
    /**
     * Body posted exactly once whenever a managed lease ends. Required in managed mode.
     * If it throws, native health becomes In conflict before any later command runs.
     */
    command_body_t native_restore;
    /** Observer for posted (fire-and-forget) commands whose body ran or failed. */
    std::function<void(command_kind_e, const command_result_t &)> posted_observer;
  };

  class coordinator_t {
  public:
    /**
     * @throws std::invalid_argument in managed mode without a `native_restore` body.
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
     * @brief Submit a command and wait for it.
     * @throws std::invalid_argument for `startup_recovery` and `launch_prepare`, which have
     *         dedicated entry points.
     */
    command_result_t
    run(command_kind_e kind, command_body_t body);

    /**
     * @brief Submit a command without waiting. Allowed from the worker thread.
     * @return `none` when accepted, otherwise the eager rejection reason.
     * @throws std::invalid_argument for `startup_recovery` and `launch_prepare`.
     */
    reject_reason_e
    post(command_kind_e kind, command_body_t body);

    launch_admission_t
    admit_launch(const launch_request_t &request);

    /**
     * @brief Run the launch's prepare body with the setup deadline recorded at admission.
     * @details On deadline the route stops waiting: a command that has not started is
     *          abandoned (`expired_before_start`, its body never runs); a running command
     *          is cancelled (`expired_while_running`) and keeps running until it observes
     *          `may_continue()`, so cleanup posted by `abort_launch` runs after it.
     */
    command_result_t
    run_launch_prepare(const launch_token_t &token, command_body_t body);

    /**
     * @brief Mark the launch's ticket as registered. Called once the handler succeeded.
     * @return false unless `token` is the current preparing launch and its prepare completed.
     */
    bool
    bind_launch(const launch_token_t &token);

    /**
     * @brief Release a preparing launch and post its cleanup atomically.
     * @details Cancels the launch's own prepare command if it is still queued or running;
     *          the cleanup runs after it. A managed lease always posts the native restore
     *          (a no-op when nothing was applied). A legacy lease posts `legacy_cleanup`
     *          when it is set.
     * @return false (and nothing posted) unless `token` is the current preparing launch.
     */
    bool
    abort_launch(const launch_token_t &token, command_body_t legacy_cleanup);

    /**
     * @brief The stream for launch `launch_session_id` connected (ticket consumed).
     */
    void
    launch_activated(std::uint32_t launch_session_id);

    /**
     * @brief End the active managed lease and post the native restore as `kind`.
     * @details Only an active lease (its stream connected) ends here. A bound lease belongs
     *          to a launch whose stream has not connected yet, possibly a newer launch than
     *          the stream that ended; it ends through ticket abandonment or the idle grace.
     * @param launch_session_id When set, only the lease of that launch is ended, so a late
     *        end event for an earlier launch cannot end a newer one.
     * @return true when this call ended the lease.
     * @throws std::invalid_argument when `kind` may not perform a native restore.
     */
    bool
    end_managed_lease(command_kind_e kind, std::optional<std::uint32_t> launch_session_id = std::nullopt);

    /**
     * @brief Re-check lease liveness (abandoned tickets, idle managed streams).
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
     * @brief Reject non-cleanup commands until `settled()` returns true.
     * @details `settled` is evaluated under the coordinator lock; it must not block.
     */
    void
    install_fence(std::function<bool()> settled);

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

    struct lease_t {
      std::uint64_t transaction {};
      std::uint32_t launch_session_id {};
      launch_flavor_e flavor { launch_flavor_e::legacy };
      lease_stage_e stage { lease_stage_e::preparing };
      std::string client_key;
      monotonic_time_t setup_deadline {};
      bool prepare_submitted {};
      std::optional<command_handle_t> prepare_handle;
      bool prepared {};
      bool activated_before_bind {};
      std::optional<monotonic_time_t> idle_since;
    };

    using submission_t = command_submission_t;

    reject_reason_e
    evaluate_locked(command_kind_e kind, std::optional<std::uint64_t> transaction, bool legacy_restore_pending);

    reject_reason_e
    admission_reason_locked();

    bool
    fence_active_locked();

    topology_command_queue_t::task_t
    make_task(command_kind_e kind, std::shared_ptr<command_state_t> state, command_body_t body,
      std::optional<std::uint64_t> transaction, bool posted);

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

    void
    submit_posted_locked(command_kind_e kind, command_body_t body);

    void
    release_lease_locked(bool post_native_restore, command_kind_e restore_kind);

    /** `options_.native_restore`, marking native health In conflict if it throws. */
    command_body_t
    guarded_native_restore();

    void
    notify_posted(command_kind_e kind, const command_result_t &result) const;

    void
    finish_recovery(bool succeeded);

    void
    complete_startup(command_result_t result);

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
    std::unique_ptr<topology_command_queue_t> queue_;
    std::uint64_t queue_generation_ {};
    std::optional<lease_t> lease_;
    std::function<bool()> fence_;
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
