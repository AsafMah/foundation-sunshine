/** @file test_ds5_auto_selection.cpp
 * Automatic gamepad-mode resolution for client controllers identified as DualSense.
 */
#ifdef _WIN32
  #include <gtest/gtest.h>
  #include "src/platform/windows/ds5/auto_selection.h"

namespace {
  using platf::ds5::client_identified_dualsense;
  using platf::ds5::mode_may_allocate_dualsense;
  using platf::ds5::resolve_auto_gamepad_mode;

  constexpr int auto_mode = 1;
  constexpr int x360 = 2;
  constexpr int ds4 = 3;
  constexpr int ds5 = 4;

  // What Moonlight sends for a USB DualSense with physical haptics: SDL PS5 pad,
  // touchpad, motion, LED, trigger rumble and the DualSense PCM capability.
  constexpr std::uint16_t dualsense_caps = LI_CCAP_ANALOG_TRIGGERS | LI_CCAP_RUMBLE | LI_CCAP_TRIGGER_RUMBLE |
                                           LI_CCAP_TOUCHPAD | LI_CCAP_ACCEL | LI_CCAP_GYRO | LI_CCAP_BATTERY_STATE |
                                           LI_CCAP_RGB_LED | LI_CCAP_DS5_HAPTICS_PCM;
  // A DualShock 4 reports the same PlayStation type and the same family of bits
  // except the DualSense PCM capability.
  constexpr std::uint16_t dualshock4_caps = LI_CCAP_ANALOG_TRIGGERS | LI_CCAP_RUMBLE | LI_CCAP_TOUCHPAD |
                                            LI_CCAP_ACCEL | LI_CCAP_GYRO | LI_CCAP_BATTERY_STATE | LI_CCAP_RGB_LED;

  TEST(Ds5AutoSelection, AutoMapsAnIdentifiedDualSenseToDualSense) {
    EXPECT_TRUE(client_identified_dualsense(LI_CTYPE_PS, dualsense_caps));
    EXPECT_EQ(resolve_auto_gamepad_mode(auto_mode, LI_CTYPE_PS, dualsense_caps, true), ds5);
  }

  TEST(Ds5AutoSelection, AutoKeepsDualShock4AndOtherPlayStationPads) {
    EXPECT_FALSE(client_identified_dualsense(LI_CTYPE_PS, dualshock4_caps));
    EXPECT_EQ(resolve_auto_gamepad_mode(auto_mode, LI_CTYPE_PS, dualshock4_caps, true), auto_mode);
    // A DualSense whose client uses emulated haptics carries no DualSense identity.
    EXPECT_EQ(resolve_auto_gamepad_mode(auto_mode, LI_CTYPE_PS, dualsense_caps & ~LI_CCAP_DS5_HAPTICS_PCM, true),
              auto_mode);
  }

  TEST(Ds5AutoSelection, IdentityRequiresThePlayStationType) {
    for (const auto type : { LI_CTYPE_UNKNOWN, LI_CTYPE_XBOX, LI_CTYPE_NINTENDO, LI_CTYPE_STEAM }) {
      EXPECT_EQ(resolve_auto_gamepad_mode(auto_mode, static_cast<std::uint8_t>(type), dualsense_caps, true), auto_mode);
    }
  }

  TEST(Ds5AutoSelection, AutoFallsBackWhenTheComponentIsUnavailable) {
    EXPECT_EQ(resolve_auto_gamepad_mode(auto_mode, LI_CTYPE_PS, dualsense_caps, false), auto_mode);
  }

  TEST(Ds5AutoSelection, ExplicitSelectionsAreNeverChanged) {
    for (const auto mode : { x360, ds4, ds5 }) {
      EXPECT_EQ(resolve_auto_gamepad_mode(mode, LI_CTYPE_PS, dualsense_caps, true), mode);
      EXPECT_EQ(resolve_auto_gamepad_mode(mode, LI_CTYPE_PS, dualshock4_caps, false), mode);
    }
  }

  TEST(Ds5AutoSelection, AutoAdvertisesDualSenseHapticsOnlyWhenTheComponentExists) {
    EXPECT_TRUE(mode_may_allocate_dualsense(auto_mode, true));
    EXPECT_TRUE(mode_may_allocate_dualsense(ds5, true));
    EXPECT_FALSE(mode_may_allocate_dualsense(auto_mode, false));
    EXPECT_FALSE(mode_may_allocate_dualsense(ds4, true));
    EXPECT_FALSE(mode_may_allocate_dualsense(x360, true));
  }
}  // namespace
#endif
