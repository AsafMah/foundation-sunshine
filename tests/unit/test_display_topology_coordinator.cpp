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
  TEST(DisplayTopologyCommandQueue, CompletionCallbacksCoverEagerAndExecutedOutcomesOutsideTheQueueLock) {
    fake_clock_t clock;
    topology_command_queue_t queue { [&clock]() { return clock.now(); } };
    std::vector<command_completion_t> records;
    const auto completed = [&](const command_completion_t &result) {
      EXPECT_EQ(queue.generation(), 1u);
      records.push_back(result);
    };
    auto expired = queue.submit(1, 0ms, [](const auto &) { FAIL() << "expired body ran"; }, completed);
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].status, command_status_e::expired_before_start);
    EXPECT_FALSE(records[0].started);
    EXPECT_EQ(records[0].id, expired.handle.id);
    auto stale = queue.submit(0, 100ms, [](const auto &) { FAIL() << "stale body ran"; }, completed);
    ASSERT_EQ(records.size(), 2u);
    EXPECT_EQ(records[1].status, command_status_e::stale_generation);
    EXPECT_EQ(records[1].generation, 0u);
    auto normal = queue.submit(1, 100ms, [](const auto &) {}, completed);
    ASSERT_EQ(normal.completion.wait_for(5s), std::future_status::ready);
    EXPECT_EQ(normal.completion.get().status, command_status_e::completed);
    ASSERT_EQ(records.size(), 3u);
    EXPECT_EQ(records[2].id, normal.handle.id);
    EXPECT_EQ(records[2].status, command_status_e::completed);
    EXPECT_TRUE(records[2].started);
  }

  TEST(DisplayTopologyCommandQueue, CompletionCallbackExceptionsAreReturnedThroughTheFuture) {
    fake_clock_t clock;
    topology_command_queue_t queue { [&clock]() { return clock.now(); } };
    for (const auto deadline : { 0ms, 100ms }) {
      auto submitted = queue.submit(1, deadline, [](const auto &) {}, [](const auto &) { throw std::runtime_error { "completion failed" }; });
      ASSERT_EQ(submitted.completion.wait_for(5s), std::future_status::ready);
      const auto result = submitted.completion.get();
      EXPECT_EQ(result.status, command_status_e::failed);
      ASSERT_TRUE(result.exception);
      EXPECT_THROW(std::rethrow_exception(result.exception), std::runtime_error);
    }
    EXPECT_EQ(queue.submit(1, 100ms, [](const auto &) {}).completion.get().status, command_status_e::completed);
  }

  TEST(DisplayTopologyCommandQueue, WorkerGuardBlocksFollowingBodiesUntilReleased) {
    fake_clock_t clock;
    topology_command_queue_t queue { [&clock]() { return clock.now(); } };
    std::shared_ptr<const dispatch_hold_t> hold;
    std::promise<void> guarded;
    std::promise<void> finish_mutation;
    auto finish = finish_mutation.get_future();
    std::thread::id worker;
    std::atomic_int following_bodies {};
    auto mutation = queue.submit(1, 100ms, [&](const auto &) {
      worker = std::this_thread::get_id();
      hold = queue.hold_dispatch();
      guarded.set_value();
      finish.wait();
    });
    guarded.get_future().wait();
    auto following = queue.submit(1, 100ms, [&](const auto &) {
      EXPECT_EQ(std::this_thread::get_id(), worker);
      ++following_bodies;
    });
    auto cleanup = queue.submit(1, 100ms, [&](const auto &) {
      EXPECT_EQ(std::this_thread::get_id(), worker);
      ++following_bodies;
    });
    finish_mutation.set_value();
    EXPECT_EQ(mutation.completion.get().status, command_status_e::completed);
    EXPECT_EQ(following.completion.wait_for(50ms), std::future_status::timeout)
      << "a mutation crossed the off-worker probe guard";
    EXPECT_EQ(cleanup.completion.wait_for(50ms), std::future_status::timeout)
      << "cleanup crossed the off-worker probe guard";
    EXPECT_EQ(following_bodies.load(), 0);
    hold.reset();
    EXPECT_EQ(following.completion.wait_for(2s), std::future_status::ready)
      << "releasing the final guard did not wake dispatch";
    EXPECT_EQ(following.completion.get().status, command_status_e::completed);
    EXPECT_EQ(cleanup.completion.get().status, command_status_e::completed);
    EXPECT_EQ(following_bodies.load(), 2);
  }

  TEST(DisplayTopologyCommandQueue, DispatchGuardCopiesAndIndependentHoldsCompose) {
    fake_clock_t clock;
    topology_command_queue_t queue { [&clock]() { return clock.now(); } };
    std::shared_ptr<const dispatch_hold_t> first;
    std::shared_ptr<const dispatch_hold_t> second;
    queue.submit(1, 100ms, [&](const auto &) {
           first = queue.hold_dispatch();
           second = queue.hold_dispatch();
         })
      .completion.get();
    auto copy = first;
    auto next = queue.submit(1, 100ms, [](const auto &) {});
    first.reset();
    EXPECT_EQ(next.completion.wait_for(50ms), std::future_status::timeout)
      << "a shared copy must keep its guard alive";
    copy.reset();
    EXPECT_EQ(next.completion.wait_for(50ms), std::future_status::timeout)
      << "releasing one independent guard must not release another";
    second.reset();
    EXPECT_EQ(next.completion.get().status, command_status_e::completed);
  }

  TEST(DisplayTopologyCommandQueue, DispatchGuardAllowsOwnCancellationWithoutReleasingOtherWork) {
    fake_clock_t clock;
    topology_command_queue_t queue { [&clock]() { return clock.now(); } };
    std::shared_ptr<const dispatch_hold_t> hold;
    queue.submit(1, 100ms, [&](const auto &) { hold = queue.hold_dispatch(); }).completion.get();
    std::atomic_bool cancelled_body {};
    std::atomic_bool unrelated_body {};
    auto cancelled = queue.submit(1, 100ms, [&](const auto &) { cancelled_body = true; });
    auto unrelated = queue.submit(1, 100ms, [&](const auto &) { unrelated_body = true; });
    EXPECT_TRUE(queue.cancel(cancelled.handle));
    EXPECT_FALSE(queue.cancel(cancelled.handle));
    EXPECT_EQ(cancelled.completion.wait_for(2s), std::future_status::ready)
      << "a skipped head command must settle while dispatch is held";
    EXPECT_EQ(unrelated.completion.wait_for(50ms), std::future_status::timeout);
    hold.reset();
    EXPECT_EQ(cancelled.completion.get().status, command_status_e::cancelled_before_start);
    EXPECT_EQ(unrelated.completion.get().status, command_status_e::completed);
    EXPECT_FALSE(cancelled_body.load());
    EXPECT_TRUE(unrelated_body.load());
  }

  TEST(DisplayTopologyCommandQueue, GenerationChangeDoesNotReleaseDispatchGuard) {
    fake_clock_t clock;
    topology_command_queue_t queue { [&clock]() { return clock.now(); } };
    std::shared_ptr<const dispatch_hold_t> hold;
    queue.submit(1, 100ms, [&](const auto &) { hold = queue.hold_dispatch(); }).completion.get();
    std::atomic_bool old_body {};
    auto old = queue.submit(1, 100ms, [&](const auto &) { old_body = true; });
    const auto generation = queue.begin_generation();
    auto next = queue.submit(generation, 100ms, [](const auto &) {});
    EXPECT_EQ(old.completion.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(next.completion.wait_for(50ms), std::future_status::timeout)
      << "new-generation work must not race a previous generation's probe";
    hold.reset();
    EXPECT_EQ(old.completion.get().status, command_status_e::cancelled_before_start);
    EXPECT_EQ(next.completion.get().status, command_status_e::completed);
    EXPECT_FALSE(old_body.load());
  }

  TEST(DisplayTopologyCommandQueue, StopDrainsSkippedReceiptsWhileGuardOutlivesQueue) {
    fake_clock_t clock;
    auto queue = std::make_unique<topology_command_queue_t>([&clock]() { return clock.now(); });
    std::shared_ptr<const dispatch_hold_t> hold;
    queue->submit(1, 100ms, [&](const auto &) { hold = queue->hold_dispatch(); }).completion.get();
    std::atomic_int callbacks {};
    std::atomic_bool body {};
    auto pending = queue->submit(1, 100ms, [&](const auto &) { body = true; }, [&](const auto &result) {
      EXPECT_EQ(result.status, command_status_e::cancelled_before_start);
      EXPECT_FALSE(result.started);
      ++callbacks; });
    auto stopped = std::async(std::launch::async, [&]() { queue.reset(); });
    const auto before_release = stopped.wait_for(2s);
    hold.reset();
    stopped.get();
    EXPECT_EQ(before_release, std::future_status::ready)
      << "queue shutdown must not wait for an external probe guard";
    EXPECT_EQ(pending.completion.get().status, command_status_e::cancelled_before_start);
    EXPECT_EQ(callbacks.load(), 1);
    EXPECT_FALSE(body.load());
  }

  TEST(DisplayTopologyCommandQueue, CancelledCaptureReleasesLastDispatchGuardOutsideQueueLock) {
    fake_clock_t clock;
    topology_command_queue_t queue { [&clock]() { return clock.now(); } };
    std::shared_ptr<const dispatch_hold_t> hold;
    queue.submit(1, 100ms, [&](const auto &) { hold = queue.hold_dispatch(); }).completion.get();
    auto cancelled = queue.submit(1, 100ms, [hold = std::move(hold)](const auto &) {
      FAIL() << "cancelled guard-owning body ran";
    });
    auto next = queue.submit(1, 100ms, [](const auto &) {});
    EXPECT_TRUE(queue.cancel(cancelled.handle));
    EXPECT_EQ(cancelled.completion.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(next.completion.wait_for(2s), std::future_status::ready)
      << "destroying a cancelled capture deadlocked or retained its dispatch guard";
    EXPECT_EQ(cancelled.completion.get().status, command_status_e::cancelled_before_start);
    EXPECT_EQ(next.completion.get().status, command_status_e::completed);
  }

  TEST(DisplayTopologyCommandQueue, DispatchGuardRequiresItsOwnWorker) {
    fake_clock_t clock;
    topology_command_queue_t queue { [&clock]() { return clock.now(); } };
    topology_command_queue_t other { [&clock]() { return clock.now(); } };
    EXPECT_THROW(queue.hold_dispatch(), std::logic_error);
    auto result = queue.submit(1, 100ms, [&](const auto &) {
                         EXPECT_THROW(other.hold_dispatch(), std::logic_error);
                         EXPECT_NO_THROW(queue.hold_dispatch());
                       })
                    .completion.get();
    EXPECT_EQ(result.status, command_status_e::completed);
  }

  TEST(DisplayTopologyCommandQueue, DispatchGuardSkipsExpiredHeadWithoutRunningOtherBodies) {
    fake_clock_t clock;
    topology_command_queue_t queue { [&clock]() { return clock.now(); } };
    std::shared_ptr<const dispatch_hold_t> hold;
    queue.submit(1, 100ms, [&](const auto &) { hold = queue.hold_dispatch(); }).completion.get();
    auto expired = queue.submit(1, 10ms, [](const auto &) { FAIL() << "expired body ran"; });
    clock.set(10ms);
    auto next = queue.submit(1, 100ms, [](const auto &) {});
    EXPECT_EQ(expired.completion.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(next.completion.wait_for(50ms), std::future_status::timeout);
    hold.reset();
    EXPECT_EQ(expired.completion.get().status, command_status_e::expired_before_start);
    EXPECT_EQ(next.completion.get().status, command_status_e::completed);
  }

  TEST(DisplayTopologyCommandQueue, CompletionCallbackCanReleaseDispatchGuard) {
    fake_clock_t clock;
    topology_command_queue_t queue { [&clock]() { return clock.now(); } };
    std::shared_ptr<const dispatch_hold_t> hold;
    auto guarded = queue.submit(1, 100ms, [&](const auto &) { hold = queue.hold_dispatch(); }, [&](const auto &) {
      hold.reset();
      EXPECT_EQ(queue.generation(), 1u); });
    EXPECT_EQ(guarded.completion.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(guarded.completion.get().status, command_status_e::completed);
    EXPECT_FALSE(hold) << "completion must release its guard before publishing the receipt";
    hold.reset();
    EXPECT_EQ(queue.submit(1, 100ms, [](const auto &) {}).completion.get().status, command_status_e::completed);
  }
}  // namespace
