/**
 * @file src/video_probe.h
 * @brief Platform-independent selection policy for encoder probe displays.
 */
#pragma once

#include <algorithm>
#include <optional>
#include <span>
#include <string>

namespace video {

  inline std::string
  select_encoder_probe_display(
    const std::string &configured_display,
    std::span<const std::string> capture_ready_displays) {
    if (std::ranges::find(capture_ready_displays, configured_display) != capture_ready_displays.end()) {
      return configured_display;
    }

    // An empty display preserves backend auto-selection, including adapter
    // filtering, display wake-up retries, and scanning subsequent outputs.
    return {};
  }

  // Matches the original value when device-id resolution fails; it may already be a display name.
  inline std::string
  resolve_capture_display(const std::string &requested, const std::string &resolved_name) {
    return resolved_name.empty() ? requested : resolved_name;
  }

  enum class capture_display_choice_e {
    requested,  ///< Capture the requested display at `index`.
    fallback,  ///< Keep the current default display.
    refused,  ///< Exact capture: the requested display is unavailable; capture nothing.
  };

  struct capture_display_choice_t {
    capture_display_choice_e choice;
    int index;
  };

  /**
   * Choose the capture display for a (re)initialization that the user did not switch.
   * An exact session (managed topology) never falls back to another display: streaming
   * a display the topology does not own is worse than failing the stream.
   */
  inline capture_display_choice_t
  select_capture_display(
    std::span<const std::string> display_names,
    int current,
    const std::string &requested,
    bool exact) {
    if (!requested.empty()) {
      if (const auto it = std::ranges::find(display_names, requested); it != display_names.end()) {
        return { capture_display_choice_e::requested, static_cast<int>(it - display_names.begin()) };
      }
    }
    return { exact ? capture_display_choice_e::refused : capture_display_choice_e::fallback, current };
  }

  /** The display index a manual switch selects, or nullopt when an exact session must ignore it. */
  inline std::optional<int>
  accept_display_switch(int requested, std::size_t count, bool exact) {
    if (exact || count == 0) {
      return std::nullopt;
    }
    return std::clamp(requested, 0, static_cast<int>(count) - 1);
  }

}  // namespace video
