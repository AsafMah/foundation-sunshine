/**
 * @file src/nvhttp_stream_start.h
 * @brief Helpers for GameStream launch/resume display preparation, encoder probing, and startup recovery.
 */
#pragma once

#include "display_device/settings.h"
#include "display_device/topology_routes.h"
#include "rtsp.h"

#include <boost/property_tree/ptree.hpp>
#include <string>

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
    std::function<display_device::topology::native_prepared_t(const display_device::topology::mutation_capability_t &)> managed_prepare;
  };

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
