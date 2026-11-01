#include "topology_routes.h"

#include <algorithm>
#include <condition_variable>
#include <limits>
#include <stdexcept>
#include <utility>

namespace display_device::topology {
  bool
  probe_handoff_t::publish(std::shared_ptr<const probe_fence_t> fence) {
    if (!fence) throw std::invalid_argument { "A probe handoff needs a fence" };
    std::lock_guard lock { mutex_ };
    if (state_ == state_e::abandoned) return false;
    if (state_ != state_e::pending) throw std::logic_error { "A probe fence can only be published once" };
    fence_ = std::move(fence);
    state_ = state_e::published;
    return true;
  }

  std::shared_ptr<const probe_fence_t>
  probe_handoff_t::take() {
    std::lock_guard lock { mutex_ };
    if (state_ != state_e::published) throw std::logic_error { "No published probe fence to claim" };
    state_ = state_e::claimed;
    return std::move(fence_);
  }

  std::shared_ptr<const probe_fence_t>
  probe_handoff_t::abandon() {
    std::lock_guard lock { mutex_ };
    state_ = state_e::abandoned;
    return std::move(fence_);
  }

  probe_timeout_error_t::probe_timeout_error_t(bool started):
      std::runtime_error { started ? "Encoder probe exceeded its setup deadline" : "Setup expired before encoder probing started" },
      started_ { started } {}

  bool
  probe_timeout_error_t::started() const noexcept {
    return started_;
  }

  void
  run_supervised_probe(std::chrono::steady_clock::time_point deadline,
    std::shared_ptr<const probe_fence_t> fence, std::function<void()> probe,
    std::function<void()> timeout, probe_thread_factory_t spawn) {
    if (!fence || !probe || !timeout) {
      throw std::invalid_argument { "Probe supervision requires a fence, a body and a fatal timeout handler" };
    }
    if (fence->on_worker_thread()) {
      throw std::logic_error { "The topology mutation worker cannot supervise a probe" };
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      throw probe_timeout_error_t { false };
    }
    struct state_t {
      std::mutex mutex;
      std::condition_variable wake;
      bool started {};
      bool cancelled {};
      bool finished {};
      bool timed_out {};
      std::exception_ptr probe_error, timeout_error;
    };
    auto state = std::make_shared<state_t>();
    std::jthread watchdog { [state, deadline, timeout = std::move(timeout)]() {
      std::unique_lock lock { state->mutex };
      state->wake.wait_until(lock, deadline, [&]() { return state->finished; });
      if (state->finished && !state->timed_out) return;
      state->timed_out = true;
      state->cancelled = true;
      const bool started = state->started;
      lock.unlock();
      if (started) {
        try {
          timeout();
        }
        catch (...) {
          state->timeout_error = std::current_exception();
        }
      }
    } };
    const auto finish = [&]() {
      {
        std::lock_guard lock { state->mutex };
        state->finished = true;
        state->cancelled = true;
        // A descheduled watchdog must not turn a late join into success.
        if (std::chrono::steady_clock::now() >= deadline) state->timed_out = true;
      }
      state->wake.notify_one();
      watchdog.join();
    };
    auto work = [state, deadline, probe = std::move(probe), guard = fence]() {
      try {
        {
          std::lock_guard lock { state->mutex };
          if (state->cancelled || std::chrono::steady_clock::now() >= deadline) {
            state->timed_out = true;
            return;
          }
          state->started = true;
        }
        probe();
      }
      catch (...) {
        state->probe_error = std::current_exception();
      }
    };
    try {
      auto worker = spawn ? spawn(std::move(work)) : std::jthread { std::move(work) };
      if (!worker.joinable()) {
        throw std::invalid_argument { "Probe thread factory did not return a joinable thread" };
      }
      // Future readiness, even at_thread_exit, precedes TLS teardown on MinGW.
      worker.join();
    }
    catch (...) {
      finish();
      throw;
    }
    finish();
    if (state->timeout_error) std::rethrow_exception(state->timeout_error);
    if (state->timed_out) throw probe_timeout_error_t { state->started };
    if (state->probe_error) std::rethrow_exception(state->probe_error);
  }

  retry_schedule_t::retry_schedule_t(monotonic_time_t delay):
      delay_ { delay } {
    if (delay_ <= monotonic_time_t::zero()) {
      throw std::invalid_argument { "Topology retry delay must be positive" };
    }
  }

  void
  retry_schedule_t::advance_generation() {
    if (generation_ == std::numeric_limits<std::uint64_t>::max()) {
      throw std::overflow_error { "Topology retry generation exhausted" };
    }
    ++generation_;
  }

  void
  retry_schedule_t::setup(bool enabled, monotonic_time_t now) {
    advance_generation();
    in_flight_.reset();
    due_ = enabled ? std::optional { now + delay_ } : std::nullopt;
  }

  std::optional<retry_token_t>
  retry_schedule_t::take_due(monotonic_time_t now) {
    if (!due_ || now < *due_) {
      return std::nullopt;
    }
    advance_generation();
    due_.reset();
    in_flight_ = retry_token_t { generation_ };
    return in_flight_;
  }

  bool
  retry_schedule_t::is_current(retry_token_t token) const {
    return in_flight_ == token;
  }

  retry_completion_e
  retry_schedule_t::complete(retry_token_t token, const command_result_t &result, bool retry_finished, monotonic_time_t now) {
    if (!is_current(token)) {
      return retry_completion_e::stale;
    }
    in_flight_.reset();
    const bool stopped = result.outcome == outcome_e::stopped ||
                         (result.outcome == outcome_e::rejected &&
                           (result.reason == reject_reason_e::draining || result.reason == reject_reason_e::stopped));
    if (stopped || (result.completed() && retry_finished)) {
      return retry_completion_e::disarmed;
    }
    due_ = now + delay_;
    return retry_completion_e::rearmed;
  }

  std::optional<monotonic_time_t>
  retry_schedule_t::next_wake() const {
    return due_;
  }

  bool
  retry_schedule_t::expedite(monotonic_time_t now) {
    if (!due_) return false;
    due_ = std::min(*due_, now);
    return true;
  }

  struct retry_timer_t::impl_t {
    struct attempt_t {
      retry_request_t request;
      retry_token_t token;
      command_kind_e kind;
      std::shared_ptr<const body_t> body;
    };

    coordinator_t &coordinator;
    std::mutex &owner_mutex;
    observer_t observer;
    std::mutex mutex;
    std::condition_variable wake;
    retry_schedule_t schedule;
    retry_request_t request;
    std::shared_ptr<const body_t> body;
    command_kind_e next_kind { command_kind_e::legacy_retry_tick };
    bool stopping {};
    std::thread thread;

    static monotonic_time_t
    now() {
      return std::chrono::duration_cast<monotonic_time_t>(std::chrono::steady_clock::now().time_since_epoch());
    }

    impl_t(coordinator_t &coordinator, std::mutex &owner_mutex, observer_t observer, monotonic_time_t delay):
        coordinator { coordinator }, owner_mutex { owner_mutex }, observer { observer }, schedule { delay } {
      if (!observer) throw std::invalid_argument { "A topology retry timer requires a completion observer" };
      thread = std::thread { [this] { run(); } };
    }

    ~impl_t() {
      if (coordinator.on_worker_thread() || thread.get_id() == std::this_thread::get_id()) std::terminate();
      {
        std::lock_guard lock { mutex };
        stopping = true;
      }
      wake.notify_one();
      thread.join();
    }

    retry_request_t
    setup(body_t replacement) {
      auto next = replacement ? std::make_shared<const body_t>(std::move(replacement)) : nullptr;
      retry_request_t result;
      {
        std::lock_guard lock { mutex };
        if (stopping) throw std::logic_error { "Cannot configure a stopping topology retry timer" };
        if (request.serial == std::numeric_limits<std::uint64_t>::max()) {
          throw std::overflow_error { "Topology retry request identity exhausted" };
        }
        schedule.setup(static_cast<bool>(next), now());
        result = { ++request.serial };
        body.swap(next);
        next_kind = command_kind_e::legacy_retry_tick;
      }
      wake.notify_one();
      return result;
    }

    bool
    request_now(retry_request_t expected) {
      std::lock_guard lock { mutex };
      if (stopping || expected != request || !body) return false;
      if (schedule.expedite(now())) next_kind = command_kind_e::legacy_unlock_restore;
      wake.notify_one();
      return true;
    }

    std::optional<attempt_t>
    wait_for_attempt() {
      std::unique_lock lock { mutex };
      while (!stopping) {
        if (const auto token = schedule.take_due(now())) {
          return attempt_t { request, *token, std::exchange(next_kind, command_kind_e::legacy_retry_tick), body };
        }
        if (const auto due = schedule.next_wake()) {
          wake.wait_until(lock, std::chrono::steady_clock::time_point {
                                  std::chrono::duration_cast<std::chrono::steady_clock::duration>(*due) });
        }
        else {
          wake.wait(lock);
        }
      }
      return std::nullopt;
    }

    void
    run() noexcept {
      retry_request_t reporting {};
      try {
        while (const auto attempt = wait_for_attempt()) {
          reporting = attempt->request;
          auto finished = std::make_shared<bool>(false);
          command_result_t result;
          try {
            const auto posted = coordinator.post_tracked(attempt->kind, [this, attempt = *attempt, finished](const auto &capability, const auto &context) {
              std::lock_guard owner_lock { owner_mutex };
              {
                std::lock_guard timer_lock { mutex };
                if (stopping || !schedule.is_current(attempt.token)) return;
              }
              *finished = (*attempt.body)(capability, context);
            });
            result = posted.completion.get();
          }
          catch (...) {
            result = { outcome_e::failed, reject_reason_e::none, std::current_exception() };
          }
          std::shared_ptr<const body_t> retired;
          {
            std::lock_guard lock { mutex };
            const auto completion = schedule.complete(attempt->token, result, *finished, now());
            if (completion == retry_completion_e::disarmed && request == attempt->request) retired.swap(body);
          }
          observer(reporting, result);
        }
      }
      catch (...) {
        observer(reporting, { outcome_e::failed, reject_reason_e::none, std::current_exception() });
        std::lock_guard lock { mutex };
        stopping = true;
      }
    }
  };

  retry_timer_t::retry_timer_t(coordinator_t &coordinator, std::mutex &owner_mutex, observer_t observer, monotonic_time_t delay):
      impl_ { std::make_unique<impl_t>(coordinator, owner_mutex, observer, delay) } {}
  retry_timer_t::~retry_timer_t() = default;

  retry_request_t
  retry_timer_t::setup(body_t body) { return impl_->setup(std::move(body)); }
  bool
  retry_timer_t::request_now(retry_request_t request) { return impl_->request_now(request); }
}  // namespace display_device::topology
