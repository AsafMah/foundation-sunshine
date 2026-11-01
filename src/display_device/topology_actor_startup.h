/**
 * @file src/display_device/topology_actor_startup.h
 * @brief Startup recovery of the durable external actor record.
 *
 * A previous process may have died while an external actor (the adapter restart
 * helper) was starting, running or owed cleanup. Startup suspends itself behind
 * the record before reading it, so a record that cannot be resolved keeps every
 * topology mutation fenced until stop instead of falling through to normal
 * operation. A recorded actor is adopted and waited for, never relaunched.
 *
 * Once the actor is gone, the rest of startup (which performs the legacy display
 * restoration) runs ahead of all queued work. The record's obligation is then
 * completed, or transferred to the legacy persistence while that restoration is
 * still deferred, and the terminal record is retired.
 */
#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "topology_actor_record.h"
#include "topology_actor_supervisor.h"
#include "topology_coordinator.h"

namespace display_device::topology {

  /** Durable owner named when the deferred legacy restoration takes over the obligation. */
  inline constexpr const char *legacy_restore_owner = "legacy_display_settings";

  struct actor_startup_options_t {
    std::shared_ptr<actor_record_store_t> store;
    /** Trustworthy boot identity now, or unknown. */
    std::optional<std::string> current_boot;
    process_observer_t observe;
    /** Runs the watcher of a still-running actor; empty uses a detached std::thread. The watcher ends once startup no longer waits (resumed, stopped or destroyed). */
    actor_spawn_t spawn;
    std::chrono::milliseconds poll_interval { 500 };
    /** Required: whether the legacy startup restoration is still deferred after startup ran. */
    std::function<bool()> restore_pending;
    /** Diagnostics; called from the worker or the watcher thread. */
    std::function<void(const std::string &)> report;
  };

  /**
   * The first statement of the startup recovery body. `startup` becomes the
   * startup continuation and runs with exact startup authority once any recorded
   * actor is gone, ahead of all queued work. Returns the initial classification;
   * `blocked` means topology stays fenced until stop.
   */
  actor_recovery_e
  recover_startup_actor(coordinator_t &coordinator, const mutation_capability_t &capability, actor_startup_options_t options,
    command_body_t startup);

}  // namespace display_device::topology
