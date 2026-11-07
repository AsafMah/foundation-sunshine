/**
 * @file tests/unit/test_display_group_member.cpp
 * @brief GW-H5: a display-group member's session injects no keyboard, text or gamepad input.
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "src/display_group_member.h"

namespace {
  /** An input packet as it arrives: NV_INPUT_HEADER (big-endian size, little-endian magic), then a body. */
  std::vector<std::uint8_t>
  packet(std::uint32_t magic, std::size_t body = 8) {
    std::vector<std::uint8_t> bytes(8 + body, 0);
    const auto size = static_cast<std::uint32_t>(4 + body);
    bytes[0] = static_cast<std::uint8_t>(size >> 24);
    bytes[1] = static_cast<std::uint8_t>(size >> 16);
    bytes[2] = static_cast<std::uint8_t>(size >> 8);
    bytes[3] = static_cast<std::uint8_t>(size);
    for (int i = 0; i < 4; ++i) bytes[4 + i] = static_cast<std::uint8_t>(magic >> (8 * i));
    return bytes;
  }

  const std::uint32_t leaders_only[] {
    KEY_DOWN_EVENT_MAGIC, KEY_UP_EVENT_MAGIC, UTF8_TEXT_EVENT_MAGIC, MULTI_CONTROLLER_MAGIC_GEN5,
    SS_CONTROLLER_ARRIVAL_MAGIC, SS_CONTROLLER_TOUCH_MAGIC, SS_CONTROLLER_MOTION_MAGIC, SS_CONTROLLER_BATTERY_MAGIC,
  };
  const std::uint32_t members_own[] {
    MOUSE_MOVE_ABS_MAGIC, MOUSE_MOVE_REL_MAGIC_GEN5, MOUSE_BUTTON_DOWN_EVENT_MAGIC_GEN5, MOUSE_BUTTON_UP_EVENT_MAGIC_GEN5,
    SCROLL_MAGIC_GEN5, SS_HSCROLL_MAGIC, SS_TOUCH_MAGIC, SS_PEN_MAGIC,
  };
}  // namespace

TEST(DisplayGroupMemberInput, KeyboardTextAndGamepadsBelongToTheLeader) {
  for (const auto magic : leaders_only) {
    EXPECT_FALSE(display_group::member_input_allowed(magic)) << std::hex << magic;
  }
}

TEST(DisplayGroupMemberInput, PointerTouchAndPenMapThroughTheMembersOwnHead) {
  for (const auto magic : members_own) {
    EXPECT_TRUE(display_group::member_input_allowed(magic)) << std::hex << magic;
  }
}

TEST(DisplayGroupMemberInput, OnlyAMembersLeaderInputIsDropped) {
  for (const auto magic : leaders_only) {
    EXPECT_TRUE(display_group::drop_member_input(true, packet(magic))) << std::hex << magic;
    EXPECT_FALSE(display_group::drop_member_input(false, packet(magic))) << "the leader and ungrouped sessions keep everything";
  }
  for (const auto magic : members_own) {
    EXPECT_FALSE(display_group::drop_member_input(true, packet(magic))) << std::hex << magic;
  }
}

TEST(DisplayGroupMemberInput, APacketTooShortForAHeaderIsLeftToTheInputLayer) {
  auto key = packet(KEY_DOWN_EVENT_MAGIC);
  key.resize(7);
  EXPECT_FALSE(display_group::drop_member_input(true, key));
  EXPECT_FALSE(display_group::drop_member_input(true, {}));
}
