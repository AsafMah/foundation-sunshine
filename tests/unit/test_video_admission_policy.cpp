/**
 * @file tests/unit/test_video_admission_policy.cpp
 * @brief Production live admission policy and adapter learning (provisional pending G1 hardware).
 */
#include <atomic>
#include <limits>
#include <memory>

#include <gtest/gtest.h>

#include "src/video_admission_policy.h"

namespace {
  using namespace std::chrono_literals;
  namespace admission = video::admission;
  using display_device::topology::error_code_e;
  using display_device::topology::monotonic_time_t;

  constexpr std::uint64_t adapter_a = 0x10;
  constexpr std::uint64_t adapter_b = 0x20;

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

  admission::request_t
  hd(std::uint64_t adapter = adapter_a) {
    return { adapter, 1920, 1080, 60 };
  }

  std::uint64_t
  sessions(const admission::budget_t &budget, std::uint64_t adapter = adapter_a) {
    return budget.held(admission::resource_kind_e::encode_session, adapter);
  }

  admission::budget_t
  managed_budget(const fake_clock_t &clock) {
    return admission::budget_t { admission::production_policy(true), clock.clock() };
  }
}  // namespace

TEST(VideoAdmissionPolicy, TheBlankProfileGetsNoBudget) {
  EXPECT_FALSE(admission::production_policy(false));
  fake_clock_t clock;
  admission::budget_t budget { admission::production_policy(false), clock.clock() };
  EXPECT_FALSE(budget.enforced());
  EXPECT_FALSE(*budget.ensure_adapter(adapter_a));
  EXPECT_FALSE(*budget.learn_adapter(adapter_a, { 1, 1 }));
  auto pending = budget.reserve(hd(), budget.max_pending());
  ASSERT_TRUE(pending);
  EXPECT_TRUE(pending->commit());
}

TEST(VideoAdmissionPolicy, AManagedBudgetStartsWithNoAdapters) {
  const auto policy = admission::production_policy(true);
  ASSERT_TRUE(policy);
  EXPECT_TRUE(policy->adapters.empty());
  ASSERT_TRUE(policy->fallback);
  EXPECT_EQ(policy->fallback->encode_sessions, admission::fallback_encode_sessions);
  EXPECT_EQ(policy->fallback->pixel_rate, admission::fallback_pixel_rate);
  EXPECT_EQ(policy->max_pending, admission::production_max_pending);

  fake_clock_t clock;
  auto budget = managed_budget(clock);
  EXPECT_TRUE(budget.enforced());
  EXPECT_FALSE(budget.limits(adapter_a));
  // The fallback is only given through ensure_adapter: reserve never admits an unknown adapter.
  auto pending = budget.reserve(hd(), budget.max_pending());
  ASSERT_FALSE(pending);
  EXPECT_EQ(pending.error().code, error_code_e::unknown_resource);
}

TEST(VideoAdmissionPolicy, ReportedCapacityBecomesTheLimits) {
  const auto derived = admission::derive_limits({ 2, 1000 });
  EXPECT_EQ(derived.limits.encode_sessions, 2u);
  EXPECT_EQ(derived.limits.pixel_rate, 256'000u);
  EXPECT_TRUE(derived.sessions_reported);
  EXPECT_TRUE(derived.pixel_rate_reported);
}

TEST(VideoAdmissionPolicy, UnreportedCapacityFallsBack) {
  const auto derived = admission::derive_limits({});
  EXPECT_EQ(derived.limits.encode_sessions, admission::fallback_encode_sessions);
  EXPECT_EQ(derived.limits.pixel_rate, admission::fallback_pixel_rate);
  EXPECT_FALSE(derived.sessions_reported);
  EXPECT_FALSE(derived.pixel_rate_reported);
}

TEST(VideoAdmissionPolicy, UnusableReportsFallBack) {
  const auto zero = admission::derive_limits({ 0, 0 });
  EXPECT_EQ(zero.limits.encode_sessions, admission::fallback_encode_sessions);
  EXPECT_EQ(zero.limits.pixel_rate, admission::fallback_pixel_rate);
  EXPECT_FALSE(zero.sessions_reported);
  EXPECT_FALSE(zero.pixel_rate_reported);

  const auto overflow = admission::derive_limits({ std::nullopt, std::numeric_limits<std::uint64_t>::max() / 255 });
  EXPECT_EQ(overflow.limits.pixel_rate, admission::fallback_pixel_rate);
  EXPECT_FALSE(overflow.pixel_rate_reported);
}

TEST(VideoAdmissionPolicy, DriverValuesAreReportedOnlyWhenPositive) {
  EXPECT_FALSE(video::encoder_capacity_t::reported(0));
  EXPECT_FALSE(video::encoder_capacity_t::reported(-1));
  EXPECT_EQ(video::encoder_capacity_t::reported(7), 7u);
}

TEST(VideoAdmissionPolicy, MergingKeepsTheLowerReportedValue) {
  video::encoder_capacity_t capacity;
  capacity.merge_min({ 4, std::nullopt });
  capacity.merge_min({ std::nullopt, 900 });
  capacity.merge_min({ 2, 1200 });
  capacity.merge_min({ 3, 600 });
  capacity.merge_min({});
  EXPECT_EQ(capacity.encode_sessions, 2u);
  EXPECT_EQ(capacity.macroblocks_per_second, 600u);
  EXPECT_TRUE(capacity.sessions_incomplete);
  EXPECT_TRUE(capacity.throughput_incomplete);
}

TEST(VideoAdmissionPolicy, CodecsThatAllReportKeepTheirValue) {
  video::encoder_capacity_t capacity;
  capacity.merge_min({ 16, 900'000 });
  capacity.merge_min({ 8, 1'200'000 });
  EXPECT_FALSE(capacity.sessions_incomplete);
  EXPECT_FALSE(capacity.throughput_incomplete);
  const auto derived = admission::derive_limits(capacity);
  EXPECT_EQ(derived.limits.encode_sessions, 8u);
  EXPECT_EQ(derived.limits.pixel_rate, 900'000u * admission::pixels_per_macroblock);
  EXPECT_TRUE(derived.sessions_reported);
  EXPECT_TRUE(derived.pixel_rate_reported);
}

// AMF reports H.264/HEVC session counts but none for AV1: the AV1 path must
// not inherit the others' larger value.
TEST(VideoAdmissionPolicy, AnUnreportingCodecCapsTheOthersAtTheFallback) {
  const std::uint64_t huge_rate = admission::fallback_pixel_rate;  // in macroblocks: 256x the fallback pixels
  video::encoder_capacity_t capacity;
  capacity.merge_min({ 16, huge_rate });
  capacity.merge_min({});
  auto derived = admission::derive_limits(capacity);
  EXPECT_EQ(derived.limits.encode_sessions, admission::fallback_encode_sessions);
  EXPECT_EQ(derived.limits.pixel_rate, admission::fallback_pixel_rate);
  EXPECT_FALSE(derived.sessions_reported);
  EXPECT_FALSE(derived.pixel_rate_reported);

  video::encoder_capacity_t tighter;
  tighter.merge_min({ 2, 1000 });
  tighter.merge_min({});
  derived = admission::derive_limits(tighter);
  EXPECT_EQ(derived.limits.encode_sessions, 2u) << "a reported value below the fallback still binds";
  EXPECT_EQ(derived.limits.pixel_rate, 1000u * admission::pixels_per_macroblock);
}

// Encoders spend whole macroblocks: 1080 rows are 68 macroblocks (1088 rows).
TEST(VideoAdmissionPolicy, AdmissionChargesWholeMacroblocks) {
  fake_clock_t clock;
  auto budget = managed_budget(clock);
  const std::uint64_t mb_1080p60 = 120ull * 68 * 60;
  ASSERT_TRUE(*budget.learn_adapter(adapter_a, admission::derive_limits({ 4, mb_1080p60 - 1 }).limits));
  EXPECT_FALSE(budget.reserve(hd(), budget.max_pending())) << "fits in raw pixels, not in macroblocks";

  ASSERT_TRUE(*budget.learn_adapter(adapter_a, admission::derive_limits({ 4, mb_1080p60 }).limits));
  auto pending = budget.reserve(hd(), budget.max_pending());
  ASSERT_TRUE(pending);
  EXPECT_EQ(budget.held(admission::resource_kind_e::pixel_rate, adapter_a), 1920ull * 1088 * 60);
}

TEST(VideoAdmissionPolicy, AnEnumeratedAdapterGetsTheFallbackOnce) {
  fake_clock_t clock;
  auto budget = managed_budget(clock);
  ASSERT_TRUE(*budget.ensure_adapter(adapter_a));
  ASSERT_TRUE(budget.limits(adapter_a));
  EXPECT_EQ(budget.limits(adapter_a)->encode_sessions, admission::fallback_encode_sessions);
  EXPECT_FALSE(budget.limits(adapter_b));

  ASSERT_TRUE(*budget.learn_adapter(adapter_a, { 1, admission::fallback_pixel_rate }));
  // Ensuring an adapter that is already known never resets its learned limits.
  EXPECT_TRUE(*budget.ensure_adapter(adapter_a));
  EXPECT_EQ(budget.limits(adapter_a)->encode_sessions, 1u);
}

TEST(VideoAdmissionPolicy, WithoutAFallbackAnUnknownAdapterStaysUnknown) {
  fake_clock_t clock;
  admission::budget_t budget { admission::policy_t { { { adapter_a, { 2, admission::fallback_pixel_rate } } }, 5000ms, 64 }, clock.clock() };
  EXPECT_TRUE(*budget.ensure_adapter(adapter_a));
  EXPECT_FALSE(*budget.ensure_adapter(adapter_b));
  EXPECT_FALSE(budget.limits(adapter_b));
  auto pending = budget.reserve(hd(adapter_b), budget.max_pending());
  ASSERT_FALSE(pending);
  EXPECT_EQ(pending.error().code, error_code_e::unknown_resource);
}

TEST(VideoAdmissionPolicy, AdapterZeroIsNeverAdded) {
  fake_clock_t clock;
  auto budget = managed_budget(clock);
  EXPECT_FALSE(*budget.ensure_adapter(0));
  EXPECT_FALSE(*budget.learn_adapter(0, { 4, admission::fallback_pixel_rate }));
  EXPECT_FALSE(budget.limits(0));
}

TEST(VideoAdmissionPolicy, LearnedLimitsBoundAdmission) {
  fake_clock_t clock;
  auto budget = managed_budget(clock);
  ASSERT_TRUE(*budget.learn_adapter(adapter_a, admission::derive_limits({ 1, std::nullopt }).limits));
  auto first = budget.reserve(hd(), budget.max_pending());
  ASSERT_TRUE(first) << first.error().detail;
  auto second = budget.reserve(hd(), budget.max_pending());
  EXPECT_FALSE(second);
  EXPECT_EQ(sessions(budget), 1u);
}

template<class Result>
bool declined(const Result &result) {
  return result.has_value() && !*result;
}

TEST(VideoAdmissionPolicy, LearningNeverMovesAHeldAdapter) {
  fake_clock_t clock;
  auto budget = managed_budget(clock);
  ASSERT_TRUE(*budget.ensure_adapter(adapter_a));
  auto pending = budget.reserve(hd(), budget.max_pending());
  ASSERT_TRUE(pending);
  EXPECT_TRUE(declined(budget.learn_adapter(adapter_a, { 1, 1 })));
  EXPECT_EQ(budget.limits(adapter_a)->encode_sessions, admission::fallback_encode_sessions);

  auto lease = pending->commit();
  ASSERT_TRUE(lease);
  EXPECT_TRUE(declined(budget.learn_adapter(adapter_a, { 1, 1 })));
  EXPECT_EQ(budget.limits(adapter_a)->pixel_rate, admission::fallback_pixel_rate);

  *lease = admission::lease_t {};
  EXPECT_TRUE(*budget.learn_adapter(adapter_a, { 1, admission::fallback_pixel_rate }));
  EXPECT_EQ(budget.limits(adapter_a)->encode_sessions, 1u);
}

TEST(VideoAdmissionPolicy, LearningKeepsOtherHoldingsAndTheRevision) {
  fake_clock_t clock;
  auto budget = managed_budget(clock);
  ASSERT_TRUE(*budget.ensure_adapter(adapter_b));
  {
    auto released = budget.reserve(hd(adapter_b), budget.max_pending());
    ASSERT_TRUE(released);
    auto gone = released->commit();
    ASSERT_TRUE(gone);
  }
  auto held = budget.reserve(hd(adapter_b), budget.max_pending());
  ASSERT_TRUE(held);
  const auto revision = budget.revision();
  ASSERT_GT(revision, 1u);

  ASSERT_TRUE(*budget.learn_adapter(adapter_a, { 2, admission::fallback_pixel_rate }));
  EXPECT_EQ(budget.revision(), revision);
  EXPECT_EQ(sessions(budget, adapter_b), 1u);
  auto lease = held->commit();
  ASSERT_TRUE(lease) << lease.error().detail;
  *lease = admission::lease_t {};
  EXPECT_EQ(sessions(budget, adapter_b), 0u);
  EXPECT_EQ(budget.revision(), revision + 2);
}

TEST(VideoAdmissionPolicy, AFailedRebuildChangesNothing) {
  fake_clock_t clock;
  admission::policy_t policy { {}, 5000ms, 2, admission::adapter_limits_t { 4, admission::fallback_pixel_rate } };
  admission::budget_t budget { policy, clock.clock() };
  ASSERT_TRUE(*budget.ensure_adapter(adapter_b));
  auto first = budget.reserve(hd(adapter_b), budget.max_pending());
  ASSERT_TRUE(first);
  auto second = budget.reserve(hd(adapter_b), budget.max_pending());
  ASSERT_TRUE(second);
  const auto revision = budget.revision();

  // Two live records fill a history of two, so the rebuild cannot fit them.
  auto learned = budget.learn_adapter(adapter_a, { 1, 1 });
  ASSERT_FALSE(learned);
  EXPECT_FALSE(budget.limits(adapter_a));
  EXPECT_EQ(budget.revision(), revision);
  EXPECT_EQ(sessions(budget, adapter_b), 2u);
  EXPECT_TRUE(first->commit());
  EXPECT_TRUE(second->commit());
}

TEST(VideoAdmissionPolicy, ASlowAppStartKeepsItsAdmission) {
  fake_clock_t clock;
  auto budget = managed_budget(clock);
  ASSERT_TRUE(*budget.ensure_adapter(adapter_a));
  // Reserved at launch, before the app starts; committed at capture attach.
  auto pending = budget.reserve(hd(), budget.max_pending());
  ASSERT_TRUE(pending);
  clock.advance(std::chrono::minutes { 30 });
  auto lease = pending->commit();
  ASSERT_TRUE(lease) << lease.error().detail;
  EXPECT_EQ(sessions(budget), 1u);
}

TEST(VideoAdmissionPolicy, TheBackstopStillEndsAnAbandonedAdmission) {
  fake_clock_t clock;
  auto budget = managed_budget(clock);
  ASSERT_TRUE(*budget.ensure_adapter(adapter_a));
  auto pending = budget.reserve(hd(), budget.max_pending());
  ASSERT_TRUE(pending);
  clock.advance(admission::production_max_pending + 1ms);
  auto lease = pending->commit();
  ASSERT_FALSE(lease);
  EXPECT_EQ(lease.error().code, error_code_e::expired);
  EXPECT_EQ(sessions(budget), 0u);
}
