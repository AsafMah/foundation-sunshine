/**
 * @file src/display_device/topology_runtime.h
 * @brief Process-lifetime topology owner and production entry routes.
 */
#pragma once

#include <expected>
#include <optional>
#include <string>
#include <string_view>

#include "settings.h"
#include "topology_coordinator.h"
#include "topology_native.h"

namespace display_device::coordinated {
  topology::coordinator_t &coordinator();
  automatic_vdd_policy_e automatic_vdd_policy();
  void start();
  /** Restore displays after already-queued work (bounded); new topology work is rejected afterwards. */
  void shutdown_restore();
  void shutdown_vdd_destroy();
  /** Managed launches only: prepare the latched native profile on the topology worker. */
  topology::native_prepared_t prepare_native(const topology::mutation_capability_t &capability);
  /** Read-only admin report of the saved `display_topology_profile` against the one this process latched. */
  topology::topology_status_report_t status_report(std::string_view desired_setting);

  // User-requested routes. Each waits for its command; a refusal is returned as
  // "outcome / reason" and nothing ran. Exceptions from the mutation propagate.
  std::optional<std::string> reset_persistence();
  std::expected<bool, std::string> create_vdd_monitor_noninteractive();
  std::expected<bool, std::string> destroy_vdd_monitor();
  /** Hotkey/tray toggle: prompts on the caller thread; true when the VDD was created or destroyed. Never throws. */
  bool toggle_display_power();
}
