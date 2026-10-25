#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <stdexcept>
#include <vector>

#include "src/display_device/topology_command_queue.h"

namespace {
  using namespace std::chrono_literals;
  using namespace display_device::topology;

  class fake_clock_t {
  public:
    monotonic_time_t
    now() const {
      return monotonic_time_t { ticks_.load(std::memory_order_relaxed) };
    }

    void
    set(monotonic_time_t time) {
      ticks_.store(time.count(), std::memory_order_relaxed);
    }

  private:
    std::atomic<monotonic_time_t::rep> ticks_ {};
  };

  TEST(DisplayTopologyCommandQueue, ExecutesOneCommandAtATimeInSubmissionOrder) {
    fake_clock_t clock;
    topology_command_queue_t queue { [&clock]() { return clock.now(); } };
    const auto generation = queue.generation();
    std::promise<void> first_started;
    std::promise<void> release_first;
    auto release_first_future = release_first.get_future().share();
    std::mutex order_mutex;
    std::vector<int> order;
    std::atomic_int active {};
    std::atomic_int maximum_active {};

    auto first = queue.submit(generation, 100ms, [&](const command_context_t &) {
      const int current = active.fetch_add(1) + 1;
      maximum_active.store(std::max(maximum_active.load(), current));
      {
        std::lock_guard lock { order_mutex };
        order.push_back(1);
      }
      first_started.set_value();
      release_first_future.wait();
      active.fetch_sub(1);
    });
    first_started.get_future().wait();
    auto second = queue.submit(generation, 100ms, [&](const command_context_t &) {
      const int current = active.fetch_add(1) + 1;
      maximum_active.store(std::max(maximum_active.load(), current));
      {
        std::lock_guard lock { order_mutex };
        order.push_back(2);
      }
      active.fetch_sub(1);
    });

    release_first.set_value();
    EXPECT_EQ(first.completion.get().status, command_status_e::completed);
    EXPECT_EQ(second.completion.get().status, command_status_e::completed);
    EXPECT_EQ(maximum_active.load(), 1);
    EXPECT_EQ(order, (std::vector<int> { 1, 2 }));
  }

  TEST(DisplayTopologyCommandQueue, NewGenerationCancelsQueuedWorkAndFencesLateCompletion) {
    fake_clock_t clock;
    topology_command_queue_t queue { [&clock]() { return clock.now(); } };
    const auto old_generation = queue.generation();
    std::promise<void> first_started;
    std::promise<void> release_first;
    auto release_first_future = release_first.get_future().share();
    std::atomic_bool second_ran {};
    std::atomic_bool first_observed_cancel {};

    auto first = queue.submit(old_generation, 100ms, [&](const command_context_t &context) {
      first_started.set_value();
      release_first_future.wait();
      first_observed_cancel.store(context.cancellation_requested());
    });
    first_started.get_future().wait();
    auto second = queue.submit(old_generation, 100ms, [&](const command_context_t &) {
      second_ran.store(true);
    });

    const auto new_generation = queue.begin_generation();
    EXPECT_GT(new_generation, old_generation);
    EXPECT_EQ(queue.cancel(old_generation), 0u);
    release_first.set_value();

    EXPECT_EQ(first.completion.get().status, command_status_e::completed_after_cancellation);
    EXPECT_EQ(second.completion.get().status, command_status_e::cancelled_before_start);
    EXPECT_TRUE(first_observed_cancel.load());
    EXPECT_FALSE(second_ran.load());

    auto current = queue.submit(new_generation, 100ms, [](const command_context_t &) {});
    EXPECT_EQ(current.completion.get().status, command_status_e::completed);
  }

  TEST(DisplayTopologyCommandQueue, ExpiresWorkWhileItWaitsBehindAnInFlightCommand) {
    fake_clock_t clock;
    topology_command_queue_t queue { [&clock]() { return clock.now(); } };
    const auto generation = queue.generation();
    std::promise<void> first_started;
    std::promise<void> release_first;
    auto release_first_future = release_first.get_future().share();
    std::atomic_bool expired_task_ran {};

    auto first = queue.submit(generation, 100ms, [&](const command_context_t &) {
      first_started.set_value();
      release_first_future.wait();
    });
    first_started.get_future().wait();
    auto expired = queue.submit(generation, 10ms, [&](const command_context_t &) {
      expired_task_ran.store(true);
    });

    clock.set(10ms);
    release_first.set_value();

    EXPECT_EQ(first.completion.get().status, command_status_e::completed);
    EXPECT_EQ(expired.completion.get().status, command_status_e::expired_before_start);
    EXPECT_FALSE(expired_task_ran.load());
  }

  TEST(DisplayTopologyCommandQueue, ReportsExceptionsAndContinuesProcessing) {
    fake_clock_t clock;
    topology_command_queue_t queue { [&clock]() { return clock.now(); } };
    const auto generation = queue.generation();

    auto failed = queue.submit(generation, 100ms, [](const command_context_t &) {
      throw std::runtime_error { "executor failure" };
    });
    const auto failure = failed.completion.get();
    EXPECT_EQ(failure.status, command_status_e::failed);
    ASSERT_TRUE(failure.exception);
    EXPECT_THROW(std::rethrow_exception(failure.exception), std::runtime_error);

    auto next = queue.submit(generation, 100ms, [](const command_context_t &) {});
    EXPECT_EQ(next.completion.get().status, command_status_e::completed);
  }

  TEST(DisplayTopologyCommandQueue, RejectsStaleGenerationAndAlreadyExpiredSubmissions) {
    fake_clock_t clock;
    topology_command_queue_t queue { [&clock]() { return clock.now(); } };
    const auto stale_generation = queue.generation();
    const auto current_generation = queue.begin_generation();
    std::promise<void> busy_started;
    std::promise<void> release_busy;
    auto release_busy_future = release_busy.get_future().share();
    std::atomic_bool ran {};

    // Keeps the worker busy, so only the submit-time checks can complete the next two.
    auto busy = queue.submit(current_generation, 100ms, [&](const command_context_t &) {
      busy_started.set_value();
      release_busy_future.wait();
    });
    busy_started.get_future().wait();
    auto stale = queue.submit(stale_generation, 100ms, [&](const command_context_t &) {
      ran.store(true);
    });
    auto expired = queue.submit(current_generation, 0ms, [&](const command_context_t &) {
      ran.store(true);
    });
    const bool stale_ready = stale.completion.wait_for(0ms) == std::future_status::ready;
    const bool expired_ready = expired.completion.wait_for(0ms) == std::future_status::ready;
    release_busy.set_value();

    EXPECT_TRUE(stale_ready) << "a stale-generation submission waited for the worker";
    EXPECT_TRUE(expired_ready) << "an already-expired submission waited for the worker";
    EXPECT_EQ(stale.completion.get().status, command_status_e::stale_generation);
    EXPECT_EQ(expired.completion.get().status, command_status_e::expired_before_start);
    EXPECT_EQ(busy.completion.get().status, command_status_e::completed);
    EXPECT_FALSE(ran.load());
  }

  TEST(DisplayTopologyCommandQueue, CancelsOnlyTheCommandAssociatedWithOneLaunch) {
    fake_clock_t clock;
    topology_command_queue_t queue { [&clock]() { return clock.now(); } };
    const auto generation = queue.generation();
    std::promise<void> first_started;
    std::promise<void> release_first;
    auto release_first_future = release_first.get_future().share();
    std::atomic_bool cancelled_task_ran {};
    std::atomic_bool unrelated_task_ran {};

    auto first = queue.submit(generation, 100ms, [&](const command_context_t &) {
      first_started.set_value();
      release_first_future.wait();
    });
    first_started.get_future().wait();
    auto cancelled = queue.submit(generation, 100ms, [&](const command_context_t &) {
      cancelled_task_ran.store(true);
    });
    auto unrelated = queue.submit(generation, 100ms, [&](const command_context_t &) {
      unrelated_task_ran.store(true);
    });

    EXPECT_TRUE(queue.cancel(cancelled.handle));
    EXPECT_FALSE(queue.cancel(cancelled.handle));
    release_first.set_value();

    EXPECT_EQ(first.completion.get().status, command_status_e::completed);
    EXPECT_EQ(cancelled.completion.get().status, command_status_e::cancelled_before_start);
    EXPECT_EQ(unrelated.completion.get().status, command_status_e::completed);
    EXPECT_FALSE(cancelled_task_ran.load());
    EXPECT_TRUE(unrelated_task_ran.load());
  }
}  // namespace
