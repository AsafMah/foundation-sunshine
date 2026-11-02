/**
 * @file tests/unit/test_video_admission.cpp
 * @brief Live admission budget: revisioned, atomic, exactly-once (provisional pending G1 hardware).
 */
#include <atomic>
#include <limits>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "src/video_admission.h"

namespace {
  using namespace std::chrono_literals;
  namespace admission = video::admission;
  using display_device::topology::error_code_e;
  using display_device::topology::monotonic_time_t;

  constexpr std::uint64_t adapter_a = 0x10;
  constexpr std::uint64_t adapter_b = 0x20;
  constexpr std::uint64_t rate_1080p60 = 1920ull * 1080 * 60;

  struct fake_clock_t {
    std::shared_ptr<std::atomic<std::int64_t>> now = std::make_shared<std::atomic<std::int64_t>>(1000);
    admission::clock_t
    clock() const {
      return [now = now] { return monotonic_time_t { now->load() }; };
    }
    void
    advance(monotonic_time_t by) {
      *now += by.count();
    }
  };

  admission::policy_t
  policy(std::uint64_t sessions, std::uint64_t pixel_rate, std::size_t history = 64) {
    return { { { adapter_a, { sessions, pixel_rate } }, { adapter_b, { sessions, pixel_rate } } }, 5000ms, history };
  }

  admission::request_t
  hd(std::uint64_t adapter = adapter_a) {
    return { adapter, 1920, 1080, 60 };
  }

  std::uint64_t
  sessions(const admission::budget_t &budget, std::uint64_t adapter = adapter_a) {
    return budget.held(admission::resource_kind_e::encode_session, adapter);
  }
}  // namespace

TEST(VideoAdmission, WithoutAPolicyAdmitsAndAccountsNothing) {
  fake_clock_t clock;
  admission::budget_t budget { std::nullopt, clock.clock() };
  EXPECT_FALSE(budget.enforced());
  for (int i = 0; i < 8; ++i) {
    auto pending = budget.reserve(hd(), 1000ms);
    ASSERT_TRUE(pending) << pending.error().detail;
    auto lease = pending->commit();
    ASSERT_TRUE(lease) << lease.error().detail;
  }
  EXPECT_EQ(budget.revision(), 0u);
  EXPECT_EQ(sessions(budget), 0u);
}

TEST(VideoAdmission, CapacityOneAdmitsExactlyOne) {
  fake_clock_t clock;
  admission::budget_t budget { policy(1, rate_1080p60 * 4), clock.clock() };
  EXPECT_TRUE(budget.enforced());
  auto first = budget.reserve(hd(), 1000ms);
  ASSERT_TRUE(first) << first.error().detail;
  auto second = budget.reserve(hd(), 1000ms);
  ASSERT_FALSE(second);
  EXPECT_EQ(second.error().code, error_code_e::resource_unavailable);
}

TEST(VideoAdmission, ConcurrentAdmissionsNeverOversubscribe) {
  fake_clock_t clock;
  admission::budget_t budget { policy(2, rate_1080p60 * 8), clock.clock() };
  std::atomic<int> admitted {};
  std::vector<admission::pending_t> held(16);
  std::vector<std::thread> threads;
  for (int i = 0; i < 16; ++i) {
    threads.emplace_back([&, i] {
      if (auto pending = budget.reserve(hd(), 1000ms)) {
        ++admitted;
        held[i] = std::move(*pending);
      }
    });
  }
  for (auto &thread : threads) thread.join();
  EXPECT_EQ(admitted.load(), 2);
  EXPECT_EQ(sessions(budget), 2u);
}

TEST(VideoAdmission, TheSessionAndPixelRateAreReservedTogetherOrNotAtAll) {
  fake_clock_t clock;
  admission::budget_t budget { policy(4, rate_1080p60), clock.clock() };
  auto first = budget.reserve(hd(), 1000ms);
  ASSERT_TRUE(first);
  const auto revision = budget.revision();
  auto second = budget.reserve(hd(), 1000ms);
  ASSERT_FALSE(second);
  EXPECT_EQ(second.error().code, error_code_e::resource_unavailable);
  EXPECT_EQ(sessions(budget), 1u);
  EXPECT_EQ(budget.held(admission::resource_kind_e::pixel_rate, adapter_a), rate_1080p60);
  EXPECT_EQ(budget.revision(), revision);
}

TEST(VideoAdmission, DisplaysOnOneAdapterShareItsBudgetButAdaptersAreIndependent) {
  fake_clock_t clock;
  admission::budget_t budget { policy(1, rate_1080p60 * 4), clock.clock() };
  auto a = budget.reserve(hd(adapter_a), 1000ms);
  ASSERT_TRUE(a);
  EXPECT_FALSE(budget.reserve(hd(adapter_a), 1000ms));
  auto b = budget.reserve(hd(adapter_b), 1000ms);
  EXPECT_TRUE(b);
  EXPECT_EQ(sessions(budget, adapter_b), 1u);
}

TEST(VideoAdmission, AnAdapterOutsideAFinitePolicyFailsClosed) {
  fake_clock_t clock;
  admission::budget_t budget { policy(4, rate_1080p60 * 4), clock.clock() };
  auto pending = budget.reserve(hd(0x30), 1000ms);
  ASSERT_FALSE(pending);
  EXPECT_EQ(pending.error().code, error_code_e::unknown_resource);
}

TEST(VideoAdmission, InvalidOrOverflowingModesAreRefused) {
  fake_clock_t clock;
  admission::budget_t budget { policy(4, ~0ull), clock.clock() };
  for (const admission::request_t request : { admission::request_t { adapter_a, 0, 1080, 60 }, admission::request_t { adapter_a, 1920, 0, 60 }, admission::request_t { adapter_a, 1920, 1080, 0 } }) {
    auto pending = budget.reserve(request, 1000ms);
    ASSERT_FALSE(pending);
    EXPECT_EQ(pending.error().code, error_code_e::invalid_profile);
  }
  auto huge = budget.reserve({ adapter_a, ~0u, ~0u, ~0u }, 1000ms);
  ASSERT_FALSE(huge);
  EXPECT_EQ(huge.error().code, error_code_e::arithmetic_overflow);
  EXPECT_EQ(sessions(budget), 0u);
}

TEST(VideoAdmission, PendingDeadlinesAreBounded) {
  fake_clock_t clock;
  admission::budget_t budget { policy(4, rate_1080p60 * 4), clock.clock() };
  auto pending = budget.reserve(hd(), 6000ms);
  ASSERT_FALSE(pending);
  EXPECT_EQ(pending.error().code, error_code_e::invalid_deadline);
}

TEST(VideoAdmission, AnExpiredPendingFreesCapacityAndCannotCommit) {
  fake_clock_t clock;
  admission::budget_t budget { policy(1, rate_1080p60 * 4), clock.clock() };
  auto stale = budget.reserve(hd(), 1000ms);
  ASSERT_TRUE(stale);
  clock.advance(1000ms);
  auto fresh = budget.reserve(hd(), 1000ms);
  ASSERT_TRUE(fresh) << fresh.error().detail;
  auto late = stale->commit();
  ASSERT_FALSE(late);
  EXPECT_EQ(late.error().code, error_code_e::expired);
  EXPECT_EQ(sessions(budget), 1u);
}

TEST(VideoAdmission, DroppingAnUncommittedPendingRollsItBack) {
  fake_clock_t clock;
  admission::budget_t budget { policy(1, rate_1080p60 * 4), clock.clock() };
  {
    auto pending = budget.reserve(hd(), 1000ms);
    ASSERT_TRUE(pending);
  }
  EXPECT_EQ(sessions(budget), 0u);
  EXPECT_TRUE(budget.reserve(hd(), 1000ms));
}

TEST(VideoAdmission, ACommittedLeaseNeverExpiresAndReleasesExactlyOnce) {
  fake_clock_t clock;
  admission::budget_t budget { policy(1, rate_1080p60 * 4), clock.clock() };
  auto pending = budget.reserve(hd(), 1000ms);
  ASSERT_TRUE(pending);
  auto lease = pending->commit();
  ASSERT_TRUE(lease);
  auto again = pending->commit();
  ASSERT_FALSE(again);
  EXPECT_EQ(again.error().code, error_code_e::already_finalized);
  clock.advance(60s);
  EXPECT_FALSE(budget.reserve(hd(), 1000ms));
  const auto before = budget.revision();
  {
    auto moved = std::move(*lease);
    *lease = admission::lease_t {};
  }
  EXPECT_EQ(budget.revision(), before + 1);
  EXPECT_EQ(sessions(budget), 0u);
  EXPECT_TRUE(budget.reserve(hd(), 1000ms));
}

TEST(VideoAdmission, ReplacingALiveSourceNeedsHeadroomForBoth) {
  fake_clock_t clock;
  admission::budget_t tight { policy(1, rate_1080p60 * 4), clock.clock() };
  auto live = tight.reserve(hd(), 1000ms)->commit();
  ASSERT_TRUE(live);
  EXPECT_FALSE(tight.reserve(hd(), 1000ms));

  admission::budget_t roomy { policy(2, rate_1080p60 * 4), clock.clock() };
  auto old_source = roomy.reserve(hd(), 1000ms)->commit();
  ASSERT_TRUE(old_source);
  auto replacement = roomy.reserve(hd(), 1000ms);
  ASSERT_TRUE(replacement);
  auto new_source = replacement->commit();
  ASSERT_TRUE(new_source);
  EXPECT_EQ(sessions(roomy), 2u);
  *old_source = admission::lease_t {};
  EXPECT_EQ(sessions(roomy), 1u);
}

TEST(VideoAdmission, TheRevisionAdvancesByExactlyOnePerChangeEvenAcrossCompaction) {
  fake_clock_t clock;
  admission::budget_t budget { policy(3, rate_1080p60 * 4, 4), clock.clock() };
  EXPECT_EQ(budget.revision(), 0u);
  auto pinned = budget.reserve(hd(), 1000ms)->commit();
  ASSERT_TRUE(pinned);
  auto kept = budget.reserve(hd(), 5000ms);
  ASSERT_TRUE(kept);
  auto last = budget.revision();
  EXPECT_EQ(last, 3u);
  for (int i = 0; i < 20; ++i) {
    auto pending = budget.reserve(hd(), 1000ms);
    ASSERT_TRUE(pending) << i << ": " << pending.error().detail;
    EXPECT_EQ(budget.revision(), ++last) << i;
    auto lease = pending->commit();
    ASSERT_TRUE(lease);
    EXPECT_EQ(budget.revision(), ++last) << i;
    *lease = admission::lease_t {};
    EXPECT_EQ(budget.revision(), ++last) << i;
  }
  auto kept_lease = kept->commit();
  EXPECT_TRUE(kept_lease);
  EXPECT_EQ(sessions(budget), 2u);
}

TEST(VideoAdmission, ALateCommitFreesTheExpiredCapacityItself) {
  fake_clock_t clock;
  admission::budget_t budget { policy(1, rate_1080p60 * 4), clock.clock() };
  auto stale = budget.reserve(hd(), 1000ms);
  ASSERT_TRUE(stale);
  clock.advance(1000ms);
  auto late = stale->commit();
  ASSERT_FALSE(late);
  EXPECT_EQ(late.error().code, error_code_e::expired);
  EXPECT_EQ(sessions(budget), 0u);
}

TEST(VideoAdmission, AClockThatStepsBackCannotStrandALease) {
  fake_clock_t clock;
  admission::budget_t budget { policy(1, rate_1080p60 * 4), clock.clock() };
  auto lease = budget.reserve(hd(), 1000ms)->commit();
  ASSERT_TRUE(lease);
  clock.advance(-500ms);
  *lease = admission::lease_t {};
  EXPECT_EQ(sessions(budget), 0u);

  auto pending = budget.reserve(hd(), 1000ms);
  ASSERT_TRUE(pending) << pending.error().detail;
  clock.advance(-500ms);
  *pending = admission::pending_t {};
  EXPECT_EQ(sessions(budget), 0u);
}

TEST(VideoAdmission, DeadlineArithmeticNeverOverflows) {
  fake_clock_t clock;
  clock.now->store(std::numeric_limits<std::int64_t>::max() - 10);
  admission::budget_t budget { policy(1, rate_1080p60 * 4), clock.clock() };
  auto pending = budget.reserve(hd(), 1000ms);
  ASSERT_FALSE(pending);
  EXPECT_EQ(pending.error().code, error_code_e::arithmetic_overflow);
  EXPECT_EQ(sessions(budget), 0u);
}

TEST(VideoAdmission, CompactionUnderConcurrentAdmissionKeepsTheBudget) {
  fake_clock_t clock;
  admission::budget_t budget { policy(3, rate_1080p60 * 3, 4), clock.clock() };
  std::atomic<int> over {};
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; ++t) {
    threads.emplace_back([&] {
      for (int i = 0; i < 200; ++i) {
        if (!budget.enforced()) ++over;
        auto pending = budget.reserve(hd(), 1000ms);
        if (!pending) continue;
        auto lease = pending->commit();
        if (sessions(budget) > 3) ++over;
      }
    });
  }
  for (auto &thread : threads) thread.join();
  EXPECT_EQ(over.load(), 0);
  EXPECT_EQ(sessions(budget), 0u);
}

TEST(VideoAdmission, BoundedHistoryCarriesLiveHoldingsAcrossCompaction) {
  fake_clock_t clock;
  admission::budget_t budget { policy(2, rate_1080p60 * 4, 4), clock.clock() };
  auto pinned = budget.reserve(hd(), 1000ms)->commit();
  ASSERT_TRUE(pinned);
  for (int i = 0; i < 20; ++i) {
    auto pending = budget.reserve(hd(), 1000ms);
    ASSERT_TRUE(pending) << i << ": " << pending.error().detail;
    EXPECT_FALSE(budget.reserve(hd(), 1000ms)) << i;
  }
  EXPECT_EQ(sessions(budget), 1u);
  *pinned = admission::lease_t {};
  EXPECT_EQ(sessions(budget), 0u);
}

TEST(VideoAdmission, APendingSurvivesCompactionAndStillCommits) {
  fake_clock_t clock;
  admission::budget_t budget { policy(4, rate_1080p60 * 8, 3), clock.clock() };
  auto kept = budget.reserve(hd(), 2000ms);
  ASSERT_TRUE(kept);
  for (int i = 0; i < 6; ++i) {
    auto churn = budget.reserve(hd(), 1000ms);
    ASSERT_TRUE(churn) << i << ": " << churn.error().detail;
  }
  auto lease = kept->commit();
  ASSERT_TRUE(lease) << lease.error().detail;
  EXPECT_EQ(sessions(budget), 1u);
}

TEST(VideoAdmission, APendingThatExpiredBeforeCompactionStillReportsExpired) {
  fake_clock_t clock;
  admission::budget_t budget { policy(4, rate_1080p60 * 8, 3), clock.clock() };
  auto stale = budget.reserve(hd(), 1000ms);
  ASSERT_TRUE(stale);
  clock.advance(1000ms);
  for (int i = 0; i < 6; ++i) {
    auto churn = budget.reserve(hd(), 1000ms);
    ASSERT_TRUE(churn) << i << ": " << churn.error().detail;
  }
  auto late = stale->commit();
  ASSERT_FALSE(late);
  EXPECT_EQ(late.error().code, error_code_e::expired);
  EXPECT_EQ(sessions(budget), 0u);
}

TEST(VideoAdmission, HandlesOutliveTheBudget) {
  fake_clock_t clock;
  std::optional<admission::pending_t> pending;
  std::optional<admission::lease_t> lease;
  {
    admission::budget_t budget { policy(2, rate_1080p60 * 4), clock.clock() };
    pending.emplace(std::move(*budget.reserve(hd(), 1000ms)));
    lease.emplace(std::move(*budget.reserve(hd(), 1000ms)->commit()));
  }
  pending.reset();
  lease.reset();
  SUCCEED();
}
