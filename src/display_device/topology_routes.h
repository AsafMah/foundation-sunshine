/**
 * @file src/display_device/topology_routes.h
 * @brief Platform-neutral lifecycle helpers for coordinated production routes.
 */
#pragma once

#include "topology_coordinator.h"

namespace display_device::topology {
  struct retry_token_t {
    std::uint64_t generation {};

    bool
    operator==(const retry_token_t &) const = default;
  };

  enum class retry_completion_e {
    stale,
    disarmed,
    rearmed,
  };

  /**
   * Generation-tagged timer state. The caller serializes access with its timer mutex,
   * never held while posting or waiting. A posted receipt stays paired with its token;
   * the worker checks is_current() under that same mutex before doing retry work.
   */
  class retry_schedule_t {
  public:
    explicit retry_schedule_t(monotonic_time_t delay);

    void
    setup(bool enabled, monotonic_time_t now);

    std::optional<retry_token_t>
    take_due(monotonic_time_t now);

    bool
    is_current(retry_token_t token) const;

    /**
     * Failures/rejections re-arm unless shutdown made further retries impossible.
     * The caller still reports the original receipt's failure; this method only schedules.
     */
    retry_completion_e
    complete(retry_token_t token, const command_result_t &result, bool retry_finished, monotonic_time_t now);

    std::optional<monotonic_time_t>
    next_wake() const;

  private:
    void
    advance_generation();

    monotonic_time_t delay_;
    std::uint64_t generation_ {};
    std::optional<monotonic_time_t> due_;
    std::optional<retry_token_t> in_flight_;
  };
}  // namespace display_device::topology
