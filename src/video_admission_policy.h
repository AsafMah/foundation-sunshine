/**
 * @file src/video_admission_policy.h
 * @brief The production live admission policy: managed profile only, limits from what the encoder reports.
 *
 * Provisional pending G1 hardware: the fallback constants and the reading of
 * driver-reported capacity are not yet observed on a real multi-head host.
 */
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>

#include "video_admission.h"
#include "video_encoder_capacity.h"

namespace video::admission {
  /** Sessions per adapter when the encoder does not report its own (NVENC, QSV, software). */
  inline constexpr std::uint64_t fallback_encode_sessions = 3;
  /** Pixel rate per adapter when the encoder does not report its throughput: two 4K60 streams. */
  inline constexpr std::uint64_t fallback_pixel_rate = 2ull * 3840 * 2160 * 60;
  /** Pixels in one 16x16 macroblock. */
  inline constexpr std::uint64_t pixels_per_macroblock = 16 * 16;
  /**
   * How long a launch's admission may stay pending. It is reserved before the
   * app starts and committed at capture attach, so it must outlast any slow app
   * start: the legacy path puts no deadline on app start. Launch-session prune
   * and RAII drop release it long before this in every normal path.
   */
  inline constexpr topology::monotonic_time_t production_max_pending = std::chrono::hours { 24 };
  inline constexpr std::size_t production_history_limit = 256;

  struct derived_limits_t {
    adapter_limits_t limits;
    bool sessions_reported {};
    bool pixel_rate_reported {};
  };

  /**
   * Adapter limits from a probe's reported capacity; anything unreported or
   * unusable falls back. When some validated codec did not report a field, the
   * others' value cannot speak for it, so that field is capped at the fallback.
   */
  inline derived_limits_t
  derive_limits(const encoder_capacity_t &capacity) {
    derived_limits_t derived { { fallback_encode_sessions, fallback_pixel_rate } };
    if (capacity.encode_sessions && *capacity.encode_sessions > 0) {
      if (!capacity.sessions_incomplete) {
        derived.limits.encode_sessions = *capacity.encode_sessions;
        derived.sessions_reported = true;
      }
      else {
        derived.limits.encode_sessions = std::min<std::uint64_t>(*capacity.encode_sessions, fallback_encode_sessions);
      }
    }
    std::uint64_t rate {};
    if (capacity.macroblocks_per_second && *capacity.macroblocks_per_second > 0 &&
        !__builtin_mul_overflow(*capacity.macroblocks_per_second, pixels_per_macroblock, &rate)) {
      if (!capacity.throughput_incomplete) {
        derived.limits.pixel_rate = rate;
        derived.pixel_rate_reported = true;
      }
      else {
        derived.limits.pixel_rate = std::min(rate, fallback_pixel_rate);
      }
    }
    return derived;
  }

  /**
   * The policy to install for this process. The blank (legacy) profile gets
   * none: no budget, nothing accounted, N=1 unchanged. A managed profile starts
   * with no adapters; each is added on validation or display enumeration.
   */
  inline std::optional<policy_t>
  production_policy(bool managed) {
    if (!managed) return std::nullopt;
    return policy_t {
      {},
      production_max_pending,
      production_history_limit,
      adapter_limits_t { fallback_encode_sessions, fallback_pixel_rate },
    };
  }
}  // namespace video::admission
