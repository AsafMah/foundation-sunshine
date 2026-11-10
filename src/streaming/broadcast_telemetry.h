/**
 * @file src/streaming/broadcast_telemetry.h
 * @brief Diagnostic rollup for the video broadcast thread. Observation only:
 * it never changes pacing, queueing or sending. Owned by one thread; no locking.
 */
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <sstream>
#include <string>

namespace stream::telemetry {
  using clock_t = std::chrono::steady_clock;
  using std::chrono::nanoseconds;

  struct thresholds_t {
    nanoseconds iteration { std::chrono::milliseconds(20) };
    nanoseconds send { std::chrono::milliseconds(10) };
    nanoseconds queue_age { std::chrono::milliseconds(10) };
    nanoseconds lock_wait { std::chrono::milliseconds(2) };
  };

  struct report_t {
    nanoseconds window {};
    nanoseconds max_iteration {};
    nanoseconds max_send {};
    nanoseconds max_queue_age {};
    nanoseconds max_lock_wait {};
    nanoseconds cpu {};
    std::uint64_t iterations = 0;
    std::uint64_t send_failures = 0;
    std::uint64_t send_would_block = 0;
    int last_send_error = 0;
  };

  inline double
  to_ms(nanoseconds value) {
    return std::chrono::duration<double, std::milli>(value).count();
  }

  inline std::string
  format(const report_t &r) {
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(2);
    const auto wall_ms = to_ms(r.window);
    out << "Broadcast stall telemetry: window=" << wall_ms << "ms iterations=" << r.iterations
        << " max_iteration=" << to_ms(r.max_iteration) << "ms max_send=" << to_ms(r.max_send)
        << "ms max_queue_age_at_pop=" << to_ms(r.max_queue_age) << "ms max_shared_call=" << to_ms(r.max_lock_wait)
        << "ms thread_cpu=" << to_ms(r.cpu) << "ms cpu_wall_ratio=" << (wall_ms > 0 ? to_ms(r.cpu) / wall_ms : 0.0)
        << " send_failures=" << r.send_failures << " send_would_block=" << r.send_would_block << " last_send_error=" << r.last_send_error;
    return out.str();
  }

  class broadcast_telemetry_t {
  public:
    explicit broadcast_telemetry_t(clock_t::time_point now, nanoseconds cpu_total = {}, thresholds_t thresholds = {}):
        limits(thresholds), window_start(now), cpu_start(cpu_total) {}

    void
    note_iteration(nanoseconds d) {
      ++current.iterations;
      current.max_iteration = std::max(current.max_iteration, d);
    }

    void
    note_send(nanoseconds d) {
      current.max_send = std::max(current.max_send, d);
    }

    void
    note_queue_age(nanoseconds d) {
      current.max_queue_age = std::max(current.max_queue_age, d);
    }

    void
    note_lock_wait(nanoseconds d) {
      current.max_lock_wait = std::max(current.max_lock_wait, d);
    }

    void
    note_send_failure(int error) {
      ++current.send_failures;
      if (error == 10035 || error == 11) {  // WSAEWOULDBLOCK / EAGAIN
        ++current.send_would_block;
      }
      current.last_send_error = error;
    }

    // Returns a report at most once per second, and only if a threshold was crossed.
    std::optional<report_t>
    poll(clock_t::time_point now, nanoseconds cpu_total) {
      if (now - window_start < std::chrono::seconds(1)) {
        return std::nullopt;
      }
      current.window = now - window_start;
      current.cpu = cpu_total - cpu_start;
      const auto crossed = current.max_iteration >= limits.iteration || current.max_send >= limits.send ||
                           current.max_queue_age >= limits.queue_age || current.max_lock_wait >= limits.lock_wait ||
                           current.send_failures > 0;
      auto result = current;
      current = {};
      window_start = now;
      cpu_start = cpu_total;
      if (!crossed) {
        return std::nullopt;
      }
      return result;
    }

  private:
    thresholds_t limits;
    clock_t::time_point window_start;
    nanoseconds cpu_start;
    report_t current;
  };
}  // namespace stream::telemetry
