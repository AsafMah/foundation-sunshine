/**
 * @file src/display_device/topology_call.h
 * @brief Run a value-returning topology mutation through the coordinator.
 */
#pragma once

#include <exception>
#include <expected>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

#include "topology_coordinator.h"

namespace display_device::topology {

  /** The coordinator did not run the body to completion, and it did not throw. */
  struct call_refused_t {
    command_result_t result;
    std::string detail;
  };

  /**
   * Run `body(capability)` as one `kind` command and wait for it. A body that
   * returned yields its value, even if it finished after a cancellation, because
   * its mutation happened. A body exception is rethrown. Anything else (rejected,
   * cancelled, stopped, reentrant) is a refusal; the body may not have run.
   */
  template <class F>
  auto
  run_for_value(coordinator_t &coordinator, command_kind_e kind, F &&body)
    -> std::expected<std::invoke_result_t<F &, const mutation_capability_t &>, call_refused_t> {
    using value_t = std::invoke_result_t<F &, const mutation_capability_t &>;
    std::optional<value_t> value;
    auto result = coordinator.run(kind, [&](const mutation_capability_t &capability, const command_context_t &) {
      value.emplace(body(capability));
    });
    if (result.exception) {
      std::rethrow_exception(result.exception);
    }
    if (value) {
      return std::move(*value);
    }
    std::string detail { to_string(result.outcome) };
    if (result.reason != reject_reason_e::none) {
      detail += " / ";
      detail += to_string(result.reason);
    }
    return std::unexpected(call_refused_t { result, std::move(detail) });
  }

}  // namespace display_device::topology
