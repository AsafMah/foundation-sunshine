/**
 * @file src/nvhttp_stream_start.h
 * @brief Helpers for GameStream launch/resume display preparation, encoder probing, and startup recovery.
 */
#pragma once

#include "display_device/settings.h"
#include "display_device/topology_routes.h"
#include "nvhttp_launch_cooldown.h"
#include "rtsp.h"

#include <boost/property_tree/ptree.hpp>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace nvhttp::stream_start {

  /**
   * Coordinated staged preparation. The coordinator and cleanup captures must outlive
   * late mutations; the bundle itself is borrowed only for this synchronous call.
   * fatal_probe_timeout must terminate production nonzero without blocking.
   * Legacy tokens need legacy_cleanup and no managed_prepare; managed tokens need
   * managed_prepare (run under the launch_prepare capability) and the coordinator in
   * managed mode, and their expiry cleanup is the coordinator's native restore.
   */
  struct launch_coordination_t {
    display_device::topology::coordinator_t &coordinator;
    display_device::topology::launch_token_t token;
    display_device::topology::command_body_t legacy_cleanup;
    std::function<void()> fatal_probe_timeout;
    display_device::topology::probe_thread_factory_t spawn_probe;
    /** The runtime's automatic-VDD policy, latched by the caller for this launch. */
    display_device::automatic_vdd_policy_e vdd_policy { display_device::automatic_vdd_policy_e::suppress };
    /** Prepares `heads` heads (1, or display_heads for a group leader) under the launch_prepare capability. */
    std::function<display_device::topology::native_prepared_t(const display_device::topology::mutation_capability_t &, std::uint8_t heads)> managed_prepare;
    /** Managed only, and required there: the failure cooldown and the profile target it is keyed on. */
    std::shared_ptr<launch_cooldown_t> cooldown;
    std::string cooldown_key;
    /**
     * Managed only: 2..max_group_heads makes this launch a display-group leader that
     * prepares every head, all or nothing; 0 or 1 is an ordinary launch.
     */
    std::uint8_t display_heads {};
    /** Required for a group leader: a fresh, never all-zero group id. */
    std::function<display_device::topology::display_group_id_t()> group_id;
  };

  /**
   * The /launch displayHeads argument: plain decimal 2..`served` (and never beyond
   * the wire limit), or nothing when the request must be refused.
   */
  std::optional<std::uint8_t>
  parse_display_heads(std::string_view value, std::uint8_t served);

  /** The group id as the client sees it: 32 lowercase hex digits, bytes in order. */
  std::string
  display_group_hex(const display_device::topology::display_group_id_t &id);

  /** A cryptographically random group id that is never all zero. */
  display_device::topology::display_group_id_t
  random_display_group_id();

  void
  set_sunshine_error(
    boost::property_tree::ptree &tree,
    int status_code,
    const std::string &status_message,
    const std::string &error_code,
    const std::string &hint,
    const std::string &recovery_action,
    const std::string &source,
    const std::string &stage,
    bool recoverable);

  /**
   * A coordinated success seals preparation, not ticket publication. The caller
   * still owns abort/bind and any application it starts. Invalid bundles throw
   * before ownership is adopted, including a missing bundle: every display
   * mutation needs the coordinator's capability, so there is no direct path.
   */
  bool
  prepare_display_and_probe_encoders(
    boost::property_tree::ptree &tree,
    std::shared_ptr<rtsp_stream::launch_session_t> launch_session,
    bool is_reconfigure,
    const launch_coordination_t *coordination);

  /**
   * A managed reference joining an existing managed lease (resume, or a launch beside
   * another stream): adopt its recorded target for exact capture, or fail with a 503
   * when it has none. A joiner that then starts an app holds its own attachment.
   */
  bool
  join_managed_display(
    boost::property_tree::ptree &tree,
    rtsp_stream::launch_session_t &launch_session,
    display_device::topology::coordinator_t &coordinator,
    const display_device::topology::launch_token_t &token);

}  // namespace nvhttp::stream_start
