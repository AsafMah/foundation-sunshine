/**
 * @file tests/unit/test_stream_dynamic_mode.cpp
 * @brief Managed native heads refuse client resolution/FPS change requests.
 * Provisional pending G1 hardware.
 */
#include <gtest/gtest.h>
#include <limits>
#include <string>

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
}  // namespace
