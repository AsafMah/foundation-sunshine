#include "topology_routes.h"

#include <limits>
#include <stdexcept>

namespace display_device::topology {
  retry_schedule_t::retry_schedule_t(monotonic_time_t delay):
      delay_ { delay } {
    if (delay_ <= monotonic_time_t::zero()) {
      throw std::invalid_argument { "Topology retry delay must be positive" };
    }
  }

  void
  retry_schedule_t::advance_generation() {
    if (generation_ == std::numeric_limits<std::uint64_t>::max()) {
      throw std::overflow_error { "Topology retry generation exhausted" };
    }
    ++generation_;
  }

  void
  retry_schedule_t::setup(bool enabled, monotonic_time_t now) {
    advance_generation();
    in_flight_.reset();
    due_ = enabled ? std::optional { now + delay_ } : std::nullopt;
  }

  std::optional<retry_token_t>
  retry_schedule_t::take_due(monotonic_time_t now) {
    if (!due_ || now < *due_) {
      return std::nullopt;
    }
    advance_generation();
    due_.reset();
    in_flight_ = retry_token_t { generation_ };
    return in_flight_;
  }

  bool
  retry_schedule_t::is_current(retry_token_t token) const {
    return in_flight_ == token;
  }

  retry_completion_e
  retry_schedule_t::complete(retry_token_t token, const command_result_t &result, bool retry_finished, monotonic_time_t now) {
    if (!is_current(token)) {
      return retry_completion_e::stale;
    }
    in_flight_.reset();
    const bool stopped = result.outcome == outcome_e::stopped ||
                         (result.outcome == outcome_e::rejected &&
                           (result.reason == reject_reason_e::draining || result.reason == reject_reason_e::stopped));
    if (stopped || (result.completed() && retry_finished)) {
      return retry_completion_e::disarmed;
    }
    due_ = now + delay_;
    return retry_completion_e::rearmed;
  }

  std::optional<monotonic_time_t>
  retry_schedule_t::next_wake() const {
    return due_;
  }
}  // namespace display_device::topology
