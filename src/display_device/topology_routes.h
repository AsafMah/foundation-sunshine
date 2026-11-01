/**
 * @file src/display_device/topology_routes.h
 * @brief Platform-neutral lifecycle helpers for coordinated production routes.
 */
#pragma once

#include <chrono>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <thread>

#include "topology_coordinator.h"

namespace display_device::topology {
  /** Shared by a mutation and its waiter; abandoned late output cannot retain a fence. */
  class probe_handoff_t {
  public:
    bool
    publish(std::shared_ptr<const probe_fence_t> fence);

    std::shared_ptr<const probe_fence_t>
    take();

    /** Enqueue rollback before dropping the returned unclaimed fence, if any. */
    std::shared_ptr<const probe_fence_t>
    abandon();

  private:
    enum class state_e { pending,
      published,
      claimed,
      abandoned };
    std::mutex mutex_;
    state_e state_ { state_e::pending };
    std::shared_ptr<const probe_fence_t> fence_;
  };

  class probe_timeout_error_t: public std::runtime_error {
  public:
    explicit probe_timeout_error_t(bool started);

    bool
    started() const noexcept;

  private:
    bool started_;
  };

  using probe_thread_factory_t = std::function<std::jthread(std::function<void()>)>;

  /**
   * Run off-worker probe work through thread exit (including TLS cleanup) under an
   * absolute deadline. The caller retains its own fence until deciding next step or
   * enqueuing rollback. timeout must terminate production nonzero without blocking.
   * A returning/throwing fake timeout hook still joins the probe before unwinding;
   * it can never turn a timeout into a successful fallback. No driver thread detaches.
   * An independent watchdog covers thread creation and the actual join acknowledgment.
   * The optional driver-thread factory must return ownership or leave no running work.
   * Calling from the fence's mutation worker is forbidden.
   */
  void
  run_supervised_probe(std::chrono::steady_clock::time_point deadline,
    std::shared_ptr<const probe_fence_t> fence, std::function<void()> probe,
    std::function<void()> timeout, probe_thread_factory_t spawn = {});

  struct retry_token_t {
    std::uint64_t generation {};

    bool
    operator==(const retry_token_t &) const = default;
  };

  enum class retry_completion_e {
    stale,
    disarmed,
    rearmed,
  };

  /**
   * Generation-tagged timer state. The caller serializes access with its timer mutex,
   * never held while posting or waiting. A posted receipt stays paired with its token;
   * the worker checks is_current() under that same mutex before doing retry work.
   */
  class retry_schedule_t {
  public:
    explicit retry_schedule_t(monotonic_time_t delay);

    void
    setup(bool enabled, monotonic_time_t now);

    std::optional<retry_token_t>
    take_due(monotonic_time_t now);

    bool
    is_current(retry_token_t token) const;

    /**
     * Failures/rejections re-arm unless shutdown made further retries impossible.
     * The caller still reports the original receipt's failure; this method only schedules.
     */
    retry_completion_e
    complete(retry_token_t token, const command_result_t &result, bool retry_finished, monotonic_time_t now);

    std::optional<monotonic_time_t>
    next_wake() const;

    /** Move a pending attempt forward, without duplicating an in-flight attempt. */
    bool
    expedite(monotonic_time_t now);

  private:
    void
    advance_generation();

    monotonic_time_t delay_;
    std::uint64_t generation_ {};
    std::optional<monotonic_time_t> due_;
    std::optional<retry_token_t> in_flight_;
  };

  struct retry_request_t {
    std::uint64_t serial {};
    bool
    operator==(const retry_request_t &) const = default;
  };

  /**
   * Owns posted retry receipts through completion, including shutdown. setup() is
   * called with owner_mutex held; request_now() is safe on a listener thread.
   * Destruction must be off both worker threads and without owner_mutex held.
   * The coordinator, owner state and mutex must outlive this object; external
   * producers such as the unlock listener must be joined before its destruction.
   */
  class retry_timer_t {
  public:
    using body_t = std::function<bool(const mutation_capability_t &, const command_context_t &)>;
    using observer_t = void (*)(retry_request_t, const command_result_t &) noexcept;

    retry_timer_t(coordinator_t &coordinator, std::mutex &owner_mutex, observer_t observer,
      monotonic_time_t delay = std::chrono::seconds { 5 });
    ~retry_timer_t();
    retry_timer_t(const retry_timer_t &) = delete;
    retry_timer_t &
    operator=(const retry_timer_t &) = delete;

    retry_request_t
    setup(body_t body);

    /** Accelerate/coalesce only this request, with unlock's narrower authority. */
    bool
    request_now(retry_request_t request);

  private:
    struct impl_t;
    std::unique_ptr<impl_t> impl_;
  };
}  // namespace display_device::topology
