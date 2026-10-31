#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string_view>

namespace nvhttp::abr_api {
  class rejection_log_limiter_t {
  public:
    using clock_t = std::chrono::steady_clock;

    bool
    should_log(std::string_view client_uuid, std::string_view code, clock_t::time_point now,
      std::uint64_t &suppressed) {
      const auto slot = code_slot(code);
      if (slot == kCodeCount) {
        return false;
      }

      std::uint64_t hash = 14695981039346656037ULL;
      for (const auto character : client_uuid) {
        hash = (hash ^ static_cast<unsigned char>(character)) * 1099511628211ULL;
      }

      std::lock_guard lock(mutex_);
      auto &state = states_[hash % kBucketCount][slot];
      if ((state.last_log != clock_t::time_point {} && now - state.last_log < kPerClientInterval) ||
          (last_global_log_ != clock_t::time_point {} && now - last_global_log_ < kGlobalInterval)) {
        ++globally_suppressed_;
        return false;
      }

      state.last_log = now;
      last_global_log_ = now;
      suppressed = globally_suppressed_;
      globally_suppressed_ = 0;
      return true;
    }

  private:
    static constexpr std::size_t kBucketCount = 64;
    static constexpr std::size_t kCodeCount = 7;
    static constexpr auto kPerClientInterval = std::chrono::seconds(60);
    static constexpr auto kGlobalInterval = std::chrono::seconds(5);

    struct state_t {
      clock_t::time_point last_log {};
    };

    static std::size_t
    code_slot(std::string_view code) {
      constexpr std::array<std::string_view, kCodeCount> codes {
        "invalid_body", "missing_client_identity", "no_active_stream", "session_starting",
        "ambiguous_session", "stale_session", "abr_not_enabled",
      };
      for (std::size_t i = 0; i < codes.size(); ++i) {
        if (codes[i] == code) {
          return i;
        }
      }
      return kCodeCount;
    }

    std::mutex mutex_;
    std::array<std::array<state_t, kCodeCount>, kBucketCount> states_ {};
    clock_t::time_point last_global_log_ {};
    std::uint64_t globally_suppressed_ {};
  };
}  // namespace nvhttp::abr_api
