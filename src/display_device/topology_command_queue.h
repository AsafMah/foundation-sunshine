#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>

#include "topology.h"

namespace display_device::topology {

  enum class command_status_e {
    completed,
    completed_after_cancellation,
    completed_after_deadline,
    cancelled_before_start,
    expired_before_start,
    stale_generation,
    queue_stopped,
    failed
  };

  struct command_completion_t {
    std::uint64_t id {};
    std::uint64_t generation {};
    command_status_e status { command_status_e::completed };
    bool started {};
    std::exception_ptr exception;
  };

  struct command_handle_t {
    std::uint64_t id {};
    std::uint64_t generation {};
    bool
    operator==(const command_handle_t &) const = default;
  };

  struct command_submission_t {
    command_handle_t handle;
    std::future<command_completion_t> completion;
  };

  class command_context_t {
  public:
    bool
    cancellation_requested() const;

    bool
    deadline_expired() const;

    bool
    may_continue() const;

    monotonic_time_t
    deadline() const;

  private:
    friend class topology_command_queue_t;

    command_context_t(std::shared_ptr<std::atomic_bool> cancelled,
      monotonic_time_t deadline, std::function<monotonic_time_t()> now);

    std::shared_ptr<std::atomic_bool> cancelled_;
    monotonic_time_t deadline_;
    std::function<monotonic_time_t()> now_;
  };

  struct dispatch_state_t;

  /** Keeps dispatch paused until the last shared holder releases it, even after shutdown. */
  class dispatch_hold_t {
  public:
    ~dispatch_hold_t();
    dispatch_hold_t(const dispatch_hold_t &) = delete;
    dispatch_hold_t &
    operator=(const dispatch_hold_t &) = delete;

  private:
    friend class topology_command_queue_t;
    explicit dispatch_hold_t(std::shared_ptr<dispatch_state_t> state);
    std::shared_ptr<dispatch_state_t> state_;
  };

  // One worker owns command execution. State locks are released before invoking
  // commands; an in-flight command remains the serialization fence until return.
  class topology_command_queue_t {
  public:
    using clock_function_t = std::function<monotonic_time_t()>;
    using task_t = std::function<void(const command_context_t &)>;
    using completion_function_t = std::function<void(const command_completion_t &)>;

    explicit topology_command_queue_t(clock_function_t now = {});
    ~topology_command_queue_t();

    topology_command_queue_t(const topology_command_queue_t &) = delete;
    topology_command_queue_t &
    operator=(const topology_command_queue_t &) = delete;

    std::uint64_t
    generation() const;

    std::uint64_t
    begin_generation();

    std::size_t
    cancel(std::uint64_t generation);

    bool
    cancel(command_handle_t handle);

    /**
     * The optional completion callback runs exactly once, outside the queue lock, before
     * the future becomes ready, even if the task never starts. Eager rejection calls it
     * on the submitter; otherwise it runs on the worker. A throwing callback is reported
     * as a failed completion through the future.
     */
    command_submission_t
    submit(std::uint64_t generation, monotonic_time_t deadline, task_t task, completion_function_t completed = {});

    /**
     * Pause subsequent command bodies before this worker command completes. Independent
     * holds compose and survive generation changes. Skipped head commands still complete
     * FIFO; cancellation behind a live held head waits for it, as behind a running body.
     * Shutdown cancels pending commands without waiting for holds. Holders must not await
     * queued work, and must keep the token through all off-worker probe teardown.
     * @throws std::logic_error unless called on this queue's worker.
     */
    std::shared_ptr<const dispatch_hold_t>
    hold_dispatch();

  private:
    friend class coordinator_t;

    /** As submit(), but ahead of every pending command (still behind a running body). */
    command_submission_t
    submit_first(std::uint64_t generation, monotonic_time_t deadline, task_t task, completion_function_t completed = {});

    command_submission_t
    enqueue(std::uint64_t generation, monotonic_time_t deadline, task_t task, completion_function_t completed, bool front);

    struct command_t {
      std::uint64_t id {};
      std::uint64_t generation {};
      monotonic_time_t deadline {};
      task_t task;
      completion_function_t completed;
      std::shared_ptr<std::atomic_bool> cancelled;
      std::promise<command_completion_t> completion;
    };

    static monotonic_time_t
    steady_now();

    void
    run();

    static void
    complete(const std::shared_ptr<command_t> &command, command_completion_t completion);

    clock_function_t now_;
    std::shared_ptr<dispatch_state_t> dispatch_;
    std::deque<std::shared_ptr<command_t>> pending_;
    std::shared_ptr<command_t> active_;
    std::uint64_t generation_ { 1 };
    std::uint64_t next_id_ { 1 };
    bool stopping_ {};
    std::thread worker_;
  };

}  // namespace display_device::topology
