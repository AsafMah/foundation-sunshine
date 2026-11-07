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
#include "topology_process_lifecycle.h"

namespace display_device::coordinated {
  topology::coordinator_t &coordinator();
  automatic_vdd_policy_e automatic_vdd_policy();
  void start();
  /** Restore displays after already-queued work (bounded); new topology work is rejected afterwards. */
  void shutdown_restore();
  void shutdown_vdd_destroy();
  /** Managed launches only: prepare the latched native profile on the topology worker. */
  topology::native_prepared_t prepare_native(const topology::mutation_capability_t &capability, std::uint8_t heads = 1);
  /**
   * Managed streams only: change the resolution of the head launch session
   * `launch_session_id` streams (its own head; a display-group member's own head), as
   * the managed lease's admitted `reconfigure`. Refused, without touching any head, when
   * not admitted.
   */
  topology::native_mode_changed_t change_session_native_mode(std::uint32_t launch_session_id, std::uint32_t width, std::uint32_t height);
  /** The most heads a display-group launch may ask for; 0 in legacy mode or without a usable virtual profile. */
  std::uint8_t display_group_max_heads();
  /** The `display_topology_profile` this process latched; blank in legacy mode. */
  const std::string &latched_profile();
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
