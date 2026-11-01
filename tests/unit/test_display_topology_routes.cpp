#include <gtest/gtest.h>

#include <atomic>
#include <future>
#include <stdexcept>

#include "src/display_device/topology_routes.h"

namespace {
  using namespace std::chrono_literals;
  using namespace display_device::topology;

  TEST(DisplayTopologyRoutes, RetryScheduleWaitsForItsDeadlineAndCompletion) {
    retry_schedule_t schedule { 10ms };
    EXPECT_FALSE(schedule.take_due(100ms));
    schedule.setup(true, 100ms);
    EXPECT_EQ(schedule.next_wake(), 110ms);
    EXPECT_FALSE(schedule.take_due(109ms));
    const auto token = schedule.take_due(110ms);
    ASSERT_TRUE(token);
    EXPECT_TRUE(schedule.is_current(*token));
    EXPECT_FALSE(schedule.take_due(1000ms)) << "a retry is already in flight";
    EXPECT_EQ(schedule.complete(*token, {}, false, 120ms), retry_completion_e::rearmed);
    EXPECT_EQ(schedule.next_wake(), 130ms);
    const auto retry = schedule.take_due(130ms);
    ASSERT_TRUE(retry);
    EXPECT_EQ(schedule.complete(*retry, {}, true, 140ms), retry_completion_e::disarmed);
    EXPECT_FALSE(schedule.next_wake());
    EXPECT_FALSE(schedule.is_current(*retry));
  }

  TEST(DisplayTopologyRoutes, ReplacingOrDisablingARetryInvalidatesItsOldCompletion) {
    retry_schedule_t schedule { 10ms };
    schedule.setup(true, 0ms);
    const auto old = schedule.take_due(10ms);
    ASSERT_TRUE(old);
    schedule.setup(true, 20ms);
    EXPECT_FALSE(schedule.is_current(*old));
    EXPECT_EQ(schedule.complete(*old, {}, true, 25ms), retry_completion_e::stale);
    EXPECT_EQ(schedule.next_wake(), 30ms);
    const auto current = schedule.take_due(30ms);
    ASSERT_TRUE(current);
    schedule.setup(false, 40ms);
    EXPECT_EQ(schedule.complete(*current, {}, false, 50ms), retry_completion_e::stale);
    EXPECT_FALSE(schedule.next_wake()) << "a stale completion must not restart a disabled timer";
  }

  TEST(DisplayTopologyRoutes, RepeatedAttemptsCannotConsumeEachOthersCompletion) {
    retry_schedule_t schedule { 10ms };
    schedule.setup(true, 0ms);
    const auto first = schedule.take_due(10ms);
    ASSERT_TRUE(first);
    ASSERT_EQ(schedule.complete(*first, {}, false, 10ms), retry_completion_e::rearmed);
    const auto second = schedule.take_due(20ms);
    ASSERT_TRUE(second);
    EXPECT_NE(*first, *second);
    EXPECT_EQ(schedule.complete(*first, {}, true, 21ms), retry_completion_e::stale);
    EXPECT_TRUE(schedule.is_current(*second));
    EXPECT_EQ(schedule.complete(*second, {}, true, 22ms), retry_completion_e::disarmed);
  }

  TEST(DisplayTopologyRoutes, RetryFailuresRearmButShutdownOutcomesDisarm) {
    for (const auto outcome : { outcome_e::completed_late, outcome_e::cancelled, outcome_e::failed, outcome_e::expired_before_start }) {
      retry_schedule_t schedule { 10ms };
      schedule.setup(true, 0ms);
      const auto token = schedule.take_due(10ms);
      ASSERT_TRUE(token);
      EXPECT_EQ(schedule.complete(*token, { outcome }, true, 20ms), retry_completion_e::rearmed);
      EXPECT_EQ(schedule.next_wake(), 30ms);
    }
    for (const auto reason : { reject_reason_e::owner_busy, reject_reason_e::managed_session, reject_reason_e::fence_active,
           reject_reason_e::draining, reject_reason_e::stopped }) {
      retry_schedule_t schedule { 10ms };
      schedule.setup(true, 0ms);
      const auto token = schedule.take_due(10ms);
      ASSERT_TRUE(token);
      const bool stopped = reason == reject_reason_e::draining || reason == reject_reason_e::stopped;
      EXPECT_EQ(schedule.complete(*token, { outcome_e::rejected, reason }, false, 20ms),
        stopped ? retry_completion_e::disarmed : retry_completion_e::rearmed);
    }
    retry_schedule_t schedule { 10ms };
    schedule.setup(true, 0ms);
    const auto token = schedule.take_due(10ms);
    ASSERT_TRUE(token);
    EXPECT_EQ(schedule.complete(*token, { outcome_e::stopped }, false, 20ms), retry_completion_e::disarmed);
    EXPECT_FALSE(schedule.next_wake());
  }

  TEST(DisplayTopologyRoutes, WorkerRejectedTickReceiptRearmsItsOwnGeneration) {
    coordinator_t coordinator;
    ASSERT_TRUE(coordinator.start({}).get().completed());
    std::promise<void> entered, released;
    auto entered_future = entered.get_future();
    auto released_future = released.get_future().share();
    const auto blocker = coordinator.post_tracked(command_kind_e::rotate, [&](const auto &, const auto &) {
      entered.set_value();
      released_future.wait();
    });
    const bool started = entered_future.wait_for(2s) == std::future_status::ready;
    if (!started) {
      released.set_value();
      FAIL() << "worker did not start";
    }
    retry_schedule_t schedule { 10ms };
    schedule.setup(true, 0ms);
    const auto token = schedule.take_due(10ms);
    // take_due cannot fail here; release the worker before any fatal assertion.
    std::atomic_int calls {};
    const auto receipt = coordinator.post_tracked(command_kind_e::legacy_retry_tick, [&](const auto &, const auto &) { ++calls; });
    auto fence = coordinator.hold_fence();
    released.set_value();
    ASSERT_TRUE(token);
    ASSERT_EQ(receipt.reason, reject_reason_e::none);
    ASSERT_EQ(receipt.completion.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(receipt.completion.get().reason, reject_reason_e::fence_active);
    EXPECT_EQ(calls, 0);
    EXPECT_EQ(schedule.complete(*token, receipt.completion.get(), false, 20ms), retry_completion_e::rearmed);
    const auto retry = schedule.take_due(30ms);
    ASSERT_TRUE(retry);
    EXPECT_NE(*retry, *token);
    fence.reset();
    const auto retried = coordinator.post_tracked(command_kind_e::legacy_retry_tick, [&](const auto &, const auto &) { ++calls; });
    ASSERT_EQ(retried.completion.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(schedule.complete(*retry, retried.completion.get(), true, 40ms), retry_completion_e::disarmed);
    EXPECT_EQ(calls, 1);
  }

  TEST(DisplayTopologyRoutes, RetryDelayMustBePositive) {
    EXPECT_THROW(retry_schedule_t { 0ms }, std::invalid_argument);
    EXPECT_THROW(retry_schedule_t { -1ms }, std::invalid_argument);
  }
}  // namespace
