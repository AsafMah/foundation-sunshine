/**
 * @file tests/unit/test_video_timestamps.cpp
 * @brief Duplicate frames are timestamped at their own send start, not at a pacing deadline from before an idle gap.
 */
#include "src/streaming/video_timestamps.h"

#include <gtest/gtest.h>

using namespace std::chrono_literals;
using clock_type = streaming::video_pacer_t::clock;

namespace {
  // Send one large paced frame at `t0` so the pacer carries a deadline into the next frame.
  streaming::video_pacer_t
  pacer_after_paced_frame(clock_type::time_point t0) {
    streaming::video_pacer_t pacer { t0 };
    const auto frame = pacer.begin_frame(t0, false, true, 20'000'000, 256 * 1024, 16'666'667ns);
    pacer.end_frame(frame, 180, 1416, t0);
    return pacer;
  }
}  // namespace

TEST(VideoTimestamps, DuplicateAfterIdleUsesItsOwnSendStart) {
  const auto t0 = clock_type::time_point {} + 100s;
  auto pacer = pacer_after_paced_frame(t0);
  ASSERT_GT(pacer.next_frame_start(), t0) << "the previous frame must leave a deadline behind";

  // One second of idle, then a repeat of the last picture (no capture time); small, so it is not paced.
  const auto now = t0 + 1s;
  const auto frame = pacer.begin_frame(now, false, false, 20'000'000, 1416, 16'666'667ns);
  const auto presentation = streaming::video_presentation_time(std::nullopt, pacer, frame);
  EXPECT_EQ(presentation, now);
  EXPECT_GT(presentation, pacer.next_frame_start()) << "a stale pre-idle deadline would rewind the RTP clock";
}

TEST(VideoTimestamps, DuplicateRtpTimestampAdvancesAcrossIdle) {
  const auto epoch = clock_type::time_point {};
  const auto t0 = epoch + 100s;
  auto pacer = pacer_after_paced_frame(t0);
  const auto previous_rtp = static_cast<std::uint32_t>((t0 - epoch) / 1ms * 90);
  const auto frame = pacer.begin_frame(t0 + 1s, false, false, 20'000'000, 1416, 16'666'667ns);
  const auto rtp = static_cast<std::uint32_t>((streaming::video_presentation_time(std::nullopt, pacer, frame) - epoch) / 1ms * 90);
  EXPECT_EQ(rtp - previous_rtp, 90'000u) << "one second of idle is 90000 ticks of the 90 kHz clock";
}

TEST(VideoTimestamps, CapturedFrameKeepsItsCaptureTime) {
  const auto t0 = clock_type::time_point {} + 100s;
  auto pacer = pacer_after_paced_frame(t0);
  const auto frame = pacer.begin_frame(t0 + 20ms, false, true, 20'000'000, 256 * 1024, 16'666'667ns);
  EXPECT_EQ(streaming::video_presentation_time(t0 + 5ms, pacer, frame), t0 + 5ms);
}
