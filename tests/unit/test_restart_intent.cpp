#include "src/streaming/restart_intent.h"

#include <future>
#include <gtest/gtest.h>
#include <thread>

namespace ri = streaming::restart_intent;

TEST(RestartIntent, ConstantsMatchTheAgreedCommonCContract) {
  EXPECT_EQ(ri::SS_TERMINATION_HOST_RESTART_V1, 0x53535201u);
  EXPECT_EQ(ri::LI_FF_HOST_RESTART, 0x00000800u);
  EXPECT_EQ(ri::ML_FF_HOST_RESTART, 0x00000100u);
  EXPECT_EQ(ri::LI_HOST_RESTART_PROTOCOL_VERSION, 1u);
  EXPECT_EQ(ri::HOST_VERSION_ATTRIBUTE, "x-ss-restart.protocolVersion");
  EXPECT_EQ(ri::CLIENT_VERSION_ATTRIBUTE, "x-ml-restart.protocolVersion");
}

TEST(RestartIntent, TransientConsoleDetachIsNotMigration) {
  EXPECT_EQ(ri::classify_console_change(1, 0xffffffff, false), ri::console_action::requery);
  EXPECT_EQ(ri::classify_console_change(1, 1, false), ri::console_action::keep);
  EXPECT_EQ(ri::classify_console_change(1, 2, false), ri::console_action::migrate);
  EXPECT_EQ(ri::classify_console_change(1, 2, true), ri::console_action::stop);
}

TEST(RestartIntent, RequiresEveryNegotiationCondition) {
  EXPECT_TRUE(ri::accepts_restart(true, true, ri::ML_FF_HOST_RESTART, "1", true));
  EXPECT_FALSE(ri::accepts_restart(false, true, ri::ML_FF_HOST_RESTART, "1", true));
  EXPECT_FALSE(ri::accepts_restart(true, false, ri::ML_FF_HOST_RESTART, "1", true));
  EXPECT_FALSE(ri::accepts_restart(true, true, 0, "1", true));
  for (const auto version : { "", "0", "2", "01", "1junk" }) {
    EXPECT_FALSE(ri::accepts_restart(true, true, ri::ML_FF_HOST_RESTART, version, true));
  }
  EXPECT_FALSE(ri::accepts_restart(true, true, ri::ML_FF_HOST_RESTART, "1", false));
}

TEST(RestartIntent, PerSessionReasonAndTerminalPrecedence) {
  EXPECT_EQ(ri::termination_reason(true, true, false), ri::SS_TERMINATION_HOST_RESTART_V1);
  EXPECT_EQ(ri::termination_reason(true, false, false), ri::GRACEFUL_TERMINATION);
  EXPECT_EQ(ri::termination_reason(true, true, true), ri::GRACEFUL_TERMINATION);
  EXPECT_EQ(ri::termination_reason(false, true, false), ri::GRACEFUL_TERMINATION);
}

TEST(RestartIntent, SpecGoldenReasonBytes) {
  EXPECT_EQ(ri::reason_bytes(ri::termination_reason(true, true, false)),
    (std::array<std::uint8_t, 4> { 0x53, 0x53, 0x52, 0x01 }));
  EXPECT_EQ(ri::reason_bytes(ri::termination_reason(true, false, false)),
    (std::array<std::uint8_t, 4> { 0x80, 0x03, 0x00, 0x23 }));
  EXPECT_EQ(ri::termination_packet(0x0109, ri::SS_TERMINATION_HOST_RESTART_V1),
    (std::array<std::uint8_t, 8> { 0x09, 0x01, 0x04, 0x00, 0x53, 0x53, 0x52, 0x01 }));
  EXPECT_EQ(ri::termination_packet(0x0109, ri::GRACEFUL_TERMINATION),
    (std::array<std::uint8_t, 8> { 0x09, 0x01, 0x04, 0x00, 0x80, 0x03, 0x00, 0x23 }));
}

TEST(RestartIntent, ReasonIsPublishedBeforeShutdownAndTerminalCannotBeDowngraded) {
  ri::shutdown_state state;
  bool published = false;
  state.request(ri::shutdown_state::intent::migration, [&] { published = true; });
  state.send_reason(true, [&](auto reason) {
    EXPECT_TRUE(published);
    EXPECT_EQ(reason, ri::SS_TERMINATION_HOST_RESTART_V1);
  });
  state.request(ri::shutdown_state::intent::terminal, [] {});
  state.request(ri::shutdown_state::intent::migration, [] {});
  state.send_reason(true, [&](auto reason) { EXPECT_EQ(reason, ri::GRACEFUL_TERMINATION); });
}

TEST(RestartIntent, PublicationIsSerializedWithTheSender) {
  ri::shutdown_state state;
  std::promise<void> reason_set, release_publish;
  auto release = release_publish.get_future();
  auto request = std::async(std::launch::async, [&] {
    state.request(ri::shutdown_state::intent::migration, [&] {
      reason_set.set_value();
      release.wait();
    });
  });
  reason_set.get_future().wait();
  auto sender = std::async(std::launch::async, [&] {
    state.send_reason(true, [&](auto reason) { EXPECT_EQ(reason, ri::SS_TERMINATION_HOST_RESTART_V1); });
  });
  EXPECT_EQ(sender.wait_for(std::chrono::milliseconds(10)), std::future_status::timeout);
  release_publish.set_value();
  request.get();
  sender.get();
}

TEST(RestartIntent, StopIsRecheckedAtPublication) {
  ri::shutdown_state state;
  bool stopping = false;
  state.set_terminal_probe([&] { return stopping; });
  state.request(ri::shutdown_state::intent::migration, [] {});
  stopping = true;
  state.send_reason(true, [&](auto reason) { EXPECT_EQ(reason, ri::GRACEFUL_TERMINATION); });
}

TEST(RestartIntent, SignalStopPublishesTerminalWithoutWaitingForTheSenderLock) {
  ri::shutdown_state state;
  std::promise<void> locked, release;
  auto released = release.get_future();
  auto migration = std::async(std::launch::async, [&] {
    state.request(ri::shutdown_state::intent::migration, [&] {
      locked.set_value();
      released.wait();
    });
  });
  locked.get_future().wait();
  auto signal = std::async(std::launch::async, [&] {
    state.request_terminal_from_signal([] {});
  });
  EXPECT_EQ(signal.wait_for(std::chrono::milliseconds(100)), std::future_status::ready);
  release.set_value();
  migration.get();
  signal.get();
  state.send_reason(true, [&](auto reason) { EXPECT_EQ(reason, ri::GRACEFUL_TERMINATION); });
}

TEST(RestartIntent, DrainPerformsProtocolWorkWithinItsBudgetAndSurfacesErrors) {
  auto clock = std::chrono::steady_clock::time_point();
  int calls = 0;
  EXPECT_TRUE(ri::drain([&] { return clock; }, [&](auto wait) {
    ++calls;
    EXPECT_LE(wait, std::chrono::milliseconds(5));
    clock += wait;
    return true; }));
  EXPECT_EQ(calls, 50);
  EXPECT_EQ(clock.time_since_epoch(), ri::DRAIN_BUDGET);
  EXPECT_FALSE(ri::drain([&] { return clock; }, [](auto) { return false; }));
}
