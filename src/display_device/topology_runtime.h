/**
 * @file src/display_device/topology_runtime.h
 * @brief Process-lifetime topology owner and production entry routes.
 */
#pragma once

#include "settings.h"
#include "topology_coordinator.h"

namespace display_device::coordinated {
  topology::coordinator_t &coordinator();
  automatic_vdd_policy_e automatic_vdd_policy();
  void start();
  /** Restore displays after already-queued work (bounded); new topology work is rejected afterwards. */
  void shutdown_restore();
  void shutdown_vdd_destroy();
}
