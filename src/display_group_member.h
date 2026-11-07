/**
 * @file src/display_group_member.h
 * @brief GW-H5: what a display-group member's own stream session may do.
 * @details A display group streams several host heads as separate sessions: the leader (head 0) and members.
 *          Keyboard, text and gamepads belong to the leader (the client forwards a member window's keys to it),
 *          and members get no audio; pointer, touch and pen map through each member's own head.
 */
#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

#include <moonlight-common-c/src/Input.h>

namespace display_group {

  /** Whether a member's session may inject an input packet with this (little-endian decoded) magic. */
  constexpr bool
  member_input_allowed(std::uint32_t magic) noexcept {
    switch (magic) {
      case KEY_DOWN_EVENT_MAGIC:
      case KEY_UP_EVENT_MAGIC:
      case UTF8_TEXT_EVENT_MAGIC:
      case MULTI_CONTROLLER_MAGIC_GEN5:
      case SS_CONTROLLER_ARRIVAL_MAGIC:
      case SS_CONTROLLER_TOUCH_MAGIC:
      case SS_CONTROLLER_MOTION_MAGIC:
      case SS_CONTROLLER_BATTERY_MAGIC:
        return false;
      default:
        return true;
    }
  }

  /**
   * Whether to drop a raw input packet (NV_INPUT_HEADER first) received on a session. Only a member's
   * keyboard, text and gamepad packets are dropped; a packet too short to carry a header is left to
   * the input layer, which rejects it itself.
   */
  inline bool
  drop_member_input(bool member, const std::vector<std::uint8_t> &packet) noexcept {
    if (!member || packet.size() < 8) {
      return false;
    }
    // NV_INPUT_HEADER: big-endian size, then the little-endian magic.
    const std::uint32_t magic = static_cast<std::uint32_t>(packet[4]) | (static_cast<std::uint32_t>(packet[5]) << 8) |
                                (static_cast<std::uint32_t>(packet[6]) << 16) | (static_cast<std::uint32_t>(packet[7]) << 24);
    return !member_input_allowed(magic);
  }

}  // namespace display_group
