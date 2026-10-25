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

  // One worker owns command execution. State locks are released before invoking
  // commands; an in-flight command remains the serialization fence until return.
  class topology_command_queue_t {
  public:
    using clock_function_t = std::function<monotonic_time_t()>;
    using task_t = std::function<void(const command_context_t &)>;

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

    command_submission_t
    submit(std::uint64_t generation, monotonic_time_t deadline, task_t task);

  private:
    struct command_t {
      std::uint64_t id {};
      std::uint64_t generation {};
      monotonic_time_t deadline {};
      task_t task;
      std::shared_ptr<std::atomic_bool> cancelled;
      std::promise<command_completion_t> completion;
    };

    static monotonic_time_t
    steady_now();

    void
    run();

    clock_function_t now_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::shared_ptr<command_t>> pending_;
    std::shared_ptr<command_t> active_;
    std::uint64_t generation_ { 1 };
    std::uint64_t next_id_ { 1 };
    bool stopping_ {};
    std::thread worker_;
  };

}  // namespace display_device::topology
