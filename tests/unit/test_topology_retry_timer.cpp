#include "src/display_device/topology_routes.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <gtest/gtest.h>

namespace {
  using namespace display_device::topology;
  using namespace std::chrono_literals;

  struct events_t {
    std::mutex mutex;
    std::condition_variable wake;
    std::array<command_result_t, 64> results;
    std::size_t count {};

    bool
    wait(std::size_t target) {
      std::unique_lock lock { mutex };
      return wake.wait_for(lock, 2s, [&] { return count >= target; });
    }

    command_result_t
    result(std::size_t index) {
      std::lock_guard lock { mutex };
      return results.at(index);
    }
  };

  events_t *observed {};
  void
  observe(retry_request_t, const command_result_t &result) noexcept {
    std::lock_guard lock { observed->mutex };
    if (observed->count < observed->results.size()) {
      observed->results[observed->count++] = result;
    }
    observed->wake.notify_all();
  }

  class TopologyRetryTimer: public ::testing::Test {
  protected:
    void
    SetUp() override { observed = &events; }
    events_t events;
  };

  struct gate_t {
    std::promise<void> entered, released;
    std::shared_future<void> ready { entered.get_future().share() };
    std::shared_future<void> resume { released.get_future().share() };
    std::once_flag once;
    void
    block() {
      entered.set_value();
      resume.wait();
    }
    bool
    started() { return ready.wait_for(2s) == std::future_status::ready; }
    void
    release() {
      std::call_once(once, [&] { released.set_value(); });
    }
    ~gate_t() { release(); }
  };

  bool
  submitted(coordinator_t &coordinator, std::uint64_t count) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    do {
      if (coordinator.status().submitted_commands >= count) return true;
      std::this_thread::yield();
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
  }

  TEST_F(TopologyRetryTimer, PollingUsesWorkerAuthorityAndRearmsUntilFinished) {
    coordinator_t coordinator;
    ASSERT_TRUE(coordinator.start({}).get().completed());
    std::mutex owner;
    int calls {};
    retry_timer_t timer { coordinator, owner, observe, 1ms };
    {
      std::lock_guard lock { owner };
      timer.setup([&](const auto &capability, const auto &) {
        EXPECT_TRUE(coordinator.on_worker_thread());
        EXPECT_EQ(capability.kind(), command_kind_e::legacy_retry_tick);
        EXPECT_NO_THROW(capability.require(operation_e::configure_display));
        EXPECT_THROW(capability.require(operation_e::vdd_create), capability_error_t);
        return ++calls == 2;
      });
    }
    ASSERT_TRUE(events.wait(2));
    std::lock_guard lock { owner };
    EXPECT_EQ(calls, 2);
    EXPECT_TRUE(events.result(0).completed());
    EXPECT_TRUE(events.result(1).completed());
  }

  TEST_F(TopologyRetryTimer, QueuedReplacementAndCancellationDiscardOldBodies) {
    for (bool replace : { false, true }) {
      SCOPED_TRACE(replace);
      coordinator_t coordinator;
      ASSERT_TRUE(coordinator.start({}).get().completed());
      std::mutex owner;
      int old_calls {}, new_calls {};
      retry_timer_t timer { coordinator, owner, observe, 1h };
      gate_t blocker;
      const auto blocked = coordinator.post_tracked(command_kind_e::rotate, [&](const auto &, const auto &) { blocker.block(); });
      ASSERT_TRUE(blocker.started());
      const auto baseline = coordinator.status().submitted_commands;
      retry_request_t old, newer;
      {
        std::lock_guard lock { owner };
        old = timer.setup([&](const auto &, const auto &) { ++old_calls; return true; });
      }
      ASSERT_TRUE(timer.request_now(old));
      ASSERT_TRUE(submitted(coordinator, baseline + 1));
      {
        std::lock_guard lock { owner };
        newer = timer.setup(replace ? retry_timer_t::body_t { [&](const auto &, const auto &) { ++new_calls; return true; } } : nullptr);
      }
      EXPECT_FALSE(timer.request_now(old));
      if (replace) EXPECT_TRUE(timer.request_now(newer));
      blocker.release();
      EXPECT_TRUE(blocked.completion.get().completed());
      const std::size_t target = replace ? 3 : 1;
      ASSERT_TRUE(events.wait(target));
      std::lock_guard lock { owner };
      EXPECT_EQ(old_calls, 0);
      EXPECT_EQ(new_calls, replace ? 1 : 0);
    }
  }

  TEST_F(TopologyRetryTimer, UnlockIdentitySurvivesAttemptsButNotReplacement) {
    coordinator_t coordinator;
    ASSERT_TRUE(coordinator.start({}).get().completed());
    std::mutex owner;
    int calls {};
    retry_timer_t timer { coordinator, owner, observe, 1h };
    retry_request_t request;
    {
      std::lock_guard lock { owner };
      request = timer.setup([&](const auto &capability, const auto &) {
        EXPECT_EQ(capability.kind(), command_kind_e::legacy_unlock_restore);
        EXPECT_NO_THROW(capability.require(operation_e::restore_state));
        EXPECT_THROW(capability.require(operation_e::configure_display), capability_error_t);
        return ++calls == 2;
      });
    }
    ASSERT_TRUE(timer.request_now(request));
    ASSERT_TRUE(events.wait(1));
    ASSERT_TRUE(timer.request_now(request)) << "attempt generation must not invalidate the request";
    ASSERT_TRUE(events.wait(2));
    EXPECT_FALSE(timer.request_now(request)) << "completed work must not be resurrected";
    {
      std::lock_guard lock { owner };
      const auto replacement = timer.setup([](const auto &, const auto &) { return true; });
      EXPECT_NE(replacement, request);
    }
    EXPECT_FALSE(timer.request_now(request));
    std::lock_guard lock { owner };
    EXPECT_EQ(calls, 2);
  }

  TEST_F(TopologyRetryTimer, BodyCanReplaceItselfWithoutStaleCompletionDisarmingNewWork) {
    coordinator_t coordinator;
    ASSERT_TRUE(coordinator.start({}).get().completed());
    std::mutex owner;
    int replacements {};
    retry_timer_t timer { coordinator, owner, observe, 1ms };
    {
      std::lock_guard lock { owner };
      timer.setup([&](const auto &, const auto &) {
        timer.setup([&](const auto &, const auto &) { ++replacements; return true; });
        return true;
      });
    }
    ASSERT_TRUE(events.wait(2));
    std::lock_guard lock { owner };
    EXPECT_EQ(replacements, 1);
  }

  TEST_F(TopologyRetryTimer, RejectedWorkAndThrowingCoordinatorObserverAreReportedAndRetried) {
    std::atomic_int observer_calls {};
    coordinator_options_t options;
    options.posted_observer = [&](auto kind, const auto &) {
      if (kind == command_kind_e::legacy_retry_tick && observer_calls.fetch_add(1) == 0) {
        throw std::runtime_error { "completion observer failed" };
      }
    };
    coordinator_t coordinator { options };
    std::mutex owner;
    int calls {};
    retry_timer_t timer { coordinator, owner, observe, 1ms };
    {
      std::lock_guard lock { owner };
      timer.setup([&](const auto &, const auto &) { ++calls; return true; });
    }
    ASSERT_TRUE(events.wait(1));
    EXPECT_EQ(events.result(0).reason, reject_reason_e::not_started);
    ASSERT_TRUE(coordinator.start({}).get().completed());
    {
      std::unique_lock lock { events.mutex };
      ASSERT_TRUE(events.wake.wait_for(lock, 2s, [&] {
        return events.count && events.results[events.count - 1].completed();
      }));
      EXPECT_TRUE(std::any_of(events.results.begin(), events.results.begin() + events.count,
        [](const auto &result) { return result.outcome == outcome_e::failed && result.exception; }));
    }
    std::lock_guard lock { owner };
    EXPECT_EQ(calls, 2);
  }

  TEST_F(TopologyRetryTimer, OwnerMutexCoversTheMutationNotJustItsGenerationCheck) {
    coordinator_t coordinator;
    ASSERT_TRUE(coordinator.start({}).get().completed());
    std::mutex owner;
    retry_timer_t timer { coordinator, owner, observe, 1h };
    std::promise<void> called;
    auto ready = called.get_future();
    const auto baseline = coordinator.status().submitted_commands;
    std::unique_lock lock { owner };
    const auto request = timer.setup([&](const auto &, const auto &) {
      called.set_value();
      return true;
    });
    ASSERT_TRUE(timer.request_now(request));
    ASSERT_TRUE(submitted(coordinator, baseline + 1));
    EXPECT_EQ(ready.wait_for(50ms), std::future_status::timeout);
    lock.unlock();
    ASSERT_EQ(ready.wait_for(2s), std::future_status::ready);
    EXPECT_TRUE(events.wait(1));
  }

  TEST_F(TopologyRetryTimer, DestructionSettlesQueuedAndRunningReceiptsBeforeOwnerDies) {
    for (bool running : { false, true }) {
      SCOPED_TRACE(running);
      coordinator_t coordinator;
      ASSERT_TRUE(coordinator.start({}).get().completed());
      std::mutex owner;
      std::atomic_int calls {};
      auto timer = std::make_unique<retry_timer_t>(coordinator, owner, observe, 1h);
      gate_t blocker;
      if (!running) coordinator.post_tracked(command_kind_e::rotate, [&](const auto &, const auto &) { blocker.block(); });
      if (!running) ASSERT_TRUE(blocker.started());
      const auto baseline = coordinator.status().submitted_commands;
      retry_request_t request;
      {
        std::lock_guard lock { owner };
        request = timer->setup([&](const auto &, const auto &) {
          ++calls;
          if (running) blocker.block();
          return true;
        });
      }
      ASSERT_TRUE(timer->request_now(request));
      ASSERT_TRUE(submitted(coordinator, baseline + 1));
      if (running) ASSERT_TRUE(blocker.started());
      std::promise<void> destroying;
      auto started = destroying.get_future();
      auto destruction = std::async(std::launch::async, [&] { destroying.set_value(); timer.reset(); });
      started.wait();
      EXPECT_EQ(destruction.wait_for(50ms), std::future_status::timeout);
      blocker.release();
      EXPECT_EQ(destruction.wait_for(2s), std::future_status::ready);
      destruction.get();
      EXPECT_EQ(calls.load(), running ? 1 : 0);
    }
  }
}  // namespace
