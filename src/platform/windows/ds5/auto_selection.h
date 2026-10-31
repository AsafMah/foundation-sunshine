/**
 * @file src/platform/windows/ds5/auto_selection.h
 * @brief Automatic gamepad-mode resolution for client controllers identified as DualSense.
 */
#pragma once

#include <cstdint>

#include <moonlight-common-c/src/Limelight.h>

namespace platf::ds5 {
  /// Gamepad modes shared with input.cpp: 1 auto, 2 Xbox 360, 3 DualShock 4, 4 DualSense.
  inline constexpr int gamepad_mode_auto = 1;
  inline constexpr int gamepad_mode_dualsense = 4;

  /**
   * @brief Whether the client positively identified this controller as a DualSense.
   * @details The arrival type only says "PlayStation" (DS3/DS4/DS5 alike). The only
   * DualSense-specific arrival bit is LI_CCAP_DS5_HAPTICS_PCM, which Moonlight sets for
   * SDL_CONTROLLER_TYPE_PS5 pads when its DualSense haptics mode is "physical".
   */
  constexpr bool
  client_identified_dualsense(std::uint8_t type, std::uint16_t capabilities) {
    return type == LI_CTYPE_PS && (capabilities & LI_CCAP_DS5_HAPTICS_PCM) != 0;
  }

  /**
   * @brief Resolves Auto to DualSense for an identified DualSense when the component can provide it.
   * @details Explicit selections (Xbox 360, DualShock 4, DualSense) are returned unchanged.
   * Without the component, or without a positive identification, Auto keeps its existing
   * type/capability-based choice, which maps PlayStation controllers to DualShock 4.
   */
  constexpr int
  resolve_auto_gamepad_mode(int gamepad_mode, std::uint8_t type, std::uint16_t capabilities, bool ds5_available) {
    return gamepad_mode == gamepad_mode_auto && ds5_available && client_identified_dualsense(type, capabilities) ?
             gamepad_mode_dualsense :
             gamepad_mode;
  }

  /**
   * @brief Whether a session in this mode may allocate a DualSense, for capability advertisement.
   */
  constexpr bool
  mode_may_allocate_dualsense(int gamepad_mode, bool ds5_available) {
    return ds5_available && (gamepad_mode == gamepad_mode_dualsense || gamepad_mode == gamepad_mode_auto);
  }
}  // namespace platf::ds5
