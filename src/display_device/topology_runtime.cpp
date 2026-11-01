/**
 * @file src/display_device/topology_runtime.cpp
 * @brief Binds the process topology lifecycle to the Sunshine session.
 */
#include "topology_runtime.h"

#include <memory>

#include "session.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/rtsp.h"
#include "topology_process_lifecycle.h"
#include "vdd_utils.h"
#ifdef _WIN32
  #include "src/platform/windows/topology_actor_observer.h"
#endif

namespace display_device::coordinated {
  namespace {
    topology::process_observation_e
    observe(const topology::actor_receipt_t &receipt) {
#ifdef _WIN32
      return platf::observe_actor_process(receipt);
#else
      (void) receipt;
      return topology::process_observation_e::inaccessible;
#endif
    }

    /**
     * Leaked on purpose: the coordinator is stopped by the session deinit guard,
     * and no static destructor may join its threads after main returns.
     * Construction must not touch `session_t::get()`, which constructs its retry
     * timer through `coordinator()`; the hooks only call it later.
     */
    topology::process_lifecycle_t &
    lifecycle() {
      static auto *const instance = [] {
        topology::coordinator_options_t options;
        options.tick_interval = std::chrono::seconds { 1 };
        options.probes.startup_restore_quiescent = [] { return !rtsp_stream::session_starting_or_active(); };
        options.probes.legacy_restore_pending = [] { return session_t::get().restore_pending(); };
        options.probes.ticket_present = [](std::uint32_t id) { return rtsp_stream::launch_ticket_present(id); };

        topology::lifecycle_hooks_t hooks;
        hooks.startup_restore = [](const auto &capability, const auto &) {
          session_t::get().restore_state(capability, automatic_vdd_policy());
        };
        hooks.shutdown_restore = [](const auto &capability, const auto &) {
          session_t::get().restore_state(capability, automatic_vdd_policy());
        };
        hooks.quiet_vdd_destroy = [](const auto &capability, const auto &) {
          vdd_utils::destroy_vdd_monitor_nolog(capability);
        };
        hooks.keep_vdd = [] { return config::video.vdd_keep_enabled; };
        hooks.actor.store = std::make_shared<topology::file_actor_record_store_t>(vdd_utils::adapter_restart_record_path());
        hooks.actor.observe = observe;
        hooks.report = [](const std::string &line) { BOOST_LOG(warning) << "Display topology: " << line; };
        return new topology::process_lifecycle_t { std::move(options), std::move(hooks) };
      }();
      return *instance;
    }
  }  // namespace

  topology::coordinator_t &
  coordinator() {
    return lifecycle().coordinator();
  }

  automatic_vdd_policy_e
  automatic_vdd_policy() {
    return lifecycle().suppress_automatic_vdd() ? automatic_vdd_policy_e::suppress : automatic_vdd_policy_e::allow;
  }

  void
  start() {
    const auto result = lifecycle().start();
    if (result != topology::lifecycle_start_e::completed) {
      BOOST_LOG(warning) << "Display topology startup: " << topology::to_string(result);
    }
  }

  void
  shutdown_restore() {
    try {
      const auto result = lifecycle().shutdown_restore();
      if (result != topology::lifecycle_cleanup_e::completed) {
        BOOST_LOG(warning) << "Display restoration at shutdown: " << topology::to_string(result);
      }
    }
    catch (const std::exception &e) {
      BOOST_LOG(error) << "Display restoration at shutdown failed: " << e.what();
    }
  }

  void
  shutdown_vdd_destroy() {
    const auto result = lifecycle().shutdown_vdd_destroy();
    if (result != topology::lifecycle_cleanup_e::completed && result != topology::lifecycle_cleanup_e::skipped) {
      BOOST_LOG(warning) << "Final VDD cleanup at shutdown: " << topology::to_string(result);
    }
  }
}  // namespace display_device::coordinated
