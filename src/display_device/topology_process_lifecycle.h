/**
 * @file src/display_device/topology_process_lifecycle.h
 * @brief Process lifecycle of the topology coordinator: startup, shutdown restore and final VDD cleanup.
 *
 * Neutral (no Sunshine globals) so the ordering and bounding rules are unit tested;
 * `topology_runtime.cpp` binds the hooks to the Sunshine session.
 *
 * Startup recovers the durable external actor record before the legacy startup
 * restoration runs. A recorded actor that is still alive keeps startup suspended,
 * so every wait here is bounded: the process continues (fenced) instead of hanging,
 * and shutdown cleanup that cannot run in time is left to the next startup.
 */
#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "topology_actor_startup.h"
#include "topology_coordinator.h"

namespace display_device::topology {

  enum class lifecycle_start_e : std::uint8_t {
    /** Startup recovery and the startup restoration completed. */
    completed,
    /** Startup ran but failed; topology is In conflict, legacy streaming continues. */
    failed,
    /** Still suspended behind a recorded external actor; topology stays fenced. */
    suspended,
    /** Still running past the bound for another reason. */
    timed_out,
  };

  enum class lifecycle_cleanup_e : std::uint8_t {
    completed,
    /** Not attempted by policy (`vdd_keep_enabled`) or already done. */
    skipped,
    /** Not admitted (for example ecovery_required while a native journal exists). */
    rejected,
    /** Admitted but did not complete; the description carries the outcome and reason. */
    failed,
    /** Not finished within the bound; stop() cancels it if still queued. */
    timed_out,
  };

  const char *
  to_string(lifecycle_start_e value) noexcept;
  const char *
  to_string(lifecycle_cleanup_e value) noexcept;

  enum class native_mode_verdict_e : std::uint8_t {
    /** The head now runs the requested mode. */
    applied,
    /** The head already ran the requested mode; nothing changed. */
    unchanged,
    /** Refused before anything changed; the head keeps its mode. */
    refused,
    /** The change failed in a way that left the head's state uncertain (In conflict). */
    conflict,
  };

  struct native_mode_changed_t {
    native_mode_verdict_e verdict { native_mode_verdict_e::refused };
    /** The head's display after the change; empty when unknown. */
    std::string display;
    std::string reason;
  };

  /**
   * Managed (native) mode. Present selects managed mode for the whole process: every
   * hook is required, and each returned health is published to the coordinator.
   */
  struct native_lifecycle_hooks_t {
    /** Health published at construction, before startup recovery. */
    std::function<native_health_t()> health;
    /** Runs first in startup recovery, before actor recovery and the legacy startup restoration. */
    std::function<native_health_t(const mutation_capability_t &)> recover;
    /** The managed restoration: lease end and shutdown. Must be idempotent. */
    std::function<native_health_t(const mutation_capability_t &)> restore;
    /** Administrator reset (`reset_persistence` authority). */
    std::function<native_health_t(const mutation_capability_t &)> reset;
    /** Managed launch preparation (`launch_prepare` authority); `health` is published after it. */
    std::function<native_prepared_t(const mutation_capability_t &)> prepare;
    /**
     * Optional: change the managed head's resolution (`native_mode` authority);
     * `health` is published after it. Absent refuses every mode change.
     */
    std::function<native_mode_changed_t(const mutation_capability_t &, std::uint32_t width, std::uint32_t height)> change_mode;
  };

  struct lifecycle_hooks_t {
    /** Required: the legacy startup restoration, run with startup authority after actor recovery. */
    command_body_t startup_restore;
    /** Required: the shutdown restoration. */
    command_body_t shutdown_restore;
    /** Required: the quiet final VDD destroy. */
    command_body_t quiet_vdd_destroy;
    /** Required: whether the final VDD destroy is disabled by configuration. */
    std::function<bool()> keep_vdd;
    /**
     * Actor record recovery. `store` and `observe` are required. An empty
     * `restore_pending` defaults to the coordinator's deferred startup restoration.
     */
    actor_startup_options_t actor;
    /** Diagnostics; may be called from the worker, the watcher or the caller. */
    std::function<void(const std::string &)> report;
    /** Empty for legacy mode (unchanged behaviour). */
    std::optional<native_lifecycle_hooks_t> native;
    std::chrono::milliseconds startup_wait { 30000 };
    std::chrono::milliseconds shutdown_restore_wait { 4000 };
    std::chrono::milliseconds vdd_destroy_wait { 3000 };
  };

  class process_lifecycle_t {
  public:
    /** @throws std::invalid_argument when a required hook is missing. */
    process_lifecycle_t(coordinator_options_t options, lifecycle_hooks_t hooks);

    process_lifecycle_t(const process_lifecycle_t &) = delete;
    process_lifecycle_t &
    operator=(const process_lifecycle_t &) = delete;

    coordinator_t &
    coordinator() noexcept { return coordinator_; }

    /** Legacy automatic VDD destroys are suppressed while a native journal may own a head. */
    bool
    suppress_automatic_vdd() const;

    /** Start the coordinator and wait (bounded) for startup recovery. */
    lifecycle_start_e
    start();

    /** Reject new work, then run the shutdown restoration after everything already queued (bounded). */
    lifecycle_cleanup_e
    shutdown_restore();

    /**
     * Final quiet VDD destroy unless kept (bounded), then stop the coordinator. Idempotent.
     * The waits bound queued work only: a body that has already started is never
     * abandoned while process teardown continues, so `stop()` joins it and the
     * process shutdown watchdog is the outer bound.
     */
    lifecycle_cleanup_e
    shutdown_vdd_destroy();

    /** Whether the process was latched into managed (native) mode. */
    bool
    managed() const noexcept {
      return hooks_.native.has_value();
    }

    /**
     * In managed mode, run the native reset under `capability` and publish its health; otherwise nothing.
     * @return Why the reset failed when the journal is still present; nullopt otherwise. A profile that
     *         keeps the process In conflict after the journal was cleared is not a failed reset.
     */
    std::optional<std::string>
    reset_native(const mutation_capability_t &capability);

    /**
     * Prepare a managed launch under `capability` and publish the resulting health,
     * also when the preparation throws (In conflict). Legacy mode refuses without running anything.
     */
    native_prepared_t
    prepare_native(const mutation_capability_t &capability);

    /**
     * Change the managed head's resolution under `capability` and publish the resulting
     * health; a throwing change publishes In conflict and returns `conflict`. Legacy mode,
     * or no change hook, refuses without running anything.
     */
    native_mode_changed_t
    change_native_mode(const mutation_capability_t &capability, std::uint32_t width, std::uint32_t height);

  private:
    coordinator_options_t
    with_native(coordinator_options_t options);
    /** Run a native hook and publish its health; an exception publishes In conflict and propagates. */
    native_health_t
    run_native(const mutation_capability_t &capability,
      const std::function<native_health_t(const mutation_capability_t &)> &hook, const char *failure);
    lifecycle_cleanup_e
    run_bounded(command_kind_e kind, command_body_t body, std::chrono::milliseconds wait);
    void
    report(const std::string &line) const;

    lifecycle_hooks_t hooks_;
    coordinator_t coordinator_;
    std::mutex shutdown_mutex_;
    bool shut_down_ {};
  };

}  // namespace display_device::topology
