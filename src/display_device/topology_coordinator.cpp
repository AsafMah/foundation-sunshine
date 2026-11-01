/**
 * @file src/display_device/topology_coordinator.cpp
 * @brief Serialized topology coordinator: admission policy, launch lease and recovery-first start.
 */
#include "topology_coordinator.h"

#include <algorithm>
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

    posted_command_t
    rejected_post(reject_reason_e reason) {
      std::promise<command_result_t> signal;
      auto future = signal.get_future().share();
      signal.set_value(rejected(reason));
      return { {}, reason, std::move(future) };
    }

    void
    require_routable(command_kind_e kind) {
      if (kind == command_kind_e::startup_recovery || kind == command_kind_e::legacy_startup_restore ||
          kind == command_kind_e::launch_prepare) {
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
    std::promise<command_result_t> posted_completion;
    std::shared_future<command_result_t> posted_future { posted_completion.get_future().share() };
    std::once_flag posted_once;
    std::shared_ptr<startup_restore_t> startup_restore;
    bool holds_startup_fence {};

    void
    complete_posted(command_result_t result) {
      std::call_once(posted_once, [&]() { posted_completion.set_value(std::move(result)); });
    }
  };

  struct coordinator_t::startup_signal_t {
    std::once_flag once;
    std::promise<command_result_t> promise;
    std::shared_future<command_result_t> future { promise.get_future().share() };
  };

  struct coordinator_t::startup_restore_t {
    explicit startup_restore_t(command_body_t restore):
        body { std::move(restore) } {}

    const command_body_t body;
    posted_command_t attempt;
  };

  coordinator_t::coordinator_t(coordinator_options_t options):
      options_ { std::move(options) },
      clock_ { options_.clock ? options_.clock : topology_command_queue_t::clock_function_t { &steady_now } } {
    if (options_.managed_mode && !options_.native_restore) {
      throw std::invalid_argument { "Managed topology mode needs a native restore body" };
    }
    if (options_.managed_mode && options_.max_managed_attachments == 0) {
      throw std::invalid_argument { "Managed topology needs at least one attachment" };
    }
    if (options_.managed_mode && !options_.probes.active_streams) {
      throw std::invalid_argument { "Managed topology needs a video-quiescence probe" };
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

  void
  coordinator_t::defer_startup_restore(const mutation_capability_t &capability, command_body_t restore) {
    capability.require(operation_e::restore_state);
    if (!on_worker_thread() || capability.kind() != command_kind_e::startup_recovery) {
      throw capability_error_t { "Only the initial startup worker can register deferred restoration" };
    }
    if (!restore || !options_.probes.startup_restore_quiescent) {
      throw std::invalid_argument { "Deferred startup restoration needs a body and startup/ticket/session quiescence probe" };
    }
    auto obligation = std::make_shared<startup_restore_t>(std::move(restore));
    std::lock_guard lock { mutex_ };
    if (phase_ != coordinator_phase_e::recovering || stopping_ || startup_restore_) {
      throw std::logic_error { "Startup restoration can only be registered once during initial recovery" };
    }
    startup_restore_ = std::move(obligation);
  }

  posted_command_t
  coordinator_t::retry_startup_restore() {
    // A previous failure may own an exception with a reentrant destructor.
    posted_command_t previous;
    std::lock_guard lock { mutex_ };
    if (startup_restore_inflight_) {
      return startup_restore_inflight_->attempt;
    }
    if (const auto reason = evaluate_locked(command_kind_e::legacy_startup_restore, std::nullopt, false);
      reason != reject_reason_e::none) {
      return rejected_post(reason);
    }
    auto state = std::make_shared<command_state_t>();
    state->startup_restore = startup_restore_;
    previous = std::move(startup_restore_->attempt);
    startup_restore_->attempt = submit_posted_locked(command_kind_e::legacy_startup_restore, {}, std::move(state));
    startup_restore_inflight_ = startup_restore_;
    return startup_restore_->attempt;
  }

  void
  coordinator_t::forget_startup_restore(const mutation_capability_t &capability) {
    capability.require(operation_e::reset_persistence);
    if (!on_worker_thread()) {
      throw capability_error_t { "Only this coordinator worker can forget startup restoration" };
    }
    std::shared_ptr<startup_restore_t> forgotten;
    {
      std::lock_guard lock { mutex_ };
      forgotten = std::move(startup_restore_);
    }
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
    if (kind == command_kind_e::legacy_startup_restore) {
      if (phase_ == coordinator_phase_e::draining) {
        return reject_reason_e::draining;
      }
      if (!startup_restore_) {
        return reject_reason_e::stale_transaction;
      }
      if (lease_) {
        return reject_reason_e::owner_busy;
      }
      return external_fence_active_locked() ? reject_reason_e::fence_active : reject_reason_e::none;
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
        if (legacy_restore_pending || startup_restore_) {
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
    return startup_restore_attempt_active_ || external_fence_active_locked();
  }

  bool
  coordinator_t::external_fence_active_locked() {
    std::erase_if(fences_, [](const auto &settled) { return settled(); });
    return !fences_.empty();
  }

  topology_command_queue_t::task_t
  coordinator_t::make_task(command_kind_e kind, std::shared_ptr<command_state_t> state, command_body_t body,
    std::optional<std::uint64_t> transaction) {
    return [this, kind, state = std::move(state), body = std::move(body), transaction](const command_context_t &context) {
      using stage_e = command_state_t::stage_e;
      const running_scope_t scope { this };

      bool legacy_pending = false;
      if (kind == command_kind_e::launch_prepare && options_.managed_mode && options_.probes.legacy_restore_pending) {
        legacy_pending = options_.probes.legacy_restore_pending();
      }

      auto startup_preflight = reject_reason_e::none;
      bool startup_quiescent = false;
      if (kind == command_kind_e::legacy_startup_restore) {
        {
          std::lock_guard lock { mutex_ };
          startup_preflight = evaluate_locked(kind, transaction, false);
          if (startup_preflight == reject_reason_e::none && startup_restore_ != state->startup_restore) {
            startup_preflight = reject_reason_e::stale_transaction;
          }
          if (startup_preflight == reject_reason_e::none) {
            startup_restore_attempt_active_ = true;
            state->holds_startup_fence = true;
          }
        }
        if (startup_preflight == reject_reason_e::none) {
          startup_quiescent = options_.probes.startup_restore_quiescent();
        }
      }

      reject_reason_e reason;
      {
        std::lock_guard lock { mutex_ };
        reason = startup_preflight != reject_reason_e::none ? startup_preflight : evaluate_locked(kind, transaction, legacy_pending);
        if (kind == command_kind_e::legacy_startup_restore && reason == reject_reason_e::none) {
          if (startup_restore_ != state->startup_restore) {
            reason = reject_reason_e::stale_transaction;
          }
          else if (!startup_quiescent) {
            reason = reject_reason_e::owner_busy;
          }
        }
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
        if (reason == reject_reason_e::none && kind == command_kind_e::launch_prepare) {
          lease_->prepare_started = true;
        }
      }

      if (reason != reject_reason_e::none) {
        if (kind == command_kind_e::startup_recovery) {
          // Only a stop() racing the worker can reject startup recovery.
          complete_startup(outcome(outcome_e::stopped));
        }
        return;
      }

      const mutation_capability_t capability { kind, options_.managed_mode, std::this_thread::get_id(), external_mutation_ };
      if (kind == command_kind_e::legacy_startup_restore) {
        state->startup_restore->body(capability, context);
        std::lock_guard lock { mutex_ };
        if (startup_restore_ == state->startup_restore) {
          startup_restore_.reset();
        }
        return;
      }
      if (kind != command_kind_e::startup_recovery) {
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
      finish_recovery(result.outcome != outcome_e::failed);
      complete_startup(std::move(result));
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
    options_.posted_observer(kind, result);
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
    return post_tracked(kind, std::move(body)).reason;
  }

  posted_command_t
  coordinator_t::post_tracked(command_kind_e kind, command_body_t body) {
    require_routable(kind);
    std::lock_guard lock { mutex_ };
    if (const auto reason = evaluate_locked(kind, std::nullopt, false); reason != reject_reason_e::none) {
      return rejected_post(reason);
    }
    return submit_posted_locked(kind, std::move(body));
  }

  coordinator_t::submission_t
  coordinator_t::submit_locked(command_kind_e kind, const std::shared_ptr<command_state_t> &state, command_body_t body,
    std::optional<std::uint64_t> transaction, monotonic_time_t deadline, bool posted) {
    ++submitted_commands_;
    topology_command_queue_t::completion_function_t completed;
    if (posted) {
      // Posted commands use the current queue generation and no deadline, so the queue
      // always invokes this on the worker, never synchronously under our submit lock.
      completed = [this, kind, state](const command_completion_t &completion) {
        const running_scope_t scope { this };
        bool stopping;
        {
          std::lock_guard lock { mutex_ };
          stopping = stopping_;
        }
        auto result = map_completion(completion, *state, stopping);
        try {
          notify_posted(kind, result);
        }
        catch (...) {
          result = { outcome_e::failed, reject_reason_e::none, std::current_exception() };
        }
        if (state->startup_restore) {
          std::lock_guard lock { mutex_ };
          if (state->holds_startup_fence) {
            startup_restore_attempt_active_ = false;
          }
          startup_restore_inflight_.reset();
          state->complete_posted(std::move(result));
          return;
        }
        state->complete_posted(std::move(result));
      };
    }
    return queue_->submit(queue_generation_, deadline, make_task(kind, state, std::move(body), transaction), std::move(completed));
  }

  posted_command_t
  coordinator_t::submit_posted_locked(command_kind_e kind, command_body_t body, std::shared_ptr<command_state_t> state) {
    if (!state) {
      state = std::make_shared<command_state_t>();
    }
    auto completion = state->posted_future;
    if (!queue_) {
      state->complete_posted(outcome(outcome_e::stopped));
      return { {}, reject_reason_e::stopped, std::move(completion) };
    }
    auto submission = submit_locked(kind, state, std::move(body), std::nullopt, no_deadline, true);
    return { submission.handle, reject_reason_e::none, std::move(completion) };
  }

  void
  coordinator_t::release_lease_locked(bool post_native_restore, command_kind_e restore_kind) {
    const auto restore_state = lease_ ? lease_->restore_state : nullptr;
    if (post_native_restore && options_.native_restore && queue_) {
      // Retain the retiring lease if constructing or submitting the command throws.
      submit_posted_locked(restore_kind, guarded_native_restore(), restore_state);
      ++native_restores_posted_;
    }
    else if (restore_state) {
      restore_state->complete_posted(outcome(post_native_restore || stopping_ ? outcome_e::stopped : outcome_e::completed));
    }
    lease_.reset();
  }

  bool
  coordinator_t::expire_preparing_locked() {
    if (!lease_ || lease_->stage != lease_stage_e::preparing || now() < lease_->setup_deadline) {
      return false;
    }
    const bool body_started = lease_->prepare_started;
    if (lease_->prepare_state) {
      using stage_e = command_state_t::stage_e;
      auto stage = stage_e::pending;
      lease_->prepare_state->stage.compare_exchange_strong(stage, stage_e::abandoned, std::memory_order_acq_rel);
    }
    if (lease_->prepare_handle && queue_) {
      queue_->cancel(*lease_->prepare_handle);
    }
    const bool managed = lease_->flavor == launch_flavor_e::managed;
    auto cleanup = lease_->legacy_expiry_cleanup;
    if (!managed && body_started && cleanup) {
      submit_posted_locked(command_kind_e::launch_failed_restore, std::move(cleanup));
    }
    release_lease_locked(managed && body_started, command_kind_e::launch_failed_restore);
    return true;
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
    if (options_.managed_mode) {
      {
        std::lock_guard lock { mutex_ };
        if (const auto reason = admission_reason_locked(); reason != reject_reason_e::none) {
          return { std::nullopt, reason };
        }
      }
      tick();
    }
    std::optional<lease_t> held;
    {
      std::lock_guard lock { mutex_ };
      if (const auto reason = admission_reason_locked(); reason != reject_reason_e::none) {
        return { std::nullopt, reason };
      }
      expire_preparing_locked();
      held = lease_;
    }

    // Probes run unlocked; their answers apply only if the lease did not change meanwhile.
    bool ticket_gone = false;
    if (held && held->flavor == launch_flavor_e::legacy && held->stage == lease_stage_e::bound && options_.probes.ticket_present) {
      ticket_gone = !options_.probes.ticket_present(held->launch_session_id);
    }
    const bool legacy_pending = options_.managed_mode && options_.probes.legacy_restore_pending &&
                                options_.probes.legacy_restore_pending();

    std::lock_guard lock { mutex_ };
    if (const auto reason = admission_reason_locked(); reason != reject_reason_e::none) {
      return { std::nullopt, reason };
    }
    expire_preparing_locked();
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
      if (legacy_pending || startup_restore_) {
        return { std::nullopt, reject_reason_e::legacy_restore_pending };
      }
    }

    lease_t lease;
    lease.transaction = next_transaction_++;
    lease.launch_session_id = request.launch_session_id;
    lease.flavor = options_.managed_mode ? launch_flavor_e::managed : launch_flavor_e::legacy;
    lease.client_key = request.client_key;
    lease.setup_deadline = now() + options_.setup_timeout;
    if (options_.managed_mode) {
      lease.restore_state = std::make_shared<command_state_t>();
      lease.attachments.push_back({ 0, request.launch_session_id, lease_stage_e::preparing, lease.setup_deadline });
    }
    lease_ = lease;
    return { launch_token_t { lease.transaction, lease.launch_session_id, lease.flavor, lease.setup_deadline },
      reject_reason_e::none };
  }

  launch_admission_t
  coordinator_t::admit_managed_join(const launch_request_t &request) {
    std::lock_guard lock { mutex_ };
    if (const auto reason = admission_reason_locked(); reason != reject_reason_e::none) {
      return { std::nullopt, reason };
    }
    expire_preparing_locked();
    expire_joins_locked();
    if (!lease_ || lease_->flavor != launch_flavor_e::managed) {
      return { std::nullopt, reject_reason_e::stale_transaction };
    }
    if (native_.in_conflict) {
      return { std::nullopt, reject_reason_e::recovery_required };
    }
    if (lease_->stage == lease_stage_e::preparing || lease_->attachments.empty() ||
        lease_->attachments.size() >= options_.max_managed_attachments ||
        std::any_of(lease_->attachments.begin(), lease_->attachments.end(),
          [&](const auto &ref) { return ref.launch_session_id == request.launch_session_id; })) {
      return { std::nullopt, reject_reason_e::owner_busy };
    }
    const auto serial = lease_->next_attachment_serial++;
    const auto deadline = now() + options_.setup_timeout;
    lease_->attachments.push_back({ serial, request.launch_session_id, lease_stage_e::preparing, deadline });
    touch_managed_locked();
    return { launch_token_t { lease_->transaction, request.launch_session_id, launch_flavor_e::managed, deadline, serial },
      reject_reason_e::none };
  }

  bool
  coordinator_t::matches_owner_locked(const launch_token_t &token) const {
    return lease_ && lease_->transaction == token.transaction && lease_->launch_session_id == token.launch_session_id &&
           lease_->flavor == token.flavor && token.attachment_serial == 0;
  }

  coordinator_t::attachment_t *
  coordinator_t::find_attachment_locked(const launch_token_t &token) {
    if (!lease_ || lease_->flavor != launch_flavor_e::managed || token.flavor != launch_flavor_e::managed ||
        lease_->transaction != token.transaction) {
      return nullptr;
    }
    const auto found = std::find_if(lease_->attachments.begin(), lease_->attachments.end(), [&](const auto &ref) {
      return ref.serial == token.attachment_serial && ref.launch_session_id == token.launch_session_id;
    });
    return found == lease_->attachments.end() ? nullptr : &*found;
  }

  void
  coordinator_t::touch_managed_locked() {
    ++lease_->activity_generation;
    lease_->idle_since.reset();
  }

  void
  coordinator_t::remove_attachment_locked(std::uint64_t serial, command_kind_e restore_kind) {
    std::erase_if(lease_->attachments, [&](const auto &ref) { return ref.serial == serial; });
    if (!lease_->restore_kind) {
      lease_->restore_kind = restore_kind;
    }
    touch_managed_locked();
  }

  void
  coordinator_t::expire_joins_locked() {
    if (!lease_ || lease_->flavor != launch_flavor_e::managed) {
      return;
    }
    const auto current = now();
    const auto removed = std::erase_if(lease_->attachments, [&](const auto &ref) {
      return ref.serial != 0 && ref.stage == lease_stage_e::preparing && current >= ref.setup_deadline;
    });
    if (removed != 0) {
      if (!lease_->restore_kind) {
        lease_->restore_kind = command_kind_e::native_restore;
      }
      touch_managed_locked();
    }
  }

  bool
  coordinator_t::launch_activated(const launch_token_t &token) {
    std::lock_guard lock { mutex_ };
    if (!lease_ || stopping_ || phase_ != coordinator_phase_e::running) {
      return false;
    }
    if (lease_->flavor == launch_flavor_e::legacy) {
      if (!matches_owner_locked(token) || lease_->stage != lease_stage_e::bound) {
        return false;
      }
      lease_.reset();
      return true;
    }
    auto *ref = find_attachment_locked(token);
    if (!ref || ref->stage != lease_stage_e::bound) {
      return false;
    }
    ref->stage = lease_stage_e::active;
    lease_->stage = lease_stage_e::active;
    touch_managed_locked();
    return true;
  }

  bool
  coordinator_t::end_managed_launch(const launch_token_t &token, command_kind_e kind,
    std::shared_future<command_result_t> *restoration) {
    if (restoration) {
      *restoration = {};
    }
    if (!kind_allows(kind, operation_e::native_restore, true)) {
      throw std::invalid_argument { std::string { "Topology command " } + std::string { to_string(kind) } +
                                    " cannot end a managed launch" };
    }
    {
      std::lock_guard lock { mutex_ };
      const auto *ref = find_attachment_locked(token);
      if (!ref || ref->stage == lease_stage_e::preparing) {
        return false;
      }
      remove_attachment_locked(ref->serial, kind);
      if (restoration && lease_->attachments.empty()) {
        *restoration = lease_->restore_state->posted_future;
      }
    }
    tick();
    return true;
  }

  command_result_t
  coordinator_t::run_launch_prepare(const launch_token_t &token, command_body_t body, command_body_t legacy_expiry_cleanup) {
    return run_prepare(token, std::move(body), std::move(legacy_expiry_cleanup), prepare_mode_e::one_shot);
  }

  command_result_t
  coordinator_t::run_launch_step(const launch_token_t &token, command_body_t body, command_body_t legacy_expiry_cleanup) {
    return run_prepare(token, std::move(body), std::move(legacy_expiry_cleanup), prepare_mode_e::staged);
  }

  command_result_t
  coordinator_t::run_prepare(const launch_token_t &token, command_body_t body, command_body_t legacy_expiry_cleanup, prepare_mode_e mode) {
    if (on_worker_thread()) {
      return outcome(outcome_e::reentrant);
    }

    auto state = std::make_shared<command_state_t>();
    submission_t submission;
    monotonic_time_t deadline {};
    {
      std::lock_guard lock { mutex_ };
      if (!matches_owner_locked(token)) {
        return rejected(reject_reason_e::stale_transaction);
      }
      if (const auto reason = evaluate_locked(command_kind_e::launch_prepare, token.transaction, false);
        reason != reject_reason_e::none) {
        return rejected(reason);
      }
      const bool first = lease_->prepare_mode == prepare_mode_e::not_submitted;
      if (!first && (mode != prepare_mode_e::staged || lease_->prepare_mode != prepare_mode_e::staged ||
                      !lease_->step_completed || lease_->prepared)) {
        return rejected(reject_reason_e::stale_transaction);
      }
      if (!first && legacy_expiry_cleanup) {
        throw std::invalid_argument { "A staged launch must retain its initial cleanup" };
      }
      if (first) {
        lease_->legacy_expiry_cleanup = std::move(legacy_expiry_cleanup);
      }
      lease_->prepare_mode = mode;
      lease_->step_completed = false;
      lease_->prepare_state = state;
      deadline = lease_->setup_deadline;
      submission = submit_locked(command_kind_e::launch_prepare, state, std::move(body), token.transaction, deadline, false);
      lease_->prepare_handle = submission.handle;
    }

    auto result = wait(submission, state, deadline);
    if (result.outcome == outcome_e::completed) {
      std::lock_guard lock { mutex_ };
      if (matches_owner_locked(token) && lease_->stage == lease_stage_e::preparing && lease_->prepare_state == state) {
        lease_->step_completed = true;
        lease_->prepared = mode == prepare_mode_e::one_shot;
      }
    }
    return result;
  }

  bool
  coordinator_t::finish_launch_prepare(const launch_token_t &token) {
    std::lock_guard lock { mutex_ };
    if (phase_ != coordinator_phase_e::running || stopping_) {
      return false;
    }
    expire_preparing_locked();
    if (!matches_owner_locked(token) || lease_->stage != lease_stage_e::preparing ||
        lease_->prepare_mode != prepare_mode_e::staged || !lease_->step_completed ||
        lease_->prepared || fence_active_locked()) {
      return false;
    }
    lease_->prepared = true;
    return true;
  }

  bool
  coordinator_t::bind_launch(const launch_token_t &token) {
    std::lock_guard lock { mutex_ };
    if (phase_ != coordinator_phase_e::running || stopping_) {
      return false;
    }
    expire_preparing_locked();
    expire_joins_locked();
    if (!lease_ || fence_active_locked()) {
      return false;
    }
    if (lease_->flavor == launch_flavor_e::managed) {
      auto *ref = find_attachment_locked(token);
      if (!ref || ref->stage != lease_stage_e::preparing || !lease_->prepared) {
        return false;
      }
      ref->stage = lease_stage_e::bound;
      if (token.attachment_serial == 0) {
        lease_->stage = lease_stage_e::bound;
      }
      touch_managed_locked();
      return true;
    }
    if (!matches_owner_locked(token) || lease_->stage != lease_stage_e::preparing || !lease_->prepared) {
      return false;
    }
    lease_->stage = lease_stage_e::bound;
    return true;
  }

  std::optional<monotonic_time_t>
  coordinator_t::launch_setup_deadline(const launch_token_t &token) {
    std::lock_guard lock { mutex_ };
    if (phase_ != coordinator_phase_e::running || stopping_) {
      return std::nullopt;
    }
    expire_preparing_locked();
    if (!matches_owner_locked(token) || lease_->stage != lease_stage_e::preparing) {
      return std::nullopt;
    }
    return lease_->setup_deadline;
  }

  bool
  coordinator_t::abort_launch(const launch_token_t &token, command_body_t legacy_cleanup) {
    if (token.attachment_serial != 0) {
      {
        std::lock_guard lock { mutex_ };
        const auto *ref = find_attachment_locked(token);
        if (!ref || ref->stage != lease_stage_e::preparing) {
          return false;
        }
        remove_attachment_locked(ref->serial, command_kind_e::launch_failed_restore);
      }
      tick();
      return true;
    }
    std::lock_guard lock { mutex_ };
    if (!matches_owner_locked(token) || lease_->stage != lease_stage_e::preparing) {
      return false;
    }
    const bool staged = lease_->prepare_mode == prepare_mode_e::staged;
    if (staged) {
      if (legacy_cleanup) {
        throw std::invalid_argument { "A staged abort must use its initial cleanup" };
      }
      if (lease_->prepare_started) {
        legacy_cleanup = lease_->legacy_expiry_cleanup;
      }
    }
    if (lease_->prepare_handle && queue_) {
      // Only this launch's own command; the cleanup posted below runs after it returns.
      queue_->cancel(*lease_->prepare_handle);
    }
    const bool managed = lease_->flavor == launch_flavor_e::managed;
    if (!managed && legacy_cleanup) {
      submit_posted_locked(command_kind_e::launch_failed_restore, std::move(legacy_cleanup));
    }
    release_lease_locked(managed && (!staged || lease_->prepare_started), command_kind_e::launch_failed_restore);
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
      expire_preparing_locked();
      expire_joins_locked();
      held = lease_;
    }
    if (!held) {
      return;
    }

    std::vector<attachment_t> gone;
    bool ticket_gone = false;
    std::optional<std::size_t> streams;
    if (held->flavor == launch_flavor_e::managed) {
      for (const auto &ref : held->attachments) {
        if (ref.stage == lease_stage_e::bound && options_.probes.ticket_present &&
            !options_.probes.ticket_present(ref.launch_session_id)) {
          gone.push_back(ref);
        }
      }
      if (held->stage != lease_stage_e::preparing && options_.probes.active_streams) {
        streams = options_.probes.active_streams();
      }
    }
    else if (held->stage == lease_stage_e::bound && options_.probes.ticket_present) {
      ticket_gone = !options_.probes.ticket_present(held->launch_session_id);
    }

    std::lock_guard lock { mutex_ };
    if (!lease_ || lease_->transaction != held->transaction) {
      return;
    }
    if (held->flavor == launch_flavor_e::legacy) {
      if (ticket_gone && lease_->stage == held->stage) {
        release_lease_locked(false, command_kind_e::native_restore);
      }
      return;
    }
    const bool activity_unchanged = lease_->activity_generation == held->activity_generation;
    for (const auto &old : gone) {
      const auto *ref = find_attachment_locked({ held->transaction, old.launch_session_id, launch_flavor_e::managed,
        old.setup_deadline, old.serial });
      if (ref && ref->stage == old.stage) {
        remove_attachment_locked(ref->serial, command_kind_e::native_restore);
      }
    }
    if (!activity_unchanged) {
      return;
    }
    if (lease_->attachments.empty()) {
      if (streams && *streams == 0) {
        release_lease_locked(true, lease_->restore_kind.value_or(command_kind_e::native_restore));
      }
      return;
    }
    const bool all_active = std::all_of(lease_->attachments.begin(), lease_->attachments.end(),
      [](const auto &ref) { return ref.stage == lease_stage_e::active; });
    if (!all_active || !streams) {
      lease_->idle_since.reset();
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
      release_lease_locked(true, lease_->restore_kind.value_or(command_kind_e::native_restore));
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
    std::shared_ptr<startup_restore_t> forgotten;
    {
      std::lock_guard lock { mutex_ };
      phase_ = coordinator_phase_e::stopped;
      stopping_ = true;
      if (lease_ && lease_->restore_state) {
        lease_->restore_state->complete_posted(outcome(outcome_e::stopped));
      }
      queue = std::move(queue_);
      forgotten = std::move(startup_restore_);
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
    if (!settled) {
      throw std::invalid_argument { "Topology fence needs a settlement predicate" };
    }
    std::lock_guard lock { mutex_ };
    fences_.push_back(std::move(settled));
  }

  external_mutation_fence_t::external_mutation_fence_t(std::shared_ptr<std::atomic_bool> blocked,
    std::shared_ptr<const dispatch_hold_t> dispatch):
      blocked_ { std::move(blocked) },
      dispatch_ { std::move(dispatch) } {}

  external_mutation_fence_t::~external_mutation_fence_t() {
    if (armed_) blocked_->store(false, std::memory_order_release);
  }

  std::shared_ptr<const external_mutation_fence_t>
  coordinator_t::hold_external_mutation(const mutation_capability_t &capability) {
    capability.require(operation_e::adapter_restart);
    return hold_external_actor(capability, false);
  }

  std::shared_ptr<const external_mutation_fence_t>
  coordinator_t::hold_external_recovery(const mutation_capability_t &capability) {
    capability.require(operation_e::native_recover);
    if (capability.kind() != command_kind_e::startup_recovery) {
      throw capability_error_t { "External recovery exclusion requires the startup command" };
    }
    return hold_external_actor(capability, true);
  }

  std::shared_ptr<const external_mutation_fence_t>
  coordinator_t::hold_external_actor(const mutation_capability_t &capability, bool recovering) {
    if (!on_worker_thread() || capability.external_mutation_ != external_mutation_) {
      throw std::logic_error { "External mutation exclusion needs its coordinator's worker capability" };
    }
    std::lock_guard lock { mutex_ };
    if (!recovering && (!queue_ || stopping_ || phase_ == coordinator_phase_e::draining)) {
      throw std::logic_error { "An external topology mutation cannot start while draining or stopped" };
    }
    if (!queue_ && !(recovering && stopping_ && phase_ == coordinator_phase_e::stopped)) {
      throw std::logic_error { "External recovery cannot adopt an actor without a running or stopping worker" };
    }
    // Stop may already own the queue. Its preflight rejects queued bodies; only
    // this still-running startup body needs raw exclusion before the worker joins.
    auto token = std::shared_ptr<external_mutation_fence_t> {
      new external_mutation_fence_t { external_mutation_, queue_ ? queue_->hold_dispatch() : nullptr }
    };
    fences_.push_back([weak = std::weak_ptr { token }]() { return weak.expired(); });
    external_mutation_->store(true, std::memory_order_release);
    token->armed_ = true;
    return token;
  }

  std::shared_ptr<const admission_fence_t>
  coordinator_t::hold_fence() {
    auto token = std::shared_ptr<const admission_fence_t> { new admission_fence_t };
    install_fence([weak = std::weak_ptr { token }]() { return weak.expired(); });
    return token;
  }

  bool
  probe_fence_t::on_worker_thread() const noexcept {
    return worker_ == std::this_thread::get_id();
  }

  std::shared_ptr<const probe_fence_t>
  coordinator_t::hold_probe_fence() {
    if (!on_worker_thread()) {
      throw std::logic_error { "A topology probe fence must be acquired on its mutation worker" };
    }
    auto token = std::shared_ptr<probe_fence_t> { new probe_fence_t };
    std::lock_guard lock { mutex_ };
    if (!queue_ || stopping_ || phase_ == coordinator_phase_e::draining) {
      throw std::logic_error { "A topology probe cannot start while draining or stopped" };
    }
    token->dispatch_ = queue_->hold_dispatch();
    fences_.push_back([weak = std::weak_ptr { token }]() { return weak.expired(); });
    return token;
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
      result.lease = lease_snapshot_t { lease_->transaction, lease_->launch_session_id, lease_->flavor, lease_->stage,
        lease_->attachments.size(), lease_->flavor == launch_flavor_e::managed && lease_->attachments.empty() };
    }
    result.native = native_;
    result.fence_active = startup_restore_attempt_active_ ||
                          std::any_of(fences_.begin(), fences_.end(), [](const auto &settled) { return !settled(); });
    result.startup_restore_pending = static_cast<bool>(startup_restore_);
    result.native_restores_posted = native_restores_posted_;
    result.submitted_commands = submitted_commands_;
    return result;
  }

}  // namespace display_device::topology
