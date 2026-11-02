/**
 * @file tests/unit/test_stream_dynamic_mode.cpp
 * @brief Managed native heads refuse client resolution/FPS change requests.
 * Provisional pending G1 hardware.
 */
#include <gtest/gtest.h>
#include <algorithm>
#include <expected>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include "src/stream_dynamic_mode.h"

namespace {
  using namespace stream::dynamic_mode;

  struct monitor_t {
    int width = 1920;
    int height = 1080;
    int framerate = 60;
    bool display_exact = false;
  };

  TEST(StreamDynamicMode, MonitorOverloadRefusesAManagedSessionFromDisplayExact) {
    monitor_t monitor;
    monitor.display_exact = true;
    int applied = 0;
    const auto resolution = gate(monitor, request_t::resolution(2560, 1440), [&] { ++applied; });
    const auto fps = gate(monitor, request_t::fps(120.0f), [&] { ++applied; });
    const auto same = gate(monitor, request_t::resolution(1920, 1080), [&] { ++applied; });
    EXPECT_EQ(resolution.decision, decision_e::refused_managed);
    EXPECT_NE(resolution.message.find("1920x1080"), std::string::npos);
    EXPECT_EQ(fps.decision, decision_e::refused_managed);
    EXPECT_NE(fps.message.find("60 FPS"), std::string::npos);
    EXPECT_EQ(same.decision, decision_e::unchanged);
    EXPECT_EQ(applied, 0);
  }

  TEST(StreamDynamicMode, MonitorOverloadRunsTheLegacyBodyWithoutDisplayExact) {
    monitor_t monitor;
    int applied = 0;
    gate(monitor, request_t::resolution(1920, 1080), [&] { ++applied; });
    gate(monitor, request_t::fps(120.0f), [&] { ++applied; });
    EXPECT_EQ(applied, 2);
  }

  TEST(StreamDynamicMode, LegacyResolutionRequestRunsTheLegacyBody) {
    monitor_t monitor;
    int applied = 0;
    const auto outcome = gate(false, request_t::resolution(2560, 1440), monitor.width, monitor.height, monitor.framerate, [&] {
      ++applied;
      monitor.width = 2560;
      monitor.height = 1440;
    });
    EXPECT_EQ(outcome.decision, decision_e::applied);
    EXPECT_EQ(applied, 1);
    EXPECT_EQ(monitor.width, 2560);
    EXPECT_TRUE(outcome.message.empty());
  }

  TEST(StreamDynamicMode, LegacyUnchangedAndInvalidRequestsStillReachTheLegacyBody) {
    // The legacy body owns its own validation and unchanged-mode handling.
    int applied = 0;
    monitor_t monitor;
    gate(false, request_t::resolution(1920, 1080), monitor.width, monitor.height, monitor.framerate, [&] { ++applied; });
    gate(false, request_t::resolution(-1, 0), monitor.width, monitor.height, monitor.framerate, [&] { ++applied; });
    gate(false, request_t::fps(60.0f), monitor.width, monitor.height, monitor.framerate, [&] { ++applied; });
    gate(false, request_t::fps(5000.0f), monitor.width, monitor.height, monitor.framerate, [&] { ++applied; });
    EXPECT_EQ(applied, 4);
  }

  TEST(StreamDynamicMode, LegacyFpsRequestRunsTheLegacyBody) {
    monitor_t monitor;
    int applied = 0;
    const auto outcome = gate(false, request_t::fps(120.0f), monitor.width, monitor.height, monitor.framerate, [&] {
      ++applied;
      monitor.framerate = 120;
    });
    EXPECT_EQ(outcome.decision, decision_e::applied);
    EXPECT_EQ(applied, 1);
    EXPECT_EQ(monitor.framerate, 120);
  }

  TEST(StreamDynamicMode, ManagedResolutionRequestIsRefusedWithoutMutation) {
    monitor_t monitor;
    int applied = 0;
    const auto outcome = gate(true, request_t::resolution(2560, 1440), monitor.width, monitor.height, monitor.framerate, [&] {
      ++applied;
      monitor.width = 2560;
    });
    EXPECT_EQ(outcome.decision, decision_e::refused_managed);
    EXPECT_EQ(applied, 0);
    EXPECT_EQ(monitor.width, 1920);
    EXPECT_EQ(monitor.height, 1080);
    EXPECT_NE(outcome.message.find("1920x1080"), std::string::npos);
    EXPECT_NE(outcome.message.find("2560x1440"), std::string::npos);
    EXPECT_NE(outcome.message.find("managed"), std::string::npos);
  }

  TEST(StreamDynamicMode, ManagedFpsRequestIsRefusedWithoutMutation) {
    monitor_t monitor;
    int applied = 0;
    const auto outcome = gate(true, request_t::fps(120.0f), monitor.width, monitor.height, monitor.framerate, [&] {
      ++applied;
      monitor.framerate = 120;
    });
    EXPECT_EQ(outcome.decision, decision_e::refused_managed);
    EXPECT_EQ(applied, 0);
    EXPECT_EQ(monitor.framerate, 60);
    EXPECT_NE(outcome.message.find("60"), std::string::npos);
    EXPECT_NE(outcome.message.find("120"), std::string::npos);
    EXPECT_NE(outcome.message.find("FPS"), std::string::npos);
  }

  TEST(StreamDynamicMode, ManagedRequestForTheCurrentModeIsANoOpWithoutRefusal) {
    monitor_t monitor;
    int applied = 0;
    const auto resolution = gate(true, request_t::resolution(1920, 1080), monitor.width, monitor.height, monitor.framerate, [&] { ++applied; });
    const auto fps = gate(true, request_t::fps(60.0f), monitor.width, monitor.height, monitor.framerate, [&] { ++applied; });
    EXPECT_EQ(resolution.decision, decision_e::unchanged);
    EXPECT_EQ(fps.decision, decision_e::unchanged);
    EXPECT_TRUE(resolution.message.empty());
    EXPECT_TRUE(fps.message.empty());
    EXPECT_EQ(applied, 0);
  }

  TEST(StreamDynamicMode, ManagedFractionalOrNonFiniteFpsIsRefused) {
    // The legacy body would still raise a 60.9 FPS event, so only an exact match is unchanged.
    monitor_t monitor;
    int applied = 0;
    const auto fractional = gate(true, request_t::fps(60.9f), monitor.width, monitor.height, monitor.framerate, [&] { ++applied; });
    const auto nan = gate(true, request_t::fps(std::numeric_limits<float>::quiet_NaN()), monitor.width, monitor.height, monitor.framerate, [&] { ++applied; });
    EXPECT_EQ(fractional.decision, decision_e::refused_managed);
    EXPECT_FALSE(fractional.message.empty());
    EXPECT_EQ(nan.decision, decision_e::refused_managed);
    EXPECT_EQ(applied, 0);
    EXPECT_EQ(monitor.framerate, 60);
  }

  // A managed resolution change against fakes: every step is traced in order.
  struct managed_fake_t {
    std::vector<std::string> trace;
    bool exclusive = true;
    std::expected<int, std::string> reservation = 7;
    std::vector<head_change_t> changes { { head_verdict_e::applied, "\\\\.\\DISPLAY9", {} } };
    std::size_t next_change = 0;
    std::expected<void, std::string> committed {};
    // Displays the capture cannot follow; any other retarget succeeds.
    std::map<std::string, std::string> unfollowable;
    int live_pending = 0;

    // A move-only pending that counts how many reservations are alive (dropping one rolls it back).
    struct pending_t {
      managed_fake_t *owner;
      int id;
      pending_t(managed_fake_t *o, int i):
          owner(o), id(i) { ++owner->live_pending; }
      pending_t(pending_t &&other) noexcept:
          owner(std::exchange(other.owner, nullptr)), id(other.id) {}
      pending_t(const pending_t &) = delete;
      ~pending_t() {
        if (owner) --owner->live_pending;
      }
    };

    outcome_t
    run(int to_width, int to_height) {
      return change_managed_resolution(request_t::resolution(to_width, to_height), 1920, 1080, "\\\\.\\DISPLAY9",
        [this](auto &&body) {
          trace.push_back("exclusive");
          if (!exclusive) return false;
          body();
          return true;
        },
        [this](std::uint32_t w, std::uint32_t h) -> std::expected<pending_t, std::string> {
          trace.push_back("reserve " + std::to_string(w) + "x" + std::to_string(h));
          if (!reservation) return std::unexpected(reservation.error());
          return pending_t { this, *reservation };
        },
        [this](std::uint32_t w, std::uint32_t h) {
          trace.push_back("change " + std::to_string(w) + "x" + std::to_string(h));
          const auto i = std::min(next_change++, changes.size() - 1);
          return changes[i];
        },
        [this](const std::string &display) -> std::expected<void, std::string> {
          trace.push_back("retarget " + display);
          if (const auto it = unfollowable.find(display); it != unfollowable.end()) return std::unexpected(it->second);
          return {};
        },
        [this](pending_t &&pending) -> std::expected<void, std::string> {
          pending_t consumed { std::move(pending) };
          trace.push_back("commit " + std::to_string(consumed.id));
          return committed;
        },
        [this](int w, int h) { trace.push_back("apply " + std::to_string(w) + "x" + std::to_string(h)); });
    }
  };

  TEST(StreamDynamicModeManaged, AdmitsThenChangesTheHeadThenCommitsThenFollows) {
    managed_fake_t fake;
    const auto outcome = fake.run(2560, 1440);
    EXPECT_EQ(outcome.decision, decision_e::applied);
    EXPECT_NE(outcome.message.find("1920x1080 -> 2560x1440"), std::string::npos);
    EXPECT_EQ(fake.trace, (std::vector<std::string> { "exclusive", "reserve 2560x1440", "change 2560x1440", "commit 7", "apply 2560x1440" }));
    EXPECT_EQ(fake.live_pending, 0);
  }

  TEST(StreamDynamicModeManaged, PlatformUnchangedStillCommitsAndFollows) {
    managed_fake_t fake;
    fake.changes = { { head_verdict_e::unchanged, "\\\\.\\DISPLAY9", {} } };
    const auto outcome = fake.run(2560, 1440);
    EXPECT_EQ(outcome.decision, decision_e::applied);
    EXPECT_EQ(fake.trace.back(), "apply 2560x1440");
  }

  TEST(StreamDynamicModeManaged, BudgetRefusalNeverTouchesTheHead) {
    managed_fake_t fake;
    fake.reservation = std::unexpected(std::string { "over the adapter's encode sessions" });
    const auto outcome = fake.run(2560, 1440);
    EXPECT_EQ(outcome.decision, decision_e::refused_managed);
    EXPECT_NE(outcome.message.find("keeping 1920x1080"), std::string::npos);
    EXPECT_NE(outcome.message.find("encode sessions"), std::string::npos);
    EXPECT_EQ(fake.trace, (std::vector<std::string> { "exclusive", "reserve 2560x1440" }));
  }

  TEST(StreamDynamicModeManaged, HeadRefusalRollsTheReservationBackAndKeepsTheMode) {
    managed_fake_t fake;
    fake.changes = { { head_verdict_e::refused, "\\\\.\\DISPLAY9", "mode not in the head's list" } };
    const auto outcome = fake.run(2560, 1440);
    EXPECT_EQ(outcome.decision, decision_e::refused_managed);
    EXPECT_NE(outcome.message.find("mode not in the head's list"), std::string::npos);
    EXPECT_EQ(fake.trace, (std::vector<std::string> { "exclusive", "reserve 2560x1440", "change 2560x1440" }));
    EXPECT_EQ(fake.live_pending, 0);
  }

  TEST(StreamDynamicModeManaged, HeadConflictTriesToRestoreAndFailsWhenItCannot) {
    managed_fake_t fake;
    fake.changes = { { head_verdict_e::conflict, {}, "journal write failed" }, { head_verdict_e::refused, {}, "In conflict" } };
    const auto outcome = fake.run(2560, 1440);
    EXPECT_EQ(outcome.decision, decision_e::failed);
    EXPECT_NE(outcome.message.find("journal write failed"), std::string::npos);
    EXPECT_NE(outcome.message.find("In conflict"), std::string::npos);
    EXPECT_EQ(fake.trace, (std::vector<std::string> { "exclusive", "reserve 2560x1440", "change 2560x1440", "change 1920x1080" }));
    EXPECT_EQ(fake.live_pending, 0);
  }

  TEST(StreamDynamicModeManaged, HeadConflictWithAProvenRestoreKeepsTheStream) {
    managed_fake_t fake;
    fake.changes = { { head_verdict_e::conflict, {}, "re-verify timed out" }, { head_verdict_e::unchanged, "\\\\.\\DISPLAY9", {} } };
    const auto outcome = fake.run(2560, 1440);
    EXPECT_EQ(outcome.decision, decision_e::refused_managed);
    EXPECT_NE(outcome.message.find("previous mode restored"), std::string::npos);
    EXPECT_EQ(fake.trace.back(), "change 1920x1080");
    EXPECT_EQ(fake.live_pending, 0);
  }

  TEST(StreamDynamicModeManaged, AMovedHeadIsFollowedBeforeTheCommit) {
    managed_fake_t fake;
    fake.changes = { { head_verdict_e::applied, "\\\\.\\DISPLAY10", {} } };
    const auto outcome = fake.run(2560, 1440);
    EXPECT_EQ(outcome.decision, decision_e::applied);
    EXPECT_NE(outcome.message.find("1920x1080 -> 2560x1440"), std::string::npos);
    EXPECT_NE(outcome.message.find("moved from [\\\\.\\DISPLAY9] to [\\\\.\\DISPLAY10]"), std::string::npos) << outcome.message;
    EXPECT_EQ(fake.trace, (std::vector<std::string> { "exclusive", "reserve 2560x1440", "change 2560x1440", "retarget \\\\.\\DISPLAY10", "commit 7", "apply 2560x1440" }));
    EXPECT_EQ(fake.live_pending, 0);
  }

  TEST(StreamDynamicModeManaged, AMovedHeadTheCaptureCannotFollowIsChangedBackAndNeverCommitted) {
    managed_fake_t fake;
    fake.unfollowable["\\\\.\\DISPLAY10"] = "another capture owns it";
    fake.changes = { { head_verdict_e::applied, "\\\\.\\DISPLAY10", {} }, { head_verdict_e::applied, "\\\\.\\DISPLAY9", {} } };
    const auto outcome = fake.run(2560, 1440);
    EXPECT_EQ(outcome.decision, decision_e::refused_managed);
    EXPECT_NE(outcome.message.find("keeping 1920x1080"), std::string::npos);
    EXPECT_NE(outcome.message.find("another capture owns it"), std::string::npos) << outcome.message;
    EXPECT_NE(outcome.message.find("previous mode restored"), std::string::npos);
    EXPECT_EQ(fake.trace, (std::vector<std::string> { "exclusive", "reserve 2560x1440", "change 2560x1440", "retarget \\\\.\\DISPLAY10", "change 1920x1080" }));
    EXPECT_EQ(fake.live_pending, 0);
  }

  TEST(StreamDynamicModeManaged, AMovedHeadWhoseRestoreCannotBeFollowedFails) {
    managed_fake_t fake;
    fake.unfollowable["\\\\.\\DISPLAY10"] = "another capture owns it";
    fake.changes = { { head_verdict_e::applied, "\\\\.\\DISPLAY10", {} }, { head_verdict_e::applied, "\\\\.\\DISPLAY10", {} } };
    const auto outcome = fake.run(2560, 1440);
    EXPECT_EQ(outcome.decision, decision_e::failed) << "the old display is gone and the capture cannot follow the head";
    EXPECT_NE(outcome.message.find("DISPLAY10"), std::string::npos);
    EXPECT_EQ(fake.trace, (std::vector<std::string> { "exclusive", "reserve 2560x1440", "change 2560x1440", "retarget \\\\.\\DISPLAY10", "change 1920x1080", "retarget \\\\.\\DISPLAY10" }));
  }

  TEST(StreamDynamicModeManaged, AHeadWithAnUnknownDisplayIsNeverFollowed) {
    managed_fake_t fake;
    fake.changes = { { head_verdict_e::applied, {}, {} }, { head_verdict_e::applied, "\\\\.\\DISPLAY9", {} } };
    const auto outcome = fake.run(2560, 1440);
    EXPECT_EQ(outcome.decision, decision_e::refused_managed);
    EXPECT_NE(outcome.message.find("previous mode restored"), std::string::npos);
    EXPECT_EQ(fake.trace, (std::vector<std::string> { "exclusive", "reserve 2560x1440", "change 2560x1440", "change 1920x1080" }));
  }

  TEST(StreamDynamicModeManaged, ARestoreThatMovesTheHeadAgainIsFollowed) {
    managed_fake_t fake;
    fake.committed = std::unexpected(std::string { "encoder generation moved" });
    fake.changes = { { head_verdict_e::applied, "\\\\.\\DISPLAY10", {} }, { head_verdict_e::applied, "\\\\.\\DISPLAY11", {} } };
    const auto outcome = fake.run(2560, 1440);
    EXPECT_EQ(outcome.decision, decision_e::refused_managed);
    EXPECT_NE(outcome.message.find("previous mode restored"), std::string::npos);
    EXPECT_EQ(fake.trace, (std::vector<std::string> { "exclusive", "reserve 2560x1440", "change 2560x1440", "retarget \\\\.\\DISPLAY10", "commit 7",
                            "change 1920x1080", "retarget \\\\.\\DISPLAY11" }));
  }

  TEST(StreamDynamicModeManaged, AConflictRestoredOntoAMovedHeadIsFollowed) {
    managed_fake_t fake;
    fake.changes = { { head_verdict_e::conflict, {}, "re-verify timed out" }, { head_verdict_e::applied, "\\\\.\\DISPLAY10", {} } };
    const auto outcome = fake.run(2560, 1440);
    EXPECT_EQ(outcome.decision, decision_e::refused_managed);
    EXPECT_EQ(fake.trace.back(), "retarget \\\\.\\DISPLAY10");
  }

  TEST(StreamDynamicModeManaged, CommitFailureRestoresThePreviousMode) {
    managed_fake_t fake;
    fake.committed = std::unexpected(std::string { "encoder generation moved" });
    fake.changes = { { head_verdict_e::applied, "\\\\.\\DISPLAY9", {} }, { head_verdict_e::applied, "\\\\.\\DISPLAY9", {} } };
    const auto outcome = fake.run(2560, 1440);
    EXPECT_EQ(outcome.decision, decision_e::refused_managed);
    EXPECT_NE(outcome.message.find("previous mode restored"), std::string::npos);
    EXPECT_NE(outcome.message.find("encoder generation moved"), std::string::npos);
    EXPECT_EQ(fake.trace, (std::vector<std::string> { "exclusive", "reserve 2560x1440", "change 2560x1440", "commit 7", "change 1920x1080" }));
  }

  TEST(StreamDynamicModeManaged, CommitFailureWithAFailedRestoreFails) {
    managed_fake_t fake;
    fake.committed = std::unexpected(std::string { "encoder generation moved" });
    fake.changes = { { head_verdict_e::applied, "\\\\.\\DISPLAY9", {} }, { head_verdict_e::refused, "\\\\.\\DISPLAY9", "busy" } };
    const auto outcome = fake.run(2560, 1440);
    EXPECT_EQ(outcome.decision, decision_e::failed);
    EXPECT_NE(outcome.message.find("busy"), std::string::npos);
    EXPECT_EQ(fake.trace.back(), "change 1920x1080");
  }

  TEST(StreamDynamicModeManaged, RestoreToADisplayTheCaptureCannotFollowIsNotARestore) {
    managed_fake_t fake;
    fake.committed = std::unexpected(std::string { "x" });
    fake.unfollowable["\\\\.\\DISPLAY10"] = "no live capture";
    fake.changes = { { head_verdict_e::applied, "\\\\.\\DISPLAY9", {} }, { head_verdict_e::applied, "\\\\.\\DISPLAY10", {} } };
    EXPECT_EQ(fake.run(2560, 1440).decision, decision_e::failed);
    managed_fake_t unknown;
    unknown.committed = std::unexpected(std::string { "x" });
    unknown.changes = { { head_verdict_e::applied, "\\\\.\\DISPLAY9", {} }, { head_verdict_e::applied, {}, {} } };
    EXPECT_EQ(unknown.run(2560, 1440).decision, decision_e::failed);
    EXPECT_EQ(unknown.trace.back(), "change 1920x1080") << "an unknown display is never retargeted";
  }

  TEST(StreamDynamicModeManaged, OtherStreamsActiveRefusesWithoutAnything) {
    managed_fake_t fake;
    fake.exclusive = false;
    const auto outcome = fake.run(2560, 1440);
    EXPECT_EQ(outcome.decision, decision_e::refused_managed);
    EXPECT_NE(outcome.message.find("other streams are active"), std::string::npos);
    EXPECT_EQ(fake.trace, (std::vector<std::string> { "exclusive" }));
  }

  TEST(StreamDynamicModeManaged, InvalidOrUnchangedSizesDoNothing) {
    for (const auto &[w, h] : std::vector<std::pair<int, int>> { { 0, 1080 }, { 1920, -1 }, { 16385, 1080 }, { 1920, 16385 } }) {
      managed_fake_t fake;
      EXPECT_EQ(fake.run(w, h).decision, decision_e::refused_managed) << w << 'x' << h;
      EXPECT_TRUE(fake.trace.empty());
    }
    managed_fake_t edge;
    EXPECT_EQ(edge.run(16384, 16384).decision, decision_e::applied);
    managed_fake_t same;
    const auto outcome = same.run(1920, 1080);
    EXPECT_EQ(outcome.decision, decision_e::unchanged);
    EXPECT_TRUE(outcome.message.empty());
    EXPECT_TRUE(same.trace.empty());
  }
}  // namespace
