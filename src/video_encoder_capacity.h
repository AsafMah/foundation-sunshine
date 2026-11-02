/**
 * @file src/video_encoder_capacity.h
 * @brief What a hardware encoder reports about its own capacity, for live admission.
 */
#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>

namespace video {
  /** Driver-reported encoder capacity; an unset field was not reported. */
  struct encoder_capacity_t {
    /** Concurrent encode sessions the driver supports. */
    std::optional<std::uint32_t> encode_sessions;
    /** Encode throughput in 16x16 macroblocks per second. */
    std::optional<std::uint64_t> macroblocks_per_second;
    /** A merged codec did not report the field, so a reported value cannot stand for the adapter alone. */
    bool sessions_incomplete {};
    bool throughput_incomplete {};

    /** A driver value; zero or negative means not reported. */
    template <class T>
    static std::optional<std::uint64_t>
    reported(T value) {
      if (value <= 0) return std::nullopt;
      return static_cast<std::uint64_t>(value);
    }

    /** Keep the lower of each reported value: the probe validated several codecs on one adapter. */
    void
    merge_min(const encoder_capacity_t &other) {
      auto lower = [](auto &mine, const auto &theirs, bool &incomplete, bool theirs_incomplete) {
        incomplete = incomplete || theirs_incomplete || !theirs;
        if (theirs) mine = mine ? std::min(*mine, *theirs) : *theirs;
      };
      lower(encode_sessions, other.encode_sessions, sessions_incomplete, other.sessions_incomplete);
      lower(macroblocks_per_second, other.macroblocks_per_second, throughput_incomplete, other.throughput_incomplete);
    }
  };
}  // namespace video
