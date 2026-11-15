/**
 * @file src/streaming/video_timestamps.h
 * @brief Presentation time of a sent video frame for its 90 kHz RTP timestamp.
 */
#pragma once

#include <optional>

#include "video_pacing.h"

namespace streaming {
  /**
   * The capture time of a frame, or for an intentional duplicate (no capture time) the moment this frame's send
   * starts (`frame.start`). A duplicate must not take the pacer's deadline left over from the previous frame: after an
   * idle interval that deadline is in the past, and the repeat would carry a stale RTP timestamp.
   */
  inline video_pacer_t::clock::time_point
  video_presentation_time(const std::optional<video_pacer_t::clock::time_point> &captured,
    [[maybe_unused]] const video_pacer_t &pacer,
    const video_pacer_t::frame_t &frame) noexcept {
    return captured.value_or(frame.start);
  }
}  // namespace streaming
