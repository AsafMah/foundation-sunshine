/**
 * @file src/nvhttp_launch_cooldown.h
 * @brief Managed launch failure cooldown (provisional pending G1 hardware).
 */
#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include "src/logging.h"

namespace nvhttp::stream_start {
  /**
   * After a hardware-side failure preparing a managed target, refuse that target
   * for a fixed window instead of driving the virtual display driver again.
   * Budget refusals are not failures here; a success clears the target.
   */
  class launch_cooldown_t {
  public:
    using clock_t = std::function<std::chrono::steady_clock::time_point()>;

    static constexpr std::chrono::milliseconds window { 5000 };

    explicit launch_cooldown_t(clock_t clock = [] { return std::chrono::steady_clock::now(); }):
        clock_ { std::move(clock) } {}

    /** The time left while `key` cools down, logging the refusal; nullopt admits. */
    std::optional<std::chrono::milliseconds>
    refuse(const std::string &key) {
      std::lock_guard lock { mutex_ };
      const auto found = until_.find(key);
      if (found == until_.end()) return std::nullopt;
      const auto now = clock_();
      if (now >= found->second) {
        until_.erase(found);
        return std::nullopt;
      }
      const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(found->second - now);
      BOOST_LOG(info) << "Managed launch on [" << key << "] refused: cooling down for another " << remaining.count() << " ms";
      return remaining;
    }

    void
    fail(const std::string &key, std::string_view reason) {
      std::lock_guard lock { mutex_ };
      until_[key] = clock_() + window;
      BOOST_LOG(info) << "Managed launch on [" << key << "] failed (" << reason << "): cooling down for " << window.count() << " ms";
    }

    void
    succeed(const std::string &key) {
      std::lock_guard lock { mutex_ };
      until_.erase(key);
    }

  private:
    clock_t clock_;
    std::mutex mutex_;
    std::map<std::string, std::chrono::steady_clock::time_point> until_;
  };
}  // namespace nvhttp::stream_start
