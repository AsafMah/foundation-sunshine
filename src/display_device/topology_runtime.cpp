/**
 * @file src/display_device/topology_runtime.cpp
 * @brief Binds the process topology lifecycle to the Sunshine session.
 */
#include "topology_runtime.h"

#include <memory>
#include <thread>
#include <vector>

#include "session.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/rtsp.h"
#include "topology_process_lifecycle.h"
#include "topology_toggle.h"
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

  std::optional<std::string>
  reset_persistence() {
    auto done = topology::run_for_value(coordinator(), topology::command_kind_e::reset_persistence,
      [](const topology::mutation_capability_t &capability) {
        session_t::get().reset_persistence(capability, automatic_vdd_policy());
        return true;
      });
    if (!done) {
      BOOST_LOG(warning) << "Display device reset was not run: " << done.error().detail;
      return std::move(done.error().detail);
    }
    return std::nullopt;
  }

  namespace {
    std::expected<bool, std::string>
    run_vdd(topology::command_kind_e kind, bool (session_t::*mutate)(const topology::mutation_capability_t &)) {
      auto done = topology::run_for_value(coordinator(), kind, [mutate](const topology::mutation_capability_t &capability) {
        return (session_t::get().*mutate)(capability);
      });
      if (!done) {
        BOOST_LOG(warning) << "VDD " << topology::to_string(kind) << " was not run: " << done.error().detail;
        return std::unexpected(std::move(done.error().detail));
      }
      return *done;
    }

    bool
    extend_toggled(std::uint64_t generation, const std::string &device, const std::vector<std::string> &physical) noexcept {
      try {
        const auto done = topology::run_for_value(coordinator(), topology::command_kind_e::vdd_toggle_followup,
          [&](const topology::mutation_capability_t &capability) {
            return generation == vdd_utils::vdd_generation() && vdd_utils::ensure_vdd_extended_mode(capability, device, physical);
          });
        if (!done) {
          BOOST_LOG(warning) << "VDD extended topology was not run: " << done.error().detail;
        }
        return done && *done;
      }
      catch (const std::exception &e) {
        BOOST_LOG(error) << "VDD extended topology failed: " << e.what();
      }
      catch (...) {
        BOOST_LOG(error) << "VDD extended topology failed with an unknown exception";
      }
      return false;
    }

    void
    destroy_toggled(std::uint64_t generation) noexcept {
      try {
        const auto reason = coordinator().post(topology::command_kind_e::vdd_toggle_followup,
          [generation](const topology::mutation_capability_t &capability, const topology::command_context_t &) {
            if (generation == vdd_utils::vdd_generation()) {
              vdd_utils::destroy_vdd_monitor(capability);
            }
          });
        if (reason != topology::reject_reason_e::none) {
          BOOST_LOG(warning) << "Unconfirmed VDD cleanup was not queued: " << topology::to_string(reason);
        }
      }
      catch (const std::exception &e) {
        BOOST_LOG(error) << "Unconfirmed VDD cleanup failed: " << e.what();
      }
      catch (...) {
        BOOST_LOG(error) << "Unconfirmed VDD cleanup failed with an unknown exception";
      }
    }
  }  // namespace

  std::expected<bool, std::string>
  create_vdd_monitor_noninteractive() {
    return run_vdd(topology::command_kind_e::vdd_create, &session_t::create_vdd_monitor_noninteractive);
  }

  std::expected<bool, std::string>
  destroy_vdd_monitor() {
    return run_vdd(topology::command_kind_e::vdd_destroy, &session_t::destroy_vdd_monitor);
  }

  bool
  toggle_display_power() {
    using vdd_utils::toggle_outcome_e;
    using vdd_utils::toggle_request_t;
    using vdd_utils::toggle_result_t;
    try {
      return topology::run_toggle(coordinator(),
        topology::toggle_steps_t<toggle_request_t, toggle_result_t> {
          vdd_utils::prepare_display_toggle,
          vdd_utils::reject_display_toggle,
          vdd_utils::apply_display_toggle,
          [](const toggle_result_t &result) {
            return result.outcome == toggle_outcome_e::created || result.outcome == toggle_outcome_e::destroyed;
          },
          [](const toggle_result_t &result) {
            if (result.outcome == toggle_outcome_e::created) {
              try {
                std::thread { [result] {
                  vdd_utils::confirm_display_toggle(result, { extend_toggled, destroy_toggled });
                } }.detach();
              }
              catch (...) {
                // Without the keep-confirmation the new VDD would stay unconfirmed forever.
                destroy_toggled(result.generation);
                throw;
              }
            }
          },
        });
    }
    catch (const std::exception &e) {
      BOOST_LOG(error) << "VDD toggle failed: " << e.what();
    }
    catch (...) {
      BOOST_LOG(error) << "VDD toggle failed with an unknown exception";
    }
    return false;
  }
}  // namespace display_device::coordinated
