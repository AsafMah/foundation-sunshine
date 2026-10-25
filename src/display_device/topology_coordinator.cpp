/**
 * @file src/display_device/topology_coordinator.cpp
 * @brief Serialized topology coordinator: admission policy, launch lease and recovery-first start.
 */
#include "topology_coordinator.h"

#include <atomic>
#include <stdexcept>
#include <utility>

namespace display_device::topology {

  namespace {
    thread_local const coordinator_t *tls_running_coordinator = nullptr;
    thread_local const coordinator_t *tls_ticking_coordinator = nullptr;

    class running_scope_t {
    public:
      explicit running_scope_t(const coordinator_t *coordinator) noexcept:
          previous_ { tls_running_coordinator } {
        tls_running_coordinator = coordinator;
      }

      ~running_scope_t() {
        tls_running_coordinator = previous_;
      }

      running_scope_t(const running_scope_t &) = delete;
      running_scope_t &
      operator=(const running_scope_t &) = delete;

    private:
      const coordinator_t *previous_;
    };

    constexpr monotonic_time_t no_deadline = monotonic_time_t::max();

    monotonic_time_t
    steady_now() {
      return std::chrono::duration_cast<monotonic_time_t>(std::chrono::steady_clock::now().time_since_epoch());
    }

    command_result_t
    rejected(reject_reason_e reason) {
      return { outcome_e::rejected, reason, {} };
    }

    command_result_t
    outcome(outcome_e value) {
      return { value, reject_reason_e::none, {} };
    }

    void
    require_routable(command_kind_e kind) {
      if (kind == command_kind_e::startup_recovery || kind == command_kind_e::launch_prepare) {
        throw std::invalid_argument { std::string { "Topology command " } + std::string { to_string(kind) } +
                                      " has a dedicated coordinator entry point" };
      }
    }
  }  // namespace

  std::string_view
  to_string(coordinator_phase_e phase) noexcept {
    switch (phase) {
      case coordinator_phase_e::not_started:
        return "not_started";
      case coordinator_phase_e::recovering:
        return "recovering";
      case coordinator_phase_e::running:
        return "running";
      case coordinator_phase_e::draining:
        return "draining";
      case coordinator_phase_e::stopped:
        return "stopped";
    }
    return "unknown";
  }

  std::string_view
  to_string(reject_reason_e reason) noexcept {
    switch (reason) {
      case reject_reason_e::none:
        return "none";
      case reject_reason_e::not_started:
        return "not_started";
      case reject_reason_e::recovering:
        return "recovering";
      case reject_reason_e::draining:
        return "draining";
      case reject_reason_e::stopped:
        return "stopped";
      case reject_reason_e::owner_busy:
        return "owner_busy";
      case reject_reason_e::managed_session:
        return "managed_session";
      case reject_reason_e::recovery_required:
        return "recovery_required";
      case reject_reason_e::legacy_restore_pending:
        return "legacy_restore_pending";
      case reject_reason_e::fence_active:
        return "fence_active";
      case reject_reason_e::stale_transaction:
        return "stale_transaction";
    }
    return "unknown";
  }

  std::string_view
  to_string(outcome_e value) noexcept {
    switch (value) {
      case outcome_e::completed:
        return "completed";
      case outcome_e::completed_late:
        return "completed_late";
      case outcome_e::rejected:
        return "rejected";
      case outcome_e::expired_before_start:
        return "expired_before_start";
      case outcome_e::expired_while_running:
        return "expired_while_running";
      case outcome_e::cancelled:
        return "cancelled";
      case outcome_e::stopped:
        return "stopped";
      case outcome_e::reentrant:
        return "reentrant";
      case outcome_e::failed:
        return "failed";
    }
    return "unknown";
  }

  std::string_view
  to_string(lease_stage_e stage) noexcept {
    switch (stage) {
      case lease_stage_e::preparing:
        return "preparing";
      case lease_stage_e::bound:
        return "bound";
      case lease_stage_e::active:
        return "active";
    }
    return "unknown";
  }

  struct coordinator_t::command_state_t {
    enum class stage_e : std::uint8_t {
      pending,
      running,
      rejected,
      abandoned,
    };

    std::atomic<stage_e> stage { stage_e::pending };
    std::atomic<reject_reason_e> reason { reject_reason_e::none };
  };

  struct coordinator_t::startup_signal_t {
    std::once_flag once;
    std::promise<command_result_t> promise;
    std::shared_future<command_result_t> future { promise.get_future().share() };
  };

  coordinator_t::coordinator_t(coordinator_options_t options):
      options_ { std::move(options) },
      clock_ { options_.clock ? options_.clock : topology_command_queue_t::clock_function_t { &steady_now } } {
    if (options_.managed_mode && !options_.native_restore) {
      throw std::invalid_argument { "Managed topology mode needs a native restore body" };
    }
    if (options_.poll_interval <= std::chrono::milliseconds::zero()) {
      options_.poll_interval = std::chrono::milliseconds { 1 };
    }
  }

  coordinator_t::~coordinator_t() {
    stop();
  }

  monotonic_time_t
  coordinator_t::now() const {
    return clock_();
  }

  bool
  coordinator_t::managed_mode() const noexcept {
    return options_.managed_mode;
  }

  bool
  coordinator_t::on_worker_thread() const noexcept {
    return tls_running_coordinator == this;
  }

  std::shared_future<command_result_t>
  coordinator_t::start(command_body_t recovery) {
    const std::lock_guard lifecycle { lifecycle_mutex_ };
    {
      std::lock_guard lock { mutex_ };
      if (phase_ != coordinator_phase_e::not_started) {
        throw std::logic_error { "Topology coordinator can only be started once" };
      }
      startup_ = std::make_shared<startup_signal_t>();
      queue_ = std::make_unique<topology_command_queue_t>(clock_);
      queue_generation_ = queue_->generation();
      phase_ = coordinator_phase_e::recovering;
      // The recovery task reports through `startup_`; its queue completion is not needed.
      static_cast<void>(submit_locked(command_kind_e::startup_recovery, std::make_shared<command_state_t>(),
        std::move(recovery), std::nullopt, no_deadline, false));
    }

    if (options_.tick_interval > std::chrono::milliseconds::zero()) {
      tick_thread_ = std::thread { [this]() { tick_loop(); } };
    }
    return startup_->future;
  }

  reject_reason_e
  coordinator_t::evaluate_locked(command_kind_e kind, std::optional<std::uint64_t> transaction, bool legacy_restore_pending) {
    if (stopping_ || phase_ == coordinator_phase_e::stopped) {
      return reject_reason_e::stopped;
    }
    if (phase_ == coordinator_phase_e::not_started) {
      return reject_reason_e::not_started;
    }
    if (kind == command_kind_e::startup_recovery) {
      // Only start() submits it, once. It runs even if draining began first, so shutdown
      // cleanup (queued behind it) never runs before recovery.
      return reject_reason_e::none;
    }
    if (phase_ == coordinator_phase_e::recovering) {
      return reject_reason_e::recovering;
    }
    if (is_cleanup_kind(kind)) {
      if (kind == command_kind_e::shutdown_vdd_destroy && (native_.journal_present || native_.in_conflict)) {
        // A head the native journal may own is never destroyed automatically.
        return reject_reason_e::recovery_required;
      }
      return reject_reason_e::none;
    }
    if (phase_ == coordinator_phase_e::draining) {
      return reject_reason_e::draining;
    }
    if (fence_active_locked()) {
      return reject_reason_e::fence_active;
    }
    if (kind == command_kind_e::launch_prepare) {
      if (!lease_ || !transaction || lease_->transaction != *transaction || lease_->stage != lease_stage_e::preparing) {
        return reject_reason_e::stale_transaction;
      }
      if (options_.managed_mode) {
        if (native_.in_conflict) {
          return reject_reason_e::recovery_required;
        }
        if (legacy_restore_pending) {
          return reject_reason_e::legacy_restore_pending;
        }
      }
      return reject_reason_e::none;
    }
    if (lease_ && lease_->flavor == launch_flavor_e::managed) {
      return reject_reason_e::managed_session;
    }
    return reject_reason_e::none;
  }

  reject_reason_e
  coordinator_t::admission_reason_locked() {
    if (stopping_ || phase_ == coordinator_phase_e::stopped) {
      return reject_reason_e::stopped;
    }
    switch (phase_) {
      case coordinator_phase_e::not_started:
        return reject_reason_e::not_started;
      case coordinator_phase_e::recovering:
        return reject_reason_e::recovering;
      case coordinator_phase_e::draining:
        return reject_reason_e::draining;
      default:
        break;
    }
    return fence_active_locked() ? reject_reason_e::fence_active : reject_reason_e::none;
  }

  bool
  coordinator_t::fence_active_locked() {
    if (!fence_) {
      return false;
    }
    if (fence_()) {
      fence_ = nullptr;
      return false;
    }
    return true;
  }

  topology_command_queue_t::task_t
  coordinator_t::make_task(command_kind_e kind, std::shared_ptr<command_state_t> state, command_body_t body,
    std::optional<std::uint64_t> transaction, bool posted) {
    return [this, kind, state = std::move(state), body = std::move(body), transaction, posted](const command_context_t &context) {
      using stage_e = command_state_t::stage_e;
      const running_scope_t scope { this };

      bool legacy_pending = false;
      if (kind == command_kind_e::launch_prepare && options_.managed_mode && options_.probes.legacy_restore_pending) {
        legacy_pending = options_.probes.legacy_restore_pending();
      }

      reject_reason_e reason;
      {
        std::lock_guard lock { mutex_ };
        reason = evaluate_locked(kind, transaction, legacy_pending);
        auto expected = stage_e::pending;
        if (reason != reject_reason_e::none) {
          state->reason.store(reason, std::memory_order_release);
          if (!state->stage.compare_exchange_strong(expected, stage_e::rejected, std::memory_order_acq_rel)) {
            return;
          }
        }
        else if (!state->stage.compare_exchange_strong(expected, stage_e::running, std::memory_order_acq_rel)) {
          // The route stopped waiting before the command started; never run it.
          return;
        }
      }

      if (reason != reject_reason_e::none) {
        if (kind == command_kind_e::startup_recovery) {
          // Only a stop() racing the worker can reject startup recovery.
          complete_startup(outcome(outcome_e::stopped));
        }
        else if (posted) {
          notify_posted(kind, rejected(reason));
        }
        return;
      }

      const mutation_capability_t capability { kind, options_.managed_mode, std::this_thread::get_id() };
      if (!posted && kind != command_kind_e::startup_recovery) {
        if (body) {
          body(capability, context);
        }
        return;
      }

      command_result_t result;
      try {
        if (body) {
          body(capability, context);
        }
        result.outcome = context.may_continue() ? outcome_e::completed : outcome_e::completed_late;
      }
      catch (...) {
        result.outcome = outcome_e::failed;
        result.exception = std::current_exception();
      }
      if (kind == command_kind_e::startup_recovery) {
        finish_recovery(result.outcome != outcome_e::failed);
        complete_startup(std::move(result));
        return;
      }
      notify_posted(kind, result);
    };
  }

  void
  coordinator_t::finish_recovery(bool succeeded) {
    std::lock_guard lock { mutex_ };
    if (!succeeded && !native_.in_conflict) {
      native_.in_conflict = true;
      native_.reason = "Startup recovery failed";
    }
    if (phase_ == coordinator_phase_e::recovering) {
      phase_ = coordinator_phase_e::running;
    }
  }

  void
  coordinator_t::complete_startup(command_result_t result) {
    std::shared_ptr<startup_signal_t> signal;
    {
      std::lock_guard lock { mutex_ };
      signal = startup_;
    }
    if (!signal) {
      return;
    }
    std::call_once(signal->once, [&signal, &result]() {
      signal->promise.set_value(std::move(result));
    });
  }

  void
  coordinator_t::notify_posted(command_kind_e kind, const command_result_t &result) const {
    if (!options_.posted_observer) {
      return;
    }
    try {
      options_.posted_observer(kind, result);
    }
    catch (...) {
    }
  }

  command_result_t
  coordinator_t::map_completion(const command_completion_t &completion, const command_state_t &state, bool stopping) {
    using stage_e = command_state_t::stage_e;
    switch (state.stage.load(std::memory_order_acquire)) {
      case stage_e::rejected:
        return rejected(state.reason.load(std::memory_order_acquire));
      case stage_e::abandoned:
        return outcome(outcome_e::expired_before_start);
      default:
        break;
    }

    switch (completion.status) {
      case command_status_e::completed:
        return outcome(outcome_e::completed);
      case command_status_e::completed_after_cancellation:
      case command_status_e::completed_after_deadline:
        return outcome(outcome_e::completed_late);
      case command_status_e::cancelled_before_start:
        return outcome(stopping ? outcome_e::stopped : outcome_e::cancelled);
      case command_status_e::expired_before_start:
        return outcome(outcome_e::expired_before_start);
      case command_status_e::stale_generation:
        return outcome(outcome_e::cancelled);
      case command_status_e::queue_stopped:
        return outcome(outcome_e::stopped);
      case command_status_e::failed:
        return { outcome_e::failed, reject_reason_e::none, completion.exception };
    }
    return outcome(outcome_e::failed);
  }

  void
  coordinator_t::cancel_handle(const command_handle_t &handle) {
    std::lock_guard lock { mutex_ };
    if (queue_) {
      queue_->cancel(handle);
    }
  }

  command_result_t
  coordinator_t::wait(submission_t &submission, const std::shared_ptr<command_state_t> &state, monotonic_time_t deadline) {
    using stage_e = command_state_t::stage_e;
    if (deadline != no_deadline) {
      while (submission.completion.wait_for(options_.poll_interval) != std::future_status::ready) {
        if (now() < deadline) {
          continue;
        }
        auto expected = stage_e::pending;
        if (state->stage.compare_exchange_strong(expected, stage_e::abandoned, std::memory_order_acq_rel)) {
          cancel_handle(submission.handle);
          return outcome(outcome_e::expired_before_start);
        }
        if (expected == stage_e::running) {
          // Cancellation is cooperative: the body keeps the worker until it returns,
          // so anything posted after this (for example launch cleanup) runs after it.
          cancel_handle(submission.handle);
          return outcome(outcome_e::expired_while_running);
        }
        break;
      }
    }

    const auto completion = submission.completion.get();
    bool stopping;
    {
      std::lock_guard lock { mutex_ };
      stopping = stopping_;
    }
    return map_completion(completion, *state, stopping);
  }

  command_result_t
  coordinator_t::run(command_kind_e kind, command_body_t body) {
    require_routable(kind);
    if (on_worker_thread()) {
      return outcome(outcome_e::reentrant);
    }

    auto state = std::make_shared<command_state_t>();
    submission_t submission;
    {
      std::lock_guard lock { mutex_ };
      if (const auto reason = evaluate_locked(kind, std::nullopt, false); reason != reject_reason_e::none) {
        return rejected(reason);
      }
      submission = submit_locked(kind, state, std::move(body), std::nullopt, no_deadline, false);
    }
    return wait(submission, state, no_deadline);
  }

  reject_reason_e
  coordinator_t::post(command_kind_e kind, command_body_t body) {
    require_routable(kind);
    std::lock_guard lock { mutex_ };
    if (const auto reason = evaluate_locked(kind, std::nullopt, false); reason != reject_reason_e::none) {
      return reason;
    }
    submit_posted_locked(kind, std::move(body));
    return reject_reason_e::none;
  }

  coordinator_t::submission_t
  coordinator_t::submit_locked(command_kind_e kind, const std::shared_ptr<command_state_t> &state, command_body_t body,
    std::optional<std::uint64_t> transaction, monotonic_time_t deadline, bool posted) {
    ++submitted_commands_;
    return queue_->submit(queue_generation_, deadline, make_task(kind, state, std::move(body), transaction, posted));
  }

  void
  coordinator_t::submit_posted_locked(command_kind_e kind, command_body_t body) {
    if (!queue_) {
      return;
    }
    // Posted commands have no waiter; the completion future is intentionally dropped.
    auto submission = submit_locked(kind, std::make_shared<command_state_t>(), std::move(body), std::nullopt, no_deadline, true);
    static_cast<void>(submission);
  }

  void
  coordinator_t::release_lease_locked(bool post_native_restore, command_kind_e restore_kind) {
    lease_.reset();
    if (post_native_restore && options_.native_restore && queue_) {
      submit_posted_locked(restore_kind, guarded_native_restore());
      ++native_restores_posted_;
    }
  }

  command_body_t
  coordinator_t::guarded_native_restore() {
    return [this](const mutation_capability_t &capability, const command_context_t &context) {
      try {
        options_.native_restore(capability, context);
      }
      catch (...) {
        {
          std::lock_guard lock { mutex_ };
          native_.in_conflict = true;
          native_.reason = "Native restore failed";
        }
        throw;
      }
    };
  }

  launch_admission_t
  coordinator_t::admit_launch(const launch_request_t &request) {
    std::optional<lease_t> held;
    {
      std::lock_guard lock { mutex_ };
      if (const auto reason = admission_reason_locked(); reason != reject_reason_e::none) {
        return { std::nullopt, reason };
      }
      held = lease_;
    }

    // Probes run unlocked; their answers apply only if the lease did not change meanwhile.
    bool ticket_gone = false;
    if (held && held->stage == lease_stage_e::bound && options_.probes.ticket_present) {
      ticket_gone = !options_.probes.ticket_present(held->launch_session_id);
    }
    const bool legacy_pending = options_.managed_mode && options_.probes.legacy_restore_pending &&
                                options_.probes.legacy_restore_pending();

    std::lock_guard lock { mutex_ };
    if (const auto reason = admission_reason_locked(); reason != reject_reason_e::none) {
      return { std::nullopt, reason };
    }
    if (ticket_gone && lease_ && held && lease_->transaction == held->transaction && lease_->stage == held->stage) {
      release_lease_locked(lease_->flavor == launch_flavor_e::managed, command_kind_e::native_restore);
    }
    if (lease_) {
      const bool supersede = lease_->flavor == launch_flavor_e::legacy && lease_->stage == lease_stage_e::bound &&
                             !request.client_key.empty() && request.client_key == lease_->client_key;
      if (!supersede) {
        return { std::nullopt, reject_reason_e::owner_busy };
      }
      release_lease_locked(false, command_kind_e::native_restore);
    }
    if (options_.managed_mode) {
      if (native_.in_conflict) {
        return { std::nullopt, reject_reason_e::recovery_required };
      }
      if (legacy_pending) {
        return { std::nullopt, reject_reason_e::legacy_restore_pending };
      }
    }

    lease_t lease;
    lease.transaction = next_transaction_++;
    lease.launch_session_id = request.launch_session_id;
    lease.flavor = options_.managed_mode ? launch_flavor_e::managed : launch_flavor_e::legacy;
    lease.client_key = request.client_key;
    lease.setup_deadline = now() + options_.setup_timeout;
    lease_ = lease;
    return { launch_token_t { lease.transaction, lease.launch_session_id, lease.flavor, lease.setup_deadline },
      reject_reason_e::none };
  }

  command_result_t
  coordinator_t::run_launch_prepare(const launch_token_t &token, command_body_t body) {
    if (on_worker_thread()) {
      return outcome(outcome_e::reentrant);
    }

    auto state = std::make_shared<command_state_t>();
    submission_t submission;
    monotonic_time_t deadline {};
    {
      std::lock_guard lock { mutex_ };
      if (const auto reason = evaluate_locked(command_kind_e::launch_prepare, token.transaction, false);
        reason != reject_reason_e::none) {
        return rejected(reason);
      }
      if (lease_->prepare_submitted) {
        return rejected(reject_reason_e::stale_transaction);
      }
      lease_->prepare_submitted = true;
      deadline = lease_->setup_deadline;
      submission = submit_locked(command_kind_e::launch_prepare, state, std::move(body), token.transaction, deadline, false);
      lease_->prepare_handle = submission.handle;
    }

    auto result = wait(submission, state, deadline);
    if (result.outcome == outcome_e::completed) {
      std::lock_guard lock { mutex_ };
      if (lease_ && lease_->transaction == token.transaction && lease_->stage == lease_stage_e::preparing) {
        lease_->prepared = true;
      }
    }
    return result;
  }

  bool
  coordinator_t::bind_launch(const launch_token_t &token) {
    std::lock_guard lock { mutex_ };
    if (!lease_ || lease_->transaction != token.transaction || lease_->stage != lease_stage_e::preparing ||
        !lease_->prepared) {
      return false;
    }
    if (lease_->activated_before_bind) {
      if (lease_->flavor == launch_flavor_e::managed) {
        lease_->stage = lease_stage_e::active;
        lease_->idle_since.reset();
      }
      else {
        lease_.reset();
      }
      return true;
    }
    lease_->stage = lease_stage_e::bound;
    return true;
  }

  bool
  coordinator_t::abort_launch(const launch_token_t &token, command_body_t legacy_cleanup) {
    std::lock_guard lock { mutex_ };
    if (!lease_ || lease_->transaction != token.transaction || lease_->stage != lease_stage_e::preparing) {
      return false;
    }
    if (lease_->prepare_handle && queue_) {
      // Only this launch's own command; the cleanup posted below runs after it returns.
      queue_->cancel(*lease_->prepare_handle);
    }
    const bool managed = lease_->flavor == launch_flavor_e::managed;
    release_lease_locked(managed, command_kind_e::launch_failed_restore);
    if (!managed && legacy_cleanup) {
      submit_posted_locked(command_kind_e::launch_failed_restore, std::move(legacy_cleanup));
    }
    return true;
  }

  void
  coordinator_t::launch_activated(std::uint32_t launch_session_id) {
    std::lock_guard lock { mutex_ };
    if (!lease_ || lease_->launch_session_id != launch_session_id) {
      return;
    }
    switch (lease_->stage) {
      case lease_stage_e::preparing:
        lease_->activated_before_bind = true;
        break;
      case lease_stage_e::bound:
        if (lease_->flavor == launch_flavor_e::managed) {
          lease_->stage = lease_stage_e::active;
          lease_->idle_since.reset();
        }
        else {
          lease_.reset();
        }
        break;
      case lease_stage_e::active:
        break;
    }
  }

  bool
  coordinator_t::end_managed_lease(command_kind_e kind, std::optional<std::uint32_t> launch_session_id) {
    if (!kind_allows(kind, operation_e::native_restore, true)) {
      throw std::invalid_argument { std::string { "Topology command " } + std::string { to_string(kind) } +
                                    " cannot end a managed lease" };
    }
    std::lock_guard lock { mutex_ };
    if (!lease_ || lease_->flavor != launch_flavor_e::managed || lease_->stage == lease_stage_e::preparing ||
        (launch_session_id && lease_->launch_session_id != *launch_session_id)) {
      return false;
    }
    release_lease_locked(true, kind);
    return true;
  }

  void
  coordinator_t::tick() {
    std::optional<lease_t> held;
    {
      std::lock_guard lock { mutex_ };
      if (phase_ != coordinator_phase_e::running && phase_ != coordinator_phase_e::draining) {
        return;
      }
      held = lease_;
    }
    if (!held) {
      return;
    }

    bool ticket_gone = false;
    std::optional<std::size_t> streams;
    if (held->stage == lease_stage_e::bound && options_.probes.ticket_present) {
      ticket_gone = !options_.probes.ticket_present(held->launch_session_id);
    }
    else if (held->stage == lease_stage_e::active && options_.probes.active_streams) {
      streams = options_.probes.active_streams();
    }

    std::lock_guard lock { mutex_ };
    if (!lease_ || lease_->transaction != held->transaction || lease_->stage != held->stage) {
      return;
    }
    if (ticket_gone) {
      // The ticket expired or was removed before the stream connected.
      release_lease_locked(lease_->flavor == launch_flavor_e::managed, command_kind_e::native_restore);
      return;
    }
    if (!streams) {
      return;
    }
    if (*streams != 0) {
      lease_->idle_since.reset();
      return;
    }
    const auto current = now();
    if (!lease_->idle_since) {
      lease_->idle_since = current;
      return;
    }
    if (current - *lease_->idle_since >= options_.native_idle_grace) {
      release_lease_locked(true, command_kind_e::native_restore);
    }
  }

  void
  coordinator_t::tick_loop() {
    tls_ticking_coordinator = this;
    std::unique_lock lock { tick_mutex_ };
    while (!tick_stop_) {
      if (tick_wake_.wait_for(lock, options_.tick_interval, [this]() { return tick_stop_; })) {
        break;
      }
      lock.unlock();
      try {
        tick();
      }
      catch (...) {
        // A throwing probe must not end the thread (and with it the process); retry next tick.
      }
      lock.lock();
    }
  }

  void
  coordinator_t::begin_draining() {
    std::lock_guard lock { mutex_ };
    if (phase_ == coordinator_phase_e::running || phase_ == coordinator_phase_e::recovering) {
      phase_ = coordinator_phase_e::draining;
    }
    if (lease_ && lease_->stage == lease_stage_e::preparing && lease_->prepare_handle && queue_) {
      queue_->cancel(*lease_->prepare_handle);
    }
  }

  void
  coordinator_t::stop() {
    if (on_worker_thread() || tls_ticking_coordinator == this) {
      throw std::logic_error { "Topology coordinator cannot be stopped from its own worker or tick thread" };
    }
    const std::lock_guard lifecycle { lifecycle_mutex_ };
    std::unique_ptr<topology_command_queue_t> queue;
    {
      std::lock_guard lock { mutex_ };
      phase_ = coordinator_phase_e::stopped;
      stopping_ = true;
      queue = std::move(queue_);
    }

    {
      std::lock_guard lock { tick_mutex_ };
      tick_stop_ = true;
    }
    tick_wake_.notify_all();
    if (tick_thread_.joinable()) {
      tick_thread_.join();
    }

    // Joins the worker once the running command returns; pending ones complete as stopped.
    queue.reset();
    complete_startup(outcome(outcome_e::stopped));
  }

  void
  coordinator_t::install_fence(std::function<bool()> settled) {
    std::lock_guard lock { mutex_ };
    fence_ = std::move(settled);
  }

  void
  coordinator_t::publish_native_health(native_health_t health) {
    std::lock_guard lock { mutex_ };
    native_ = std::move(health);
  }

  coordinator_status_t
  coordinator_t::status() const {
    std::lock_guard lock { mutex_ };
    coordinator_status_t result;
    result.phase = phase_;
    result.managed_mode = options_.managed_mode;
    if (lease_) {
      result.lease = lease_snapshot_t { lease_->transaction, lease_->launch_session_id, lease_->flavor, lease_->stage };
    }
    result.native = native_;
    result.fence_active = fence_ && !fence_();
    result.native_restores_posted = native_restores_posted_;
    result.submitted_commands = submitted_commands_;
    return result;
  }

}  // namespace display_device::topology
