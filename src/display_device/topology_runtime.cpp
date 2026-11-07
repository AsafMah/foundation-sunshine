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
#include "src/process.h"
#include "src/rtsp.h"
#include "src/stream.h"
#include "topology_native.h"
#include "topology_process_lifecycle.h"
#include "topology_toggle.h"
#include "vdd_utils.h"
#ifdef _WIN32
  #include "src/globals.h"
  #include "src/platform/windows/display_device/native_edge.h"
  #include "src/platform/windows/topology_actor_observer.h"
  #include "src/uuid.h"
  #include "topology_journal_store.h"
  #include "topology_windows_native.h"
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

    /** nullptr fails a managed profile closed: native topology is Windows-only. */
    std::shared_ptr<topology::native_executor_t>
    make_native_executor() {
#ifdef _WIN32
      struct bundle_t {
        topology::file_journal_store_t store { platf::appdata() / "display_topology_journal.json" };
        topology::win32_edge::win32_native_edge_t edge;
        topology::windows_native_platform_t platform { edge, platform_options() };

        static topology::windows_native_options_t
        platform_options() {
          topology::windows_native_options_t options { topology::zako_vdd_driver, ZAKO_NAME };
          options.note = [](const std::string &line) { BOOST_LOG(info) << "Display topology: " << line; };
          return options;
        }
        topology::native_executor_t executor {
          store,
          platform,
          {
            [] {
              const auto uuid = uuid_util::uuid_t::generate();
              topology::uuid_bytes_t bytes;
              std::copy(std::begin(uuid.b8), std::end(uuid.b8), bytes.begin());
              return bytes;
            },
            2,
            topology::zako_vdd_driver,
          },
        };
      };
      auto bundle = std::make_shared<bundle_t>();
      return { bundle, &bundle->executor };
#else
      return nullptr;
#endif
    }

    struct latched_binding_t {
      /** The setting this process latched, for the admin Desired/Applied report. */
      std::string setting;
      std::shared_ptr<topology::managed_native_t> binding;
    };

    /**
     * The `display_topology_profile` latched for this process: a null binding is legacy mode.
     * Leaked with the lifecycle that uses it.
     */
    const latched_binding_t &
    latched() {
      static auto *const instance = [] {
        auto setting = config::video.display_topology_profile;
        auto binding = topology::bind_native_profile(setting, make_native_executor);
        return new latched_binding_t { std::move(setting), std::move(binding) };
      }();
      return *instance;
    }

    const std::shared_ptr<topology::managed_native_t> &
    native_binding() {
      return latched().binding;
    }

    std::optional<topology::native_lifecycle_hooks_t>
    native_hooks() {
      const auto &managed = native_binding();
      if (!managed) {
        return std::nullopt;
      }
      return topology::native_lifecycle_hooks_t {
        [managed] { return managed->health(); },
        [managed](const auto &capability) { return managed->recover(capability); },
        [managed](const auto &capability) { return managed->restore(capability); },
        [managed](const auto &capability) { return managed->reset(capability); },
        [managed](const auto &capability, std::uint8_t heads) {
          // One head is an ordinary launch of any valid profile; a group needs the profile's offer.
          auto outcome = heads <= 1 ? managed->prepare(capability) : managed->prepare(capability, heads);
          if (outcome.verdict != topology::native_verdict_e::ok || !outcome.target) {
            auto reason = std::string { topology::to_string(outcome.verdict) };
            if (!outcome.reason.empty()) {
              reason += ": " + outcome.reason;
            }
            return topology::native_prepared_t { std::nullopt, std::move(reason) };
          }
          const auto target_of = [](const topology::native_target_t &head) {
            topology::managed_target_t target { head.display.value, head.virtual_head };
            if (head.layout) {
              target.geometry = topology::to_head_geometry(*head.layout);
            }
            return target;
          };
          topology::native_prepared_t prepared { target_of(*outcome.target), {} };
          for (const auto &head : outcome.targets) {
            prepared.heads.push_back(target_of(head));
          }
          if (prepared.heads.empty()) {
            prepared.heads.push_back(*prepared.display);
          }
          return prepared;
        },
        [managed](const auto &capability, std::size_t head, std::uint32_t width, std::uint32_t height) {
          // Per head: with one owned head this is exactly the single-head change.
          auto outcome = managed->change_mode(capability, head, resolution_t { width, height });
          topology::native_mode_changed_t changed;
          switch (outcome.verdict) {
            case topology::mode_change_verdict_e::applied:
              changed.verdict = topology::native_mode_verdict_e::applied;
              break;
            case topology::mode_change_verdict_e::unchanged:
              changed.verdict = topology::native_mode_verdict_e::unchanged;
              break;
            case topology::mode_change_verdict_e::refused:
              changed.verdict = topology::native_mode_verdict_e::refused;
              break;
            case topology::mode_change_verdict_e::conflict:
            default:
              changed.verdict = topology::native_mode_verdict_e::conflict;
              break;
          }
          changed.display = outcome.display ? outcome.display->value : std::string {};
          changed.reason = std::move(outcome.reason);
          if (outcome.layout) {
            changed.geometry = topology::to_head_geometry(*outcome.layout);
          }
          return changed;
        },
      };
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
        // Video sessions only, and without side effects: rtsp's session_count() also counts
        // control-only sessions and clears stopped ones.
        options.probes.active_streams = [] { return stream::session::has_active_video_sessions() ? std::size_t { 1 } : std::size_t { 0 }; };
        options.probes.app_running = [] { return proc::proc.running() > 0; };
        options.group_ended = [](const std::vector<std::uint32_t> &members) {
          BOOST_LOG(info) << "Display group ended; terminating " << members.size() << " member stream(s)";
          rtsp_stream::terminate_launch_sessions(members);
        };

        topology::lifecycle_hooks_t hooks;
        hooks.native = native_hooks();
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

  topology::native_prepared_t
  prepare_native(const topology::mutation_capability_t &capability, std::uint8_t heads) {
    return lifecycle().prepare_native(capability, heads);
  }

  topology::native_mode_changed_t
  change_session_native_mode(std::uint32_t launch_session_id, std::uint32_t width, std::uint32_t height) {
    return lifecycle().change_session_native_mode(launch_session_id, width, height);
  }

  std::uint64_t
  display_group_revision() {
    return coordinator().display_group_revision();
  }

  std::optional<topology::group_layout_t>
  display_group_layout(std::uint32_t launch_session_id) {
    return coordinator().display_group_layout(launch_session_id);
  }

  std::uint8_t
  display_group_max_heads() {
    const auto &managed = native_binding();
    return managed ? managed->group_max_heads() : 0;
  }

  const std::string &
  latched_profile() {
    return latched().setting;
  }

  topology::topology_status_report_t
  status_report(std::string_view desired_setting) {
    const auto status = coordinator().status();
    return topology::describe_topology_status(desired_setting, latched().setting, status);
  }

  std::optional<std::string>
  reset_persistence() {
    auto done = topology::run_for_value(coordinator(), topology::command_kind_e::reset_persistence,
      [](const topology::mutation_capability_t &capability) {
        session_t::get().reset_persistence(capability, automatic_vdd_policy());
        return lifecycle().reset_native(capability);
      });
    if (!done) {
      BOOST_LOG(warning) << "Display device reset was not run: " << done.error().detail;
      return std::move(done.error().detail);
    }
    if (*done) {
      BOOST_LOG(warning) << "Display device reset left the native topology journal: " << **done;
      return std::move(*done);
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
