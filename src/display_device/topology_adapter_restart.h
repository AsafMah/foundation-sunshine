/**
 * @file src/display_device/topology_adapter_restart.h
 * @brief Last-resort adapter restart run as one supervised external actor.
 *
 * The restart runs as one actor under one durable intent. Inside it, a creation
 * that definitely failed is retried up to `retry.attempts` times, matching the
 * legacy bounded retry. Once a process may exist the result is final, because a
 * launch whose creation is uncertain may already be running. The caller's restoration is
 * not run inline. It is handed to the coordinator, which runs it ahead of all
 * pending work only after the restart has durably settled. Until then the
 * coordinator stays fenced.
 */
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "topology_actor_body.h"
#include "topology_actor_cleanup.h"

namespace display_device::topology {

  struct adapter_restart_plan_t {
    std::shared_ptr<actor_record_store_t> store;
    /** Unique per restart, including across process restarts. */
    std::uint64_t operation {};
    /** Trustworthy boot identity, or unknown. */
    std::optional<std::string> boot;
    /** Runs on the supervisor's body thread. */
    actor_process_factory_t process;
    /** Bounded retry of definite creation failures, inside the one supervised actor. */
    actor_launch_retry_t retry;
    /** `cleanup_sink` is replaced. */
    actor_supervisor_options_t options;
  };

  enum class adapter_restart_e : std::uint8_t {
    /** Nothing was launched and no obligation was taken; the caller restores now. */
    not_started,
    /** The coordinator owns the restoration and runs it first once the restart settles. */
    cleanup_owned,
    /** The restoration cannot be delivered; the coordinator stays fenced and the caller must not restore. */
    wedged,
  };

  struct adapter_restart_result_t {
    adapter_restart_e status { adapter_restart_e::not_started };
    std::optional<actor_launch_result_t> launch;
    std::string detail;
    /** Alive while the restart is running or blocked; released once it settles. */
    std::weak_ptr<actor_supervisor_t> supervisor;
  };

  /**
   * Call from the running command that owns `capability`, which must allow
   * `adapter_restart`. `restore` runs later, as a `launch_failed_restore`
   * command, and must report whether the restoration succeeded. Throws, before
   * any side effect, when the coordinator refuses the obligation: no authority,
   * another coordinator's capability, draining, or an earlier obligation that is
   * still undischarged.
   */
  adapter_restart_result_t
  restart_adapter(coordinator_t &coordinator, const mutation_capability_t &capability, adapter_restart_plan_t plan,
    external_cleanup_body_t restore);

}  // namespace display_device::topology
