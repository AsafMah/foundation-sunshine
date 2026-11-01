#include "topology_command_queue.h"

#include <chrono>
#include <limits>
#include <stdexcept>
#include <utility>

namespace display_device::topology {

  struct dispatch_state_t {
    std::mutex mutex;
    std::condition_variable wake;
    std::size_t holds {};
    std::thread::id worker;
  };

  dispatch_hold_t::dispatch_hold_t(std::shared_ptr<dispatch_state_t> state):
      state_ { std::move(state) } {
    std::lock_guard lock { state_->mutex };
    ++state_->holds;
  }

  dispatch_hold_t::~dispatch_hold_t() {
    {
      std::lock_guard lock { state_->mutex };
      --state_->holds;
    }
    state_->wake.notify_one();
  }

  command_context_t::command_context_t(std::shared_ptr<std::atomic_bool> cancelled,
    monotonic_time_t deadline, std::function<monotonic_time_t()> now):
      cancelled_ { std::move(cancelled) },
      deadline_ { deadline },
      now_ { std::move(now) } {}

  bool
  command_context_t::cancellation_requested() const {
    return cancelled_->load(std::memory_order_acquire);
  }

  bool
  command_context_t::deadline_expired() const {
    return now_() >= deadline_;
  }

  bool
  command_context_t::may_continue() const {
    return !cancellation_requested() && !deadline_expired();
  }

  monotonic_time_t
  command_context_t::deadline() const {
    return deadline_;
  }

  topology_command_queue_t::topology_command_queue_t(clock_function_t now):
      now_ { now ? std::move(now) : &topology_command_queue_t::steady_now },
      dispatch_ { std::make_shared<dispatch_state_t>() },
      worker_ { [this]() { run(); } } {}

  topology_command_queue_t::~topology_command_queue_t() {
    {
      std::lock_guard lock { dispatch_->mutex };
      stopping_ = true;
      if (active_) active_->cancelled->store(true, std::memory_order_release);
      for (const auto &command : pending_) {
        command->cancelled->store(true, std::memory_order_release);
      }
    }
    dispatch_->wake.notify_one();
    worker_.join();
  }

  std::uint64_t
  topology_command_queue_t::generation() const {
    std::lock_guard lock { dispatch_->mutex };
    return generation_;
  }

  std::uint64_t
  topology_command_queue_t::begin_generation() {
    std::lock_guard lock { dispatch_->mutex };
    if (generation_ == std::numeric_limits<std::uint64_t>::max()) {
      throw std::overflow_error { "Topology command generation exhausted" };
    }
    ++generation_;
    if (active_) active_->cancelled->store(true, std::memory_order_release);
    for (const auto &command : pending_) {
      command->cancelled->store(true, std::memory_order_release);
    }
    dispatch_->wake.notify_one();
    return generation_;
  }

  std::size_t
  topology_command_queue_t::cancel(std::uint64_t generation) {
    std::lock_guard lock { dispatch_->mutex };
    std::size_t cancelled {};
    if (active_ && active_->generation == generation &&
        !active_->cancelled->exchange(true, std::memory_order_acq_rel)) {
      ++cancelled;
    }
    for (const auto &command : pending_) {
      if (command->generation == generation &&
          !command->cancelled->exchange(true, std::memory_order_acq_rel)) {
        ++cancelled;
      }
    }
    dispatch_->wake.notify_one();
    return cancelled;
  }

  bool
  topology_command_queue_t::cancel(command_handle_t handle) {
    std::lock_guard lock { dispatch_->mutex };
    const auto cancel_command = [handle](const std::shared_ptr<command_t> &command) {
      return command->id == handle.id && command->generation == handle.generation &&
             !command->cancelled->exchange(true, std::memory_order_acq_rel);
    };
    bool cancelled = active_ && cancel_command(active_);
    if (!cancelled) {
      for (const auto &command : pending_) {
        if (cancel_command(command)) {
          cancelled = true;
          break;
        }
      }
    }
    if (cancelled) dispatch_->wake.notify_one();
    return cancelled;
  }

  command_submission_t
  topology_command_queue_t::submit(std::uint64_t generation, monotonic_time_t deadline, task_t task, completion_function_t completed) {
    auto command = std::make_shared<command_t>();
    command->generation = generation;
    command->deadline = deadline;
    command->task = std::move(task);
    command->completed = std::move(completed);
    command->cancelled = std::make_shared<std::atomic_bool>(false);
    auto future = command->completion.get_future();
    command_submission_t submission { { command->id, generation }, std::move(future) };

    command_status_e status { command_status_e::completed };
    {
      std::lock_guard lock { dispatch_->mutex };
      command->id = next_id_++;
      submission.handle.id = command->id;
      if (stopping_) {
        status = command_status_e::queue_stopped;
      }
      else if (generation != generation_) {
        status = command_status_e::stale_generation;
      }
      else if (now_() >= deadline) {
        status = command_status_e::expired_before_start;
      }
      else {
        pending_.push_back(command);
      }
    }
    if (status != command_status_e::completed) {
      complete(command, { command->id, generation, status, false, {} });
      return submission;
    }
    dispatch_->wake.notify_one();
    return submission;
  }

  std::shared_ptr<const dispatch_hold_t>
  topology_command_queue_t::hold_dispatch() {
    {
      std::lock_guard lock { dispatch_->mutex };
      if (dispatch_->worker != std::this_thread::get_id()) {
        throw std::logic_error { "Dispatch guards must be acquired on their queue worker" };
      }
    }
    return std::shared_ptr<const dispatch_hold_t> { new dispatch_hold_t { dispatch_ } };
  }

  void
  topology_command_queue_t::complete(const std::shared_ptr<command_t> &command, command_completion_t completion) {
    if (command->completed) {
      try {
        command->completed(completion);
      }
      catch (...) {
        completion.status = command_status_e::failed;
        completion.exception = std::current_exception();
      }
    }
    command->completion.set_value(std::move(completion));
  }

  monotonic_time_t
  topology_command_queue_t::steady_now() {
    return std::chrono::duration_cast<monotonic_time_t>(
      std::chrono::steady_clock::now().time_since_epoch());
  }

  void
  topology_command_queue_t::run() {
    {
      std::lock_guard lock { dispatch_->mutex };
      dispatch_->worker = std::this_thread::get_id();
    }
    while (true) {
      std::shared_ptr<command_t> command;
      command_status_e preflight_status { command_status_e::completed };
      bool should_run {};

      {
        std::unique_lock lock { dispatch_->mutex };
        dispatch_->wake.wait(lock, [this]() {
          if (stopping_) return true;
          if (pending_.empty()) return false;
          const auto &head = pending_.front();
          return dispatch_->holds == 0 || head->cancelled->load(std::memory_order_acquire) ||
                 head->generation != generation_ || now_() >= head->deadline;
        });
        if (pending_.empty()) {
          if (stopping_) return;
          continue;
        }
        command = std::move(pending_.front());
        pending_.pop_front();
        active_ = command;

        if (stopping_ || command->cancelled->load(std::memory_order_acquire)) {
          preflight_status = command_status_e::cancelled_before_start;
        }
        else if (command->generation != generation_) {
          preflight_status = command_status_e::stale_generation;
        }
        else if (now_() >= command->deadline) {
          preflight_status = command_status_e::expired_before_start;
        }
        else {
          should_run = true;
        }
      }

      command_completion_t completion { command->id, command->generation, preflight_status, should_run, {} };
      if (should_run) {
        const command_context_t context { command->cancelled, command->deadline, now_ };
        try {
          command->task(context);
          if (context.cancellation_requested()) {
            completion.status = command_status_e::completed_after_cancellation;
          }
          else if (context.deadline_expired()) {
            completion.status = command_status_e::completed_after_deadline;
          }
          else {
            completion.status = command_status_e::completed;
          }
        }
        catch (...) {
          completion.status = command_status_e::failed;
          completion.exception = std::current_exception();
        }
      }

      {
        std::lock_guard lock { dispatch_->mutex };
        if (active_ == command) active_.reset();
      }
      complete(command, std::move(completion));
      dispatch_->wake.notify_one();
    }
  }

}  // namespace display_device::topology
