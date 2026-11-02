/**
 * @file tests/unit/test_video_probe.cpp
 * @brief Tests for temporary encoder-probe display selection.
 */
#include <src/video_probe.h>

#include <array>
#include <gtest/gtest.h>

TEST(VideoProbe, PreservesCaptureReadyConfiguredDisplay) {
  const std::array displays { std::string { R"(\\.\DISPLAY7)" }, std::string { R"(\\.\DISPLAY9)" } };
  EXPECT_EQ(video::select_encoder_probe_display(R"(\\.\DISPLAY9)", displays), R"(\\.\DISPLAY9)");
}

TEST(VideoProbe, MissingConfiguredDisplayUsesBackendAutoselection) {
  const std::array displays { std::string { R"(\\.\DISPLAY7)" }, std::string { R"(\\.\DISPLAY9)" } };
  EXPECT_TRUE(video::select_encoder_probe_display(R"(\\.\DISPLAY18)", displays).empty());
}

TEST(VideoProbe, EmptyConfiguredDisplayPreservesBackendAutoselection) {
  const std::array displays { std::string { R"(\\.\DISPLAY7)" }, std::string { R"(\\.\DISPLAY9)" } };
  EXPECT_TRUE(video::select_encoder_probe_display({}, displays).empty());
}

TEST(VideoProbe, EmptyCaptureReadyListUsesBackendAutoselection) {
  const std::array<std::string, 0> displays {};
  EXPECT_TRUE(video::select_encoder_probe_display(R"(\\.\DISPLAY18)", displays).empty());
}

namespace {
  using choice_e = video::capture_display_choice_e;
  const std::array capture_displays { std::string { R"(\\.\DISPLAY1)" }, std::string { R"(\\.\DISPLAY4)" } };
}  // namespace

TEST(CaptureDisplay, ARequestedDisplayThatIsPresentIsCapturedInBothModes) {
  for (const bool exact : { false, true }) {
    const auto chosen = video::select_capture_display(capture_displays, 0, R"(\\.\DISPLAY4)", exact);
    EXPECT_EQ(chosen.choice, choice_e::requested) << exact;
    EXPECT_EQ(chosen.index, 1) << exact;
  }
}

TEST(CaptureDisplay, LegacyFallsBackToTheCurrentDisplayWhenTheRequestIsMissing) {
  const auto chosen = video::select_capture_display(capture_displays, 1, R"(\\.\DISPLAY9)", false);
  EXPECT_EQ(chosen.choice, choice_e::fallback);
  EXPECT_EQ(chosen.index, 1);
}

TEST(CaptureDisplay, LegacyWithoutARequestKeepsTheCurrentDisplay) {
  const auto chosen = video::select_capture_display(capture_displays, 1, {}, false);
  EXPECT_EQ(chosen.choice, choice_e::fallback);
  EXPECT_EQ(chosen.index, 1);
}

// A managed stream must never silently capture a display the topology does not own.
TEST(CaptureDisplay, ExactCaptureRefusesAMissingOrEmptyRequest) {
  EXPECT_EQ(video::select_capture_display(capture_displays, 0, R"(\\.\DISPLAY9)", true).choice, choice_e::refused);
  EXPECT_EQ(video::select_capture_display(capture_displays, 0, {}, true).choice, choice_e::refused);
  const std::array<std::string, 1> only_empty { std::string {} };
  EXPECT_EQ(video::select_capture_display(only_empty, 0, {}, true).choice, choice_e::refused)
    << "an empty name never matches, even against an empty entry";
}

TEST(CaptureDisplay, UnresolvedDeviceIdsAreMatchedVerbatim) {
  EXPECT_EQ(video::resolve_capture_display("{dev}", R"(\\.\DISPLAY4)"), R"(\\.\DISPLAY4)");
  EXPECT_EQ(video::resolve_capture_display(R"(\\.\DISPLAY1)", {}), R"(\\.\DISPLAY1)");
}

TEST(CaptureDisplay, LegacyHonoursAManualSwitchClampedToTheList) {
  EXPECT_EQ(video::accept_display_switch(1, 3, false), 1);
  EXPECT_EQ(video::accept_display_switch(9, 3, false), 2);
  EXPECT_EQ(video::accept_display_switch(-4, 3, false), 0);
}

TEST(CaptureDisplay, ExactCaptureIgnoresManualSwitches) {
  EXPECT_EQ(video::accept_display_switch(1, 3, true), std::nullopt)
    << "a managed stream never leaves the display its topology owns";
}
