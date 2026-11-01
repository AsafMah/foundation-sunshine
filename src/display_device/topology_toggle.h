/**
 * @file src/display_device/topology_toggle.h
 * @brief The user-facing VDD toggle as one coordinated command.
 */
#pragma once

#include <functional>
#include <optional>

#include "topology_call.h"

namespace display_device::topology {

  template <class Request, class Result>
  struct toggle_steps_t {
    /** Caller thread: debounce and any confirmation prompt. Empty means do nothing. */
    std::function<std::optional<Request>()> prepare;
    /** Caller thread: undo `prepare`'s reservation when the command never ran. */
    std::function<void(const Request &)> release;
    /** Worker: the mutation itself. */
    std::function<Result(const mutation_capability_t &, const Request &)> apply;
    std::function<bool(const Result &)> applied;
    /** Caller thread, after the command finished: start any follow-up; must not wait on the worker's queue. */
    std::function<void(const Result &)> follow_up;
  };

  /**
   * Prompt, then run one `vdd_toggle` command and wait for it, as the legacy
   * synchronous toggle did. A refused command releases the reservation and
   * returns false. A body exception propagates and keeps the reservation.
   * Blocks the caller: never call it from a thread the worker may synchronously
   * wait on (the native tray thread, whose window receives `tray_update`).
   */
  template <class Request, class Result>
  bool
  run_toggle(coordinator_t &coordinator, const toggle_steps_t<Request, Result> &steps) {
    const auto request = steps.prepare();
    if (!request) {
      return false;
    }
    const auto result = run_for_value(coordinator, command_kind_e::vdd_toggle,
      [&](const mutation_capability_t &capability) { return steps.apply(capability, *request); });
    if (!result) {
      steps.release(*request);
      return false;
    }
    if (steps.follow_up) {
      steps.follow_up(*result);
    }
    return steps.applied(*result);
  }

}  // namespace display_device::topology
