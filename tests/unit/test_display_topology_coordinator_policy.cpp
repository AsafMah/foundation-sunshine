/**
 * @file tests/unit/test_display_topology_coordinator_policy.cpp
 * @brief Admission, lease, deadline and capability tests for the topology coordinator.
 *
 * Every body here is a fake: no display, CCD, DXGI, SetupAPI or VDD call is made.
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <ostream>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "src/display_device/topology_call.h"
#include "src/display_device/topology_toggle.h"
#include "src/display_device/topology_capability.h"
#include "src/display_device/topology_coordinator.h"

namespace display_device::topology {
  inline void
  PrintTo(outcome_e value, std::ostream *os) {
    *os << to_string(value);
  }

  inline void
  PrintTo(reject_reason_e value, std::ostream *os) {
    *os << to_string(value);
  }

  inline void
  PrintTo(coordinator_phase_e value, std::ostream *os) {
    *os << to_string(value);
  }

  inline void
  PrintTo(lease_stage_e value, std::ostream *os) {
    *os << to_string(value);
  }

  inline void
  PrintTo(command_kind_e value, std::ostream *os) {
    *os << to_string(value);
  }
}  // namespace display_device::topology

namespace {
  using namespace std::chrono_literals;
  using namespace display_device::topology;

  constexpr auto real_timeout = 5s;

  constexpr std::array all_kinds {
    command_kind_e::startup_recovery,
    command_kind_e::legacy_startup_restore,
    command_kind_e::launch_prepare,
    command_kind_e::launch_failed_restore,
    command_kind_e::reconfigure,
    command_kind_e::stream_restore,
    command_kind_e::shutdown_restore,
    command_kind_e::shutdown_vdd_destroy,
    command_kind_e::rotate,
    command_kind_e::scale,
    command_kind_e::vdd_toggle,
    command_kind_e::vdd_toggle_followup,
    command_kind_e::vdd_create,
    command_kind_e::vdd_destroy,
    command_kind_e::reset_persistence,
    command_kind_e::legacy_retry_tick,
    command_kind_e::legacy_unlock_restore,
    command_kind_e::native_restore,
  };

  constexpr std::array all_operations {
    operation_e::configure_display,
    operation_e::restore_state,
    operation_e::reset_persistence,
    operation_e::vdd_create,
    operation_e::vdd_destroy,
    operation_e::vdd_extended_mode,
    operation_e::rotate,
    operation_e::scale,
    operation_e::legacy_deferred,
    operation_e::native_apply,
    operation_e::native_restore,
    operation_e::native_recover,
    operation_e::write_topology,
    operation_e::write_display_modes,
    operation_e::write_primary,
    operation_e::write_hdr,
    operation_e::write_color_profile,
    operation_e::vdd_set_modes,
    operation_e::vdd_cursor,
    operation_e::adapter_restart,
  };

  constexpr std::array restoration_leaves {
    operation_e::write_topology,
    operation_e::write_display_modes,
    operation_e::write_primary,
    operation_e::write_hdr,
    operation_e::write_color_profile,
  };

  /** Spelled out independently of `is_cleanup_kind`, so a change there cannot make a test vacuous. */
  const std::set<command_kind_e> expected_cleanup_kinds {
    command_kind_e::launch_failed_restore,
    command_kind_e::stream_restore,
    command_kind_e::shutdown_restore,
    command_kind_e::shutdown_vdd_destroy,
    command_kind_e::native_restore,
  };

  class fake_clock_t {
  public:
    monotonic_time_t
    now() const {
      return monotonic_time_t { ticks_.load(std::memory_order_acquire) };
    }

    void
    advance(monotonic_time_t delta) {
      ticks_.fetch_add(delta.count(), std::memory_order_acq_rel);
    }

  private:
    std::atomic<monotonic_time_t::rep> ticks_ { 1000 };
  };

  /**
   * Blocks whoever calls `hook()` until released. State is shared, so bodies keep it
   * alive on their own; the destructor releases so a failed assertion cannot hang teardown.
   */
  class gate_t {
  public:
    gate_t() = default;
    gate_t(const gate_t &) = delete;
    gate_t &
    operator=(const gate_t &) = delete;

    ~gate_t() {
      release();
    }

    std::function<void()>
    hook() const {
      return [state = state_]() {
        state->enter();
      };
    }

    command_body_t
    body() const {
      return [state = state_](const mutation_capability_t &, const command_context_t &) {
        state->enter();
      };
    }

    bool
    wait_started() const {
      return state_->started_future.wait_for(real_timeout) == std::future_status::ready;
    }

    void
    release() {
      std::call_once(state_->release_once, [this]() { state_->released.set_value(); });
    }

  private:
    struct state_t {
      std::promise<void> started;
      std::shared_future<void> started_future { started.get_future().share() };
      std::once_flag started_once;
      std::promise<void> released;
      std::shared_future<void> released_future { released.get_future().share() };
      std::once_flag release_once;

      void
      enter() {
        std::call_once(started_once, [this]() { started.set_value(); });
        released_future.wait();
      }
    };

    std::shared_ptr<state_t> state_ { std::make_shared<state_t>() };
  };

  class finally_t {
  public:
    explicit finally_t(std::function<void()> action):
        action_ { std::move(action) } {}

    ~finally_t() {
      if (action_) {
        action_();
      }
    }

    finally_t(const finally_t &) = delete;
    finally_t &
    operator=(const finally_t &) = delete;

  private:
    std::function<void()> action_;
  };

  class event_log_t {
  public:
    void
    add(std::string event) {
      std::lock_guard lock { mutex_ };
      events_.push_back(std::move(event));
    }

    std::vector<std::string>
    events() const {
      std::lock_guard lock { mutex_ };
      return events_;
    }

    bool
    contains(const std::string &event) const {
      std::lock_guard lock { mutex_ };
      return std::find(events_.begin(), events_.end(), event) != events_.end();
    }

  private:
    mutable std::mutex mutex_;
    std::vector<std::string> events_;
  };

  struct posted_record_t {
    command_kind_e kind;
    outcome_e outcome;
    reject_reason_e reason;
    bool has_exception;
  };

  class posted_log_t {
  public:
    std::function<void(command_kind_e, const command_result_t &)>
    observer() {
      return [this](command_kind_e kind, const command_result_t &result) {
        std::lock_guard lock { mutex_ };
        records_.push_back({ kind, result.outcome, result.reason, static_cast<bool>(result.exception) });
      };
    }

    std::vector<posted_record_t>
    records() const {
      std::lock_guard lock { mutex_ };
      return records_;
    }

  private:
    mutable std::mutex mutex_;
    std::vector<posted_record_t> records_;
  };

  bool
  wait_until(const std::function<bool()> &predicate) {
    const auto give_up = std::chrono::steady_clock::now() + real_timeout;
    while (!predicate()) {
      if (std::chrono::steady_clock::now() >= give_up) {
        return false;
      }
      std::this_thread::sleep_for(1ms);
    }
    return true;
  }

  coordinator_options_t
  make_options(fake_clock_t &clock, bool managed = false) {
    coordinator_options_t options;
    options.clock = [&clock]() {
      return clock.now();
    };
    options.managed_mode = managed;
    options.poll_interval = 1ms;
    options.setup_timeout = 1000ms;
    options.native_idle_grace = 500ms;
    if (managed) {
      // Managed mode requires a restore body; tests that observe restores replace this one.
      options.native_restore = [](const mutation_capability_t &, const command_context_t &) {};
      options.probes.active_streams = []() { return 0u; };
    }
    return options;
  }

  command_body_t
  counting(std::atomic_int &calls) {
    return [&calls](const mutation_capability_t &, const command_context_t &) {
      ++calls;
    };
  }

  command_body_t
  logging(event_log_t &log, std::string event) {
    return [&log, event = std::move(event)](const mutation_capability_t &, const command_context_t &) {
      log.add(event);
    };
  }

  /** Logs its start, then spins until cancelled (or the real timeout) and logs which ended it. */
  command_body_t
  until_cancelled(event_log_t &log, std::promise<void> &started) {
    return [&log, &started](const mutation_capability_t &, const command_context_t &context) {
      log.add("prepare:start");
      started.set_value();
      const auto give_up = std::chrono::steady_clock::now() + real_timeout;
      while (!context.cancellation_requested() && std::chrono::steady_clock::now() < give_up) {
        std::this_thread::sleep_for(1ms);
      }
      log.add(context.cancellation_requested() ? "prepare:cancelled" : "prepare:not-cancelled");
    };
  }

  command_result_t
  start_ready(coordinator_t &coordinator) {
    return coordinator.start({}).get();
  }

  /** Runs an empty prepare for the launch, then binds it. */
  bool
  prepare_and_bind(coordinator_t &coordinator, const launch_token_t &token) {
    return coordinator.run_launch_prepare(token, {}).completed() && coordinator.bind_launch(token);
  }

  /** Runs an empty cleanup command; FIFO means everything posted earlier has finished. */
  command_result_t
  flush(coordinator_t &coordinator) {
    return coordinator.run(command_kind_e::stream_restore, {});
  }

  std::uint64_t
  submitted(const coordinator_t &coordinator) {
    return coordinator.status().submitted_commands;
  }

  // ---------------------------------------------------------------------------
  // Capability table
  // ---------------------------------------------------------------------------

  TEST(DisplayTopologyCapability, KindTableMatchesTheAgreedOperationTable) {
    using kind = command_kind_e;
    using op = operation_e;
    const auto with = [](std::set<op> operations, std::initializer_list<op> additions) {
      operations.insert(additions);
      return operations;
    };
    const std::set<op> restore { op::restore_state, op::write_topology, op::write_display_modes,
      op::write_primary, op::write_hdr, op::write_color_profile };
    const auto prepare = with(restore, { op::configure_display, op::vdd_create, op::vdd_destroy,
                                         op::vdd_set_modes, op::vdd_cursor, op::adapter_restart });
    const auto cleanup = with(restore, { op::vdd_create, op::vdd_destroy, op::native_restore });
    const std::map<kind, std::set<op>> legacy {
      { kind::startup_recovery, with(restore, { op::native_recover, op::vdd_create, op::vdd_destroy }) },
      { kind::legacy_startup_restore, restore },
      { kind::launch_prepare, with(prepare, { op::native_apply }) },
      { kind::launch_failed_restore, cleanup },
      { kind::reconfigure, prepare },
      { kind::stream_restore, cleanup },
      { kind::shutdown_restore, cleanup },
      { kind::shutdown_vdd_destroy, { op::vdd_destroy } },
      { kind::rotate, { op::rotate } },
      { kind::scale, { op::scale } },
      { kind::vdd_toggle, { op::vdd_create, op::vdd_destroy } },
      { kind::vdd_toggle_followup, { op::vdd_extended_mode, op::write_topology, op::vdd_destroy } },
      { kind::vdd_create, { op::vdd_create, op::vdd_extended_mode, op::write_topology } },
      { kind::vdd_destroy, { op::vdd_destroy } },
      { kind::reset_persistence, with(restore, { op::reset_persistence, op::vdd_destroy }) },
      { kind::legacy_retry_tick, with(restore, { op::configure_display, op::legacy_deferred, op::vdd_destroy }) },
      { kind::legacy_unlock_restore, with(restore, { op::legacy_deferred }) },
      { kind::native_restore, { op::native_restore, op::vdd_destroy } },
    };
    // Managed mode: whole-state restore survives only for startup and its registered continuation.
    const std::map<kind, std::set<op>> managed {
      { kind::startup_recovery, with(restore, { op::native_recover }) },
      { kind::legacy_startup_restore, restore },
      { kind::launch_prepare, { op::native_apply, op::vdd_create, op::vdd_destroy } },
      { kind::launch_failed_restore, { op::native_restore, op::vdd_destroy } },
      { kind::reconfigure, {} },
      { kind::stream_restore, { op::native_restore, op::vdd_destroy } },
      { kind::shutdown_restore, { op::native_restore, op::vdd_destroy } },
      { kind::shutdown_vdd_destroy, {} },
      { kind::rotate, { op::rotate } },
      { kind::scale, { op::scale } },
      { kind::vdd_toggle, {} },
      { kind::vdd_toggle_followup, {} },
      { kind::vdd_create, {} },
      { kind::vdd_destroy, {} },
      { kind::reset_persistence, { op::reset_persistence } },
      { kind::legacy_retry_tick, { op::legacy_deferred } },
      { kind::legacy_unlock_restore, { op::legacy_deferred } },
      { kind::native_restore, { op::native_restore, op::vdd_destroy } },
    };
    ASSERT_EQ(legacy.size(), all_kinds.size());
    ASSERT_EQ(managed.size(), all_kinds.size());

    for (const auto command : all_kinds) {
      for (const auto operation : all_operations) {
        EXPECT_EQ(kind_allows(command, operation, false), legacy.at(command).contains(operation))
          << to_string(command) << " / " << to_string(operation) << " (legacy)";
        EXPECT_EQ(kind_allows(command, operation, true), managed.at(command).contains(operation))
          << to_string(command) << " / " << to_string(operation) << " (managed)";
      }
    }
  }

  TEST(DisplayTopologyCapability, RestoreOnlyWorkersReachRestorationLeavesButNeverDriverMutators) {
    for (const bool managed : { false, true }) {
      for (const auto kind : { command_kind_e::startup_recovery, command_kind_e::legacy_startup_restore,
             command_kind_e::legacy_unlock_restore }) {
        if ((!managed && kind == command_kind_e::startup_recovery) ||
            (managed && kind == command_kind_e::legacy_unlock_restore)) {
          continue;
        }
        fake_clock_t clock;
        auto options = make_options(clock, managed);
        options.probes.startup_restore_quiescent = [] { return true; };
        coordinator_t coordinator { std::move(options) };
        std::atomic_int calls {};
        const command_body_t body = [&](const auto &capability, const auto &) {
          SCOPED_TRACE(std::string { to_string(kind) } + (managed ? "/managed" : "/legacy"));
          EXPECT_NO_THROW(capability.require(operation_e::restore_state));
          for (const auto operation : restoration_leaves) {
            SCOPED_TRACE(to_string(operation));
            EXPECT_NO_THROW(capability.require(operation));
            auto elsewhere = std::async(std::launch::async, [&] { capability.require(operation); });
            EXPECT_THROW(elsewhere.get(), capability_error_t);
          }
          for (const auto operation : { operation_e::configure_display, operation_e::vdd_create,
                 operation_e::vdd_destroy, operation_e::vdd_set_modes, operation_e::vdd_cursor,
                 operation_e::adapter_restart }) {
            SCOPED_TRACE(to_string(operation));
            EXPECT_THROW(capability.require(operation), capability_error_t);
          }
          ++calls;
        };
        if (kind == command_kind_e::startup_recovery) {
          EXPECT_TRUE(coordinator.start(body).get().completed());
        }
        else if (kind == command_kind_e::legacy_startup_restore) {
          ASSERT_TRUE(coordinator.start([&](const auto &capability, const auto &) {
                                   coordinator.defer_startup_restore(capability, body);
                                 })
              .get()
              .completed());
          EXPECT_TRUE(coordinator.retry_startup_restore().completion.get().completed());
        }
        else {
          ASSERT_TRUE(start_ready(coordinator).completed());
          EXPECT_TRUE(coordinator.run(kind, body).completed());
        }
        EXPECT_EQ(calls.load(), 1);
      }
    }
  }

  TEST(DisplayTopologyCapability, NativePreparationAndLegacyRetryHaveDifferentDriverAuthority) {
    for (const bool managed : { false, true }) {
      fake_clock_t clock;
      std::atomic_int prepared {};
      std::atomic_int retried {};
      coordinator_t coordinator { make_options(clock, managed) };
      ASSERT_TRUE(start_ready(coordinator).completed());
      const auto launch = coordinator.admit_launch({ 709, "driver-authority" });
      ASSERT_TRUE(launch);
      EXPECT_TRUE(coordinator.run_launch_prepare(*launch.token, [&](const auto &capability, const auto &) {
                               SCOPED_TRACE(managed);
                               EXPECT_NO_THROW(capability.require(operation_e::vdd_create));
                               EXPECT_NO_THROW(capability.require(operation_e::vdd_destroy));
                               for (const auto operation : { operation_e::vdd_set_modes, operation_e::vdd_cursor,
                                      operation_e::adapter_restart, operation_e::write_topology }) {
                                 SCOPED_TRACE(to_string(operation));
                                 if (managed) {
                                   EXPECT_THROW(capability.require(operation), capability_error_t);
                                 }
                                 else {
                                   EXPECT_NO_THROW(capability.require(operation));
                                 }
                               }
                               ++prepared;
                             })
          .completed());
      EXPECT_EQ(prepared.load(), 1);
      ASSERT_TRUE(coordinator.abort_launch(*launch.token, {}));
      EXPECT_TRUE(coordinator.run(command_kind_e::legacy_retry_tick, [&](const auto &capability, const auto &) {
                               SCOPED_TRACE(managed);
                               for (const auto operation : { operation_e::configure_display, operation_e::restore_state,
                                      operation_e::vdd_destroy, operation_e::write_topology, operation_e::write_hdr }) {
                                 SCOPED_TRACE(to_string(operation));
                                 if (managed) {
                                   EXPECT_THROW(capability.require(operation), capability_error_t);
                                 }
                                 else {
                                   EXPECT_NO_THROW(capability.require(operation));
                                 }
                               }
                               for (const auto operation : { operation_e::vdd_create, operation_e::vdd_set_modes,
                                      operation_e::vdd_cursor, operation_e::adapter_restart }) {
                                 SCOPED_TRACE(to_string(operation));
                                 EXPECT_THROW(capability.require(operation), capability_error_t);
                               }
                               ++retried;
                             })
          .completed());
      EXPECT_EQ(retried.load(), 1);
    }
  }

  TEST(DisplayTopologyCapability, CleanupKindsAreExactlyTheRestoreAndShutdownKinds) {
    for (const auto command : all_kinds) {
      EXPECT_EQ(is_cleanup_kind(command), expected_cleanup_kinds.contains(command)) << to_string(command);
    }
  }

  TEST(DisplayTopologyCapability, RotateCapabilityCannotDestroyAVirtualDisplay) {
    fake_clock_t clock;
    std::atomic_bool rotated {};
    std::atomic_bool destroyed {};
    coordinator_t coordinator { make_options(clock) };
    ASSERT_TRUE(start_ready(coordinator).completed());

    const auto result = coordinator.run(command_kind_e::rotate,
      [&](const mutation_capability_t &capability, const command_context_t &) {
        capability.require(operation_e::rotate);
        rotated = true;
        capability.require(operation_e::vdd_destroy);
        destroyed = true;
      });

    EXPECT_TRUE(rotated);
    EXPECT_FALSE(destroyed);
    EXPECT_EQ(result.outcome, outcome_e::failed);
    ASSERT_TRUE(result.exception);
    EXPECT_THROW(std::rethrow_exception(result.exception), capability_error_t);
  }

  TEST(DisplayTopologyCapability, CapabilityIsRefusedOffTheWorkerThread) {
    fake_clock_t clock;
    std::atomic_bool refused_elsewhere {};
    std::atomic_bool on_worker_inside {};
    coordinator_t coordinator { make_options(clock) };
    ASSERT_TRUE(start_ready(coordinator).completed());

    const auto result = coordinator.run(command_kind_e::rotate,
      [&](const mutation_capability_t &capability, const command_context_t &) {
        on_worker_inside = coordinator.on_worker_thread();
        auto elsewhere = std::async(std::launch::async, [&capability]() {
          capability.require(operation_e::rotate);
        });
        try {
          elsewhere.get();
        }
        catch (const capability_error_t &) {
          refused_elsewhere = true;
        }
        capability.require(operation_e::rotate);
      });

    EXPECT_TRUE(result.completed()) << to_string(result.outcome);
    EXPECT_TRUE(refused_elsewhere);
    EXPECT_TRUE(on_worker_inside);
    EXPECT_FALSE(coordinator.on_worker_thread());
  }

  TEST(DisplayTopologyCapability, ManagedModeRefusesLegacyWholeStateOutsideStartupRecovery) {
    fake_clock_t clock;
    std::atomic_bool recovery_restored {};
    std::atomic_bool native_restored {};
    std::atomic_bool legacy_restored {};
    coordinator_t coordinator { make_options(clock, true) };
    const auto recovery = coordinator.start([&](const mutation_capability_t &capability, const command_context_t &) {
      capability.require(operation_e::restore_state);
      recovery_restored = true;
    });
    ASSERT_TRUE(recovery.get().completed());

    const auto result = coordinator.run(command_kind_e::stream_restore,
      [&](const mutation_capability_t &capability, const command_context_t &) {
        capability.require(operation_e::native_restore);
        native_restored = true;
        capability.require(operation_e::restore_state);
        legacy_restored = true;
      });

    EXPECT_TRUE(recovery_restored);
    EXPECT_TRUE(native_restored);
    EXPECT_FALSE(legacy_restored);
    EXPECT_EQ(result.outcome, outcome_e::failed);
    ASSERT_TRUE(result.exception);
    EXPECT_THROW(std::rethrow_exception(result.exception), capability_error_t);
  }

  // ---------------------------------------------------------------------------
  // Start and recovery
  // ---------------------------------------------------------------------------

  TEST(DisplayTopologyCoordinator, RejectsEveryRouteBeforeStart) {
    fake_clock_t clock;
    std::atomic_int calls {};
    coordinator_t coordinator { make_options(clock) };

    const auto result = coordinator.run(command_kind_e::rotate, counting(calls));
    EXPECT_EQ(result.outcome, outcome_e::rejected);
    EXPECT_EQ(result.reason, reject_reason_e::not_started);
    EXPECT_EQ(coordinator.post(command_kind_e::stream_restore, counting(calls)), reject_reason_e::not_started);
    const auto admission = coordinator.admit_launch({ 1, "cert:a" });
    EXPECT_FALSE(admission);
    EXPECT_EQ(admission.reason, reject_reason_e::not_started);
    EXPECT_EQ(calls.load(), 0);
    EXPECT_EQ(coordinator.status().phase, coordinator_phase_e::not_started);
  }

  TEST(DisplayTopologyCoordinator, StartupRecoveryCompletesBeforeAnyRequestIsAdmitted) {
    fake_clock_t clock;
    event_log_t log;
    std::atomic_int calls {};
    coordinator_t coordinator { make_options(clock) };
    gate_t recovery_gate;
    std::future<command_result_t> waiting_run;
    const finally_t release { [&]() { recovery_gate.release(); } };

    const auto recovery = coordinator.start(
      [&log, hold = recovery_gate.hook()](const mutation_capability_t &, const command_context_t &) {
        log.add("recovery");
        hold();
      });
    ASSERT_TRUE(recovery_gate.wait_started());

    EXPECT_EQ(coordinator.status().phase, coordinator_phase_e::recovering);
    EXPECT_EQ(coordinator.post(command_kind_e::rotate, counting(calls)), reject_reason_e::recovering);
    EXPECT_EQ(coordinator.post(command_kind_e::stream_restore, counting(calls)), reject_reason_e::recovering);
    EXPECT_EQ(coordinator.admit_launch({ 1, "cert:a" }).reason, reject_reason_e::recovering);
    waiting_run = std::async(std::launch::async, [&]() {
      return coordinator.run(command_kind_e::scale, counting(calls));
    });
    ASSERT_EQ(waiting_run.wait_for(real_timeout), std::future_status::ready) << "run() queued behind recovery instead of rejecting";
    EXPECT_EQ(waiting_run.get().reason, reject_reason_e::recovering);

    recovery_gate.release();
    EXPECT_EQ(recovery.get().outcome, outcome_e::completed);
    EXPECT_EQ(coordinator.status().phase, coordinator_phase_e::running);
    EXPECT_TRUE(coordinator.run(command_kind_e::rotate, logging(log, "rotate")).completed());
    EXPECT_EQ(log.events(), (std::vector<std::string> { "recovery", "rotate" }));
    EXPECT_EQ(calls.load(), 0);
  }

  TEST(DisplayTopologyCoordinator, FailedRecoveryLeavesNativeInConflictAndLegacyRoutesWorking) {
    fake_clock_t clock;
    coordinator_t coordinator { make_options(clock, true) };
    const auto recovery = coordinator.start([](const mutation_capability_t &, const command_context_t &) {
      throw std::runtime_error { "journal unreadable" };
    });

    const auto result = recovery.get();
    EXPECT_EQ(result.outcome, outcome_e::failed);
    const auto status = coordinator.status();
    EXPECT_EQ(status.phase, coordinator_phase_e::running);
    EXPECT_TRUE(status.native.in_conflict);
    EXPECT_EQ(status.native.reason, "Startup recovery failed");
    EXPECT_EQ(coordinator.admit_launch({ 1, "cert:a" }).reason, reject_reason_e::recovery_required);
    EXPECT_TRUE(coordinator.run(command_kind_e::rotate, {}).completed());
  }

  TEST(DisplayTopologyCoordinator, FailedRecoveryKeepsTheConflictReasonItPublished) {
    fake_clock_t clock;
    coordinator_t coordinator { make_options(clock, true) };
    const auto recovery = coordinator.start([&coordinator](const mutation_capability_t &, const command_context_t &) {
      coordinator.publish_native_health({ true, true, "Journal version 7 is unknown" });
      throw std::runtime_error { "journal unreadable" };
    });

    EXPECT_EQ(recovery.get().outcome, outcome_e::failed);
    const auto status = coordinator.status();
    EXPECT_TRUE(status.native.in_conflict);
    EXPECT_EQ(status.native.reason, "Journal version 7 is unknown") << "the generic reason replaced the one recovery published";
  }

  TEST(DisplayTopologyCoordinator, ManagedModeRequiresANativeRestoreBody) {
    fake_clock_t clock;
    auto managed = make_options(clock, true);
    managed.native_restore = nullptr;
    EXPECT_THROW({ coordinator_t coordinator { std::move(managed) }; }, std::invalid_argument);

    auto legacy = make_options(clock);
    legacy.native_restore = nullptr;
    EXPECT_NO_THROW({ coordinator_t coordinator { std::move(legacy) }; });
  }

  TEST(DisplayTopologyCoordinator, StartupRestoreRegistrationIsWorkerScopedAndImmutable) {
    fake_clock_t clock;
    std::atomic_int calls {};
    auto options = make_options(clock);
    options.probes.startup_restore_quiescent = [] { return true; };
    coordinator_t coordinator { std::move(options) };
    coordinator_t foreign { make_options(clock) };
    ASSERT_TRUE(start_ready(foreign).completed());
    ASSERT_TRUE(coordinator.start([&](const auto &capability, const auto &) {
                             EXPECT_THROW(coordinator.defer_startup_restore(capability, {}), std::invalid_argument);
                             EXPECT_THROW(foreign.defer_startup_restore(capability, counting(calls)), capability_error_t);
                             std::thread other([&] {
                               EXPECT_THROW(coordinator.defer_startup_restore(capability, counting(calls)), capability_error_t);
                             });
                             other.join();
                             coordinator.defer_startup_restore(capability, [&](const auto &restore_capability, const auto &) {
                               EXPECT_TRUE(coordinator.on_worker_thread());
                               EXPECT_EQ(restore_capability.kind(), command_kind_e::legacy_startup_restore);
                               for (const auto operation : all_operations) {
                                 SCOPED_TRACE(to_string(operation));
                                 if (operation == operation_e::restore_state ||
                                     std::find(restoration_leaves.begin(), restoration_leaves.end(), operation) != restoration_leaves.end()) {
                                   EXPECT_NO_THROW(restore_capability.require(operation));
                                 }
                                 else {
                                   EXPECT_THROW(restore_capability.require(operation), capability_error_t);
                                 }
                               }
                               ++calls;
                             });
                             EXPECT_THROW(coordinator.defer_startup_restore(capability, counting(calls)), std::logic_error);
                           })
        .get()
        .completed());

    EXPECT_THROW((void) coordinator.run(command_kind_e::legacy_startup_restore, {}), std::invalid_argument);
    EXPECT_THROW((void) coordinator.post(command_kind_e::legacy_startup_restore, {}), std::invalid_argument);
    EXPECT_THROW((void) coordinator.post_tracked(command_kind_e::legacy_startup_restore, {}), std::invalid_argument);
    EXPECT_TRUE(coordinator.run(command_kind_e::shutdown_restore, [&](const auto &capability, const auto &) {
                             EXPECT_THROW(coordinator.defer_startup_restore(capability, counting(calls)), capability_error_t);
                             EXPECT_THROW(coordinator.forget_startup_restore(capability), capability_error_t);
                           })
        .completed());
    EXPECT_TRUE(coordinator.retry_startup_restore().completion.get().completed());
    EXPECT_EQ(calls.load(), 1);
    EXPECT_FALSE(coordinator.status().startup_restore_pending);
    EXPECT_EQ(coordinator.retry_startup_restore().reason, reject_reason_e::stale_transaction);
  }

  TEST(DisplayTopologyCoordinator, StartupRestoreRegistrationRequiresAnExplicitQuiescenceProbe) {
    for (const bool managed : { false, true }) {
      SCOPED_TRACE(managed);
      fake_clock_t clock;
      coordinator_t coordinator { make_options(clock, managed) };
      EXPECT_TRUE(coordinator.start([&](const auto &capability, const auto &) {
                               EXPECT_THROW(coordinator.defer_startup_restore(capability, [](const auto &, const auto &) {}), std::invalid_argument);
                             })
          .get()
          .completed());
      EXPECT_FALSE(coordinator.status().startup_restore_pending);
    }
  }

  TEST(DisplayTopologyCoordinator, StartupRestorePendingBlocksManagedButPreservesLegacyMerge) {
    for (const bool managed : { false, true }) {
      SCOPED_TRACE(managed);
      fake_clock_t clock;
      std::atomic_int attempts {};
      auto options = make_options(clock, managed);
      options.probes.startup_restore_quiescent = [] { return true; };
      coordinator_t coordinator { std::move(options) };
      ASSERT_TRUE(coordinator.start([&](const auto &capability, const auto &) {
                               coordinator.defer_startup_restore(capability, [&](const auto &restore_capability, const auto &) {
                                 restore_capability.require(operation_e::restore_state);
                                 if (++attempts == 1) throw std::runtime_error { "desktop still locked" };
                               });
                             })
          .get()
          .completed());
      EXPECT_TRUE(coordinator.status().startup_restore_pending);
      const auto launch = coordinator.admit_launch({ 710, "legacy-merge" });
      if (managed) {
        EXPECT_FALSE(launch);
        EXPECT_EQ(launch.reason, reject_reason_e::legacy_restore_pending);
      }
      else {
        ASSERT_TRUE(launch);
        EXPECT_EQ(coordinator.retry_startup_restore().reason, reject_reason_e::owner_busy);
        EXPECT_EQ(attempts.load(), 0);
        ASSERT_TRUE(coordinator.abort_launch(*launch.token, {}));
      }
      const auto failed = coordinator.retry_startup_restore().completion.get();
      EXPECT_EQ(failed.outcome, outcome_e::failed);
      EXPECT_TRUE(failed.exception);
      EXPECT_TRUE(coordinator.status().startup_restore_pending);
      EXPECT_FALSE(coordinator.status().fence_active);
      EXPECT_TRUE(coordinator.retry_startup_restore().completion.get().completed());
      EXPECT_EQ(attempts.load(), 2);
      EXPECT_FALSE(coordinator.status().startup_restore_pending);
      EXPECT_TRUE(coordinator.admit_launch({ 711, "after-restore" }));
    }
  }

  TEST(DisplayTopologyCoordinator, StartupRestoreCoalescesAndFencesTheProbeThroughItsObserver) {
    fake_clock_t clock;
    gate_t probe;
    gate_t observer;
    std::atomic_int calls {};
    auto options = make_options(clock);
    options.probes.startup_restore_quiescent = [hold = probe.hook()] { hold(); return true; };
    options.posted_observer = [hold = observer.hook()](const auto kind, const auto &) {
      if (kind == command_kind_e::legacy_startup_restore) {
        hold();
        throw std::runtime_error { "observer failed after restoration" };
      }
    };
    coordinator_t coordinator { std::move(options) };
    const finally_t release { [&] { probe.release(); observer.release(); } };
    ASSERT_TRUE(coordinator.start([&](const auto &capability, const auto &) {
                             coordinator.defer_startup_restore(capability, counting(calls));
                           })
        .get()
        .completed());
    const auto before = submitted(coordinator);
    const auto first = coordinator.retry_startup_restore();
    ASSERT_TRUE(probe.wait_started());
    EXPECT_EQ(coordinator.admit_launch({ 712, "during-probe" }).reason, reject_reason_e::fence_active);
    EXPECT_EQ(coordinator.post(command_kind_e::rotate, counting(calls)), reject_reason_e::fence_active);
    const auto duplicate = coordinator.retry_startup_restore();
    EXPECT_EQ(submitted(coordinator), before + 1);
    EXPECT_EQ(calls.load(), 0);
    EXPECT_EQ(duplicate.completion.wait_for(0ms), std::future_status::timeout);

    probe.release();
    ASSERT_TRUE(observer.wait_started());
    EXPECT_EQ(calls.load(), 1);
    EXPECT_FALSE(coordinator.status().startup_restore_pending);
    EXPECT_EQ(coordinator.admit_launch({ 713, "during-observer" }).reason, reject_reason_e::fence_active);
    const auto observing_duplicate = coordinator.retry_startup_restore();
    EXPECT_EQ(observing_duplicate.reason, reject_reason_e::none);
    EXPECT_EQ(submitted(coordinator), before + 1);
    EXPECT_EQ(observing_duplicate.completion.wait_for(0ms), std::future_status::timeout);
    EXPECT_EQ(first.completion.wait_for(0ms), std::future_status::timeout);
    auto independent = coordinator.hold_fence();
    observer.release();
    ASSERT_EQ(first.completion.wait_for(real_timeout), std::future_status::ready);
    EXPECT_EQ(first.completion.get().outcome, outcome_e::failed);
    EXPECT_EQ(duplicate.completion.get().outcome, outcome_e::failed);
    EXPECT_EQ(observing_duplicate.completion.get().outcome, outcome_e::failed);
    EXPECT_FALSE(coordinator.status().startup_restore_pending);
    EXPECT_EQ(coordinator.admit_launch({ 714, "independent-fence" }).reason, reject_reason_e::fence_active);
    independent.reset();
    EXPECT_FALSE(coordinator.status().fence_active);
    EXPECT_TRUE(coordinator.admit_launch({ 715, "after-observer" }));
  }

  TEST(DisplayTopologyCoordinator, StartupRestoreRechecksQueuedWorkBeforeEnteringItsBody) {
    for (const auto expected : { reject_reason_e::owner_busy, reject_reason_e::fence_active,
           reject_reason_e::draining, reject_reason_e::stale_transaction }) {
      SCOPED_TRACE(to_string(expected));
      fake_clock_t clock;
      gate_t busy;
      std::atomic_int calls {};
      auto options = make_options(clock);
      options.probes.startup_restore_quiescent = [] { return true; };
      coordinator_t coordinator { std::move(options) };
      const finally_t release { [&] { busy.release(); } };
      ASSERT_TRUE(coordinator.start([&](const auto &capability, const auto &) {
                               coordinator.defer_startup_restore(capability, counting(calls));
                             })
          .get()
          .completed());
      const bool reset = expected == reject_reason_e::stale_transaction;
      const auto blocker = coordinator.post_tracked(reset ? command_kind_e::reset_persistence : command_kind_e::rotate,
        [&, hold = busy.hook()](const auto &capability, const auto &) {
          hold();
          if (reset) coordinator.forget_startup_restore(capability);
        });
      ASSERT_TRUE(busy.wait_started());
      const auto retry = coordinator.retry_startup_restore();
      ASSERT_EQ(retry.reason, reject_reason_e::none);
      std::shared_ptr<const admission_fence_t> fence;
      if (expected == reject_reason_e::owner_busy) {
        ASSERT_TRUE(coordinator.admit_launch({ 716, "queued-race" }));
      }
      if (expected == reject_reason_e::fence_active) fence = coordinator.hold_fence();
      if (expected == reject_reason_e::draining) coordinator.begin_draining();
      busy.release();
      ASSERT_EQ(retry.completion.wait_for(real_timeout), std::future_status::ready);
      EXPECT_EQ(retry.completion.get().reason, expected);
      EXPECT_EQ(retry.completion.get().outcome, outcome_e::rejected);
      EXPECT_EQ(calls.load(), 0);
      EXPECT_EQ(coordinator.status().startup_restore_pending, !reset);
    }
  }

  TEST(DisplayTopologyCoordinator, StartupRestoreQuiescenceFailureAndExceptionRetainTheObligation) {
    fake_clock_t clock;
    std::atomic_int state {};
    std::atomic_int calls {};
    auto options = make_options(clock);
    options.probes.startup_restore_quiescent = [&] {
      if (state == 1) throw std::runtime_error { "cannot inspect startup work" };
      return state == 2;
    };
    coordinator_t coordinator { std::move(options) };
    ASSERT_TRUE(coordinator.start([&](const auto &capability, const auto &) {
                             coordinator.defer_startup_restore(capability, counting(calls));
                           })
        .get()
        .completed());
    EXPECT_EQ(coordinator.retry_startup_restore().completion.get().reason, reject_reason_e::owner_busy);
    EXPECT_TRUE(coordinator.status().startup_restore_pending);
    EXPECT_FALSE(coordinator.status().fence_active);
    state = 1;
    const auto failed = coordinator.retry_startup_restore().completion.get();
    EXPECT_EQ(failed.outcome, outcome_e::failed);
    EXPECT_TRUE(failed.exception);
    EXPECT_EQ(calls.load(), 0);
    EXPECT_TRUE(coordinator.status().startup_restore_pending);
    EXPECT_FALSE(coordinator.status().fence_active);
    state = 2;
    EXPECT_TRUE(coordinator.retry_startup_restore().completion.get().completed());
    EXPECT_EQ(calls.load(), 1);
  }

  TEST(DisplayTopologyCoordinator, StartupRestoreRechecksFencesAfterItsUnlockedQuiescenceProbe) {
    fake_clock_t clock;
    gate_t probe;
    std::atomic_int calls {};
    auto options = make_options(clock);
    options.probes.startup_restore_quiescent = [hold = probe.hook()] { hold(); return true; };
    coordinator_t coordinator { std::move(options) };
    std::future<std::shared_ptr<const admission_fence_t>> installing;
    const finally_t release { [&] { probe.release(); } };
    ASSERT_TRUE(coordinator.start([&](const auto &capability, const auto &) {
                             coordinator.defer_startup_restore(capability, counting(calls));
                           })
        .get()
        .completed());
    const auto retry = coordinator.retry_startup_restore();
    ASSERT_TRUE(probe.wait_started());
    installing = std::async(std::launch::async, [&] { return coordinator.hold_fence(); });
    ASSERT_EQ(installing.wait_for(real_timeout), std::future_status::ready) << "quiescence probe held the coordinator mutex";
    auto independent = installing.get();
    probe.release();
    ASSERT_EQ(retry.completion.wait_for(real_timeout), std::future_status::ready);
    EXPECT_EQ(retry.completion.get().reason, reject_reason_e::fence_active);
    EXPECT_EQ(calls.load(), 0);
    EXPECT_TRUE(coordinator.status().startup_restore_pending);
    independent.reset();
    EXPECT_FALSE(coordinator.status().fence_active);
    EXPECT_TRUE(coordinator.retry_startup_restore().completion.get().completed());
    EXPECT_EQ(calls.load(), 1);
  }

  TEST(DisplayTopologyCoordinator, ForgettingStartupRestoreDestroysCapturesOutsideTheCoordinatorLock) {
    fake_clock_t clock;
    auto options = make_options(clock);
    options.probes.startup_restore_quiescent = [] { return true; };
    coordinator_t coordinator { std::move(options) };
    std::atomic_bool destroyed {};
    std::weak_ptr<int> weak;
    ASSERT_TRUE(coordinator.start([&](const auto &capability, const auto &) {
                             auto capture = std::shared_ptr<int>(new int {}, [&](int *value) {
                               EXPECT_FALSE(coordinator.status().startup_restore_pending);
                               destroyed = true;
                               delete value;
                             });
                             weak = capture;
                             coordinator.defer_startup_restore(capability, [capture](const auto &, const auto &) {});
                           })
        .get()
        .completed());
    ASSERT_FALSE(weak.expired());
    const auto reset = coordinator.post_tracked(command_kind_e::reset_persistence, [&](const auto &capability, const auto &) {
      coordinator.forget_startup_restore(capability);
    });
    ASSERT_EQ(reset.completion.wait_for(real_timeout), std::future_status::ready) << "capture destructor deadlocked on status()";
    EXPECT_TRUE(reset.completion.get().completed());
    EXPECT_TRUE(destroyed.load());
    EXPECT_TRUE(weak.expired());
    EXPECT_FALSE(coordinator.status().startup_restore_pending);
  }

  TEST(DisplayTopologyCoordinator, StoppingSettlesAnUnstartedStartupRestoreWithoutEnteringItsBody) {
    fake_clock_t clock;
    gate_t busy;
    std::atomic_int calls {};
    auto options = make_options(clock);
    options.probes.startup_restore_quiescent = [] { return true; };
    coordinator_t coordinator { std::move(options) };
    std::future<void> stopping;
    const finally_t release { [&] { busy.release(); } };
    ASSERT_TRUE(coordinator.start([&](const auto &capability, const auto &) {
                             coordinator.defer_startup_restore(capability, counting(calls));
                           })
        .get()
        .completed());
    coordinator.post(command_kind_e::rotate, busy.body());
    ASSERT_TRUE(busy.wait_started());
    const auto retry = coordinator.retry_startup_restore();
    stopping = std::async(std::launch::async, [&] { coordinator.stop(); });
    ASSERT_TRUE(wait_until([&] { return coordinator.status().phase == coordinator_phase_e::stopped; }));
    busy.release();
    stopping.get();
    ASSERT_EQ(retry.completion.wait_for(0ms), std::future_status::ready);
    EXPECT_EQ(retry.completion.get().outcome, outcome_e::stopped);
    EXPECT_EQ(calls.load(), 0);
    EXPECT_FALSE(coordinator.status().startup_restore_pending);
    EXPECT_FALSE(coordinator.status().fence_active);
    EXPECT_EQ(coordinator.retry_startup_restore().reason, reject_reason_e::stopped);
  }

  TEST(DisplayTopologyCoordinator, StartupRestoreReleasesThePreviousFailureOutsideTheCoordinatorLock) {
    struct failure_t: std::runtime_error {
      explicit failure_t(std::shared_ptr<int> state):
          std::runtime_error { "deferred" }, state { std::move(state) } {}
      std::shared_ptr<int> state;
    };
    fake_clock_t clock;
    std::atomic_int attempts {};
    std::atomic_bool destroyed {};
    auto options = make_options(clock);
    options.probes.startup_restore_quiescent = [] { return true; };
    coordinator_t coordinator { std::move(options) };
    ASSERT_TRUE(coordinator.start([&](const auto &capability, const auto &) {
                             coordinator.defer_startup_restore(capability, [&](const auto &, const auto &) {
                               if (++attempts == 1) {
                                 throw failure_t { std::shared_ptr<int>(new int {}, [&](int *value) {
                                   (void) coordinator.status();
                                   destroyed = true;
                                   delete value;
                                 }) };
                               }
                             });
                           })
        .get()
        .completed());
    {
      const auto first = coordinator.retry_startup_restore();
      EXPECT_EQ(first.completion.get().outcome, outcome_e::failed);
    }
    ASSERT_TRUE(flush(coordinator).completed());
    EXPECT_FALSE(destroyed.load());
    auto retry = std::async(std::launch::async, [&] { return coordinator.retry_startup_restore(); });
    ASSERT_EQ(retry.wait_for(real_timeout), std::future_status::ready) << "previous exception destructor deadlocked on status()";
    EXPECT_TRUE(retry.get().completion.get().completed());
    EXPECT_TRUE(destroyed.load());
    EXPECT_EQ(attempts.load(), 2);
  }

  TEST(DisplayTopologyCoordinator, StartupFutureBecomesReadyOnItsOwn) {
    fake_clock_t clock;
    coordinator_t coordinator { make_options(clock) };
    gate_t recovery_gate;
    const finally_t release { [&]() { recovery_gate.release(); } };

    const auto recovery = coordinator.start(recovery_gate.body());
    ASSERT_TRUE(recovery_gate.wait_started());
    EXPECT_EQ(recovery.wait_for(0ms), std::future_status::timeout) << "the startup future is deferred, or ready while recovery still runs";

    recovery_gate.release();
    ASSERT_EQ(recovery.wait_for(real_timeout), std::future_status::ready) << "the startup future never became ready by itself";
    EXPECT_EQ(recovery.get().outcome, outcome_e::completed);
  }

  TEST(DisplayTopologyCoordinator, RecoveryStillRunsWhenDrainingBeginsBeforeTheWorkerReachesIt) {
    fake_clock_t clock;
    event_log_t log;
    gate_t preflight;
    std::atomic_bool preflight_gated {};
    const auto test_thread = std::this_thread::get_id();
    auto options = make_options(clock);
    // The worker reads the clock as it picks up a command; hold it on its first read.
    options.clock = [&clock, &preflight_gated, test_thread, hold = preflight.hook()]() {
      if (std::this_thread::get_id() != test_thread && !preflight_gated.exchange(true)) {
        hold();
      }
      return clock.now();
    };
    coordinator_t coordinator { std::move(options) };
    const finally_t release { [&]() { preflight.release(); } };

    const auto recovery = coordinator.start(logging(log, "recovery"));
    ASSERT_TRUE(preflight.wait_started());
    coordinator.begin_draining();
    ASSERT_EQ(coordinator.status().phase, coordinator_phase_e::draining);
    preflight.release();

    EXPECT_EQ(recovery.get().outcome, outcome_e::completed);
    EXPECT_EQ(coordinator.status().phase, coordinator_phase_e::draining) << "finishing recovery must not undo draining";
    EXPECT_TRUE(coordinator.run(command_kind_e::shutdown_restore, logging(log, "cleanup")).completed());
    EXPECT_EQ(log.events(), (std::vector<std::string> { "recovery", "cleanup" }));
  }

  TEST(DisplayTopologyCoordinator, StopAlwaysLeavesTheStartupFutureReady) {
    // Races stop() against the worker: recovery may not have been picked up yet, may be
    // running, or may have finished. Whichever it was, stop() leaves the future ready.
    for (int attempt = 0; attempt < 200; ++attempt) {
      fake_clock_t clock;
      std::atomic_int recoveries {};
      coordinator_t coordinator { make_options(clock) };
      const auto recovery = coordinator.start(counting(recoveries));
      coordinator.stop();

      ASSERT_EQ(recovery.wait_for(0ms), std::future_status::ready) << "attempt " << attempt << ": stop() returned before the startup future was ready";
      const auto outcome = recovery.get().outcome;
      ASSERT_TRUE(outcome == outcome_e::completed || outcome == outcome_e::completed_late || outcome == outcome_e::stopped)
        << to_string(outcome);
      ASSERT_EQ(recoveries.load(), outcome == outcome_e::stopped ? 0 : 1) << to_string(outcome);
    }
  }

  TEST(DisplayTopologyCoordinator, RecoveryInterruptedByStopReportsCompletedLate) {
    fake_clock_t clock;
    coordinator_t coordinator { make_options(clock) };
    std::promise<void> started;
    auto started_future = started.get_future();
    std::atomic_bool saw_cancellation {};

    const auto recovery = coordinator.start([&](const mutation_capability_t &, const command_context_t &context) {
      started.set_value();
      const auto give_up = std::chrono::steady_clock::now() + real_timeout;
      while (!context.cancellation_requested() && std::chrono::steady_clock::now() < give_up) {
        std::this_thread::sleep_for(1ms);
      }
      saw_cancellation = context.cancellation_requested();
    });
    ASSERT_EQ(started_future.wait_for(real_timeout), std::future_status::ready);
    coordinator.stop();

    EXPECT_TRUE(saw_cancellation.load()) << "stop() did not cancel the running recovery";
    ASSERT_EQ(recovery.wait_for(0ms), std::future_status::ready);
    EXPECT_EQ(recovery.get().outcome, outcome_e::completed_late) << "stop() cancelled recovery, but its result hides that";
  }

  TEST(DisplayTopologyCoordinator, DedicatedKindsHaveDedicatedEntryPoints) {
    fake_clock_t clock;
    coordinator_t coordinator { make_options(clock) };
    ASSERT_TRUE(start_ready(coordinator).completed());

    EXPECT_THROW(static_cast<void>(coordinator.run(command_kind_e::startup_recovery, {})), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(coordinator.run(command_kind_e::launch_prepare, {})), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(coordinator.post(command_kind_e::startup_recovery, {})), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(coordinator.post(command_kind_e::launch_prepare, {})), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(coordinator.start({})), std::logic_error);
  }

  // ---------------------------------------------------------------------------
  // Launch ownership
  // ---------------------------------------------------------------------------

  TEST(DisplayTopologyCoordinator, CompetingLaunchesAreRefusedWithZeroPrepareCalls) {
    fake_clock_t clock;
    std::atomic_int prepares {};
    std::atomic_int native_restores {};
    auto options = make_options(clock);
    options.native_restore = counting(native_restores);
    coordinator_t coordinator { std::move(options) };
    ASSERT_TRUE(start_ready(coordinator).completed());

    const auto first = coordinator.admit_launch({ 11, "cert:a" });
    ASSERT_TRUE(first);
    // While the first launch prepares, every other launch is refused, same client included.
    for (const std::string key : { "cert:b", "cert:a", "" }) {
      const auto other = coordinator.admit_launch({ 12, key });
      EXPECT_FALSE(other) << "client '" << key << "'";
      EXPECT_EQ(other.reason, reject_reason_e::owner_busy) << "client '" << key << "'";
      if (other) {
        static_cast<void>(coordinator.run_launch_prepare(*other.token, counting(prepares)));
      }
    }
    EXPECT_EQ(prepares.load(), 0);

    EXPECT_TRUE(coordinator.run_launch_prepare(*first.token, counting(prepares)).completed());
    EXPECT_EQ(prepares.load(), 1);
    ASSERT_TRUE(coordinator.bind_launch(*first.token));

    // Once bound, only the same client may supersede its own pending ticket.
    EXPECT_EQ(coordinator.admit_launch({ 13, "cert:b" }).reason, reject_reason_e::owner_busy);
    EXPECT_EQ(coordinator.admit_launch({ 13, "" }).reason, reject_reason_e::owner_busy);
    const auto superseding = coordinator.admit_launch({ 14, "cert:a" });
    ASSERT_TRUE(superseding);
    auto status = coordinator.status();
    ASSERT_TRUE(status.lease);
    EXPECT_EQ(status.lease->transaction, superseding.token->transaction);
    EXPECT_EQ(status.lease->launch_session_id, 14u);
    EXPECT_EQ(status.lease->stage, lease_stage_e::preparing);
    EXPECT_EQ(status.native_restores_posted, 0u);
    EXPECT_FALSE(coordinator.bind_launch(*first.token));

    // An empty client key never matches, not even another empty key.
    ASSERT_TRUE(prepare_and_bind(coordinator, *superseding.token));
    ASSERT_TRUE(coordinator.launch_activated(*superseding.token));
    const auto anonymous = coordinator.admit_launch({ 15, "" });
    ASSERT_TRUE(anonymous);
    ASSERT_TRUE(prepare_and_bind(coordinator, *anonymous.token));
    EXPECT_EQ(coordinator.admit_launch({ 16, "" }).reason, reject_reason_e::owner_busy);
    EXPECT_EQ(prepares.load(), 1);
    ASSERT_TRUE(flush(coordinator).completed());
    EXPECT_EQ(native_restores.load(), 0) << "a legacy launch posted a native restore";
  }

  TEST(DisplayTopologyCoordinator, LegacyLeaseEndsWhenItsBoundTicketDisappears) {
    fake_clock_t clock;
    std::atomic_bool ticket_present { true };
    std::atomic_int native_restores {};
    std::mutex probed_mutex;
    std::vector<std::uint32_t> probed;
    auto options = make_options(clock);
    options.native_restore = counting(native_restores);
    options.probes.ticket_present = [&](std::uint32_t id) {
      std::lock_guard lock { probed_mutex };
      probed.push_back(id);
      return ticket_present.load();
    };
    coordinator_t coordinator { std::move(options) };
    ASSERT_TRUE(start_ready(coordinator).completed());

    const auto first = coordinator.admit_launch({ 21, "cert:a" });
    ASSERT_TRUE(first);
    // A preparing launch has no ticket yet, so a missing ticket must not end it.
    ticket_present = false;
    EXPECT_EQ(coordinator.admit_launch({ 22, "cert:b" }).reason, reject_reason_e::owner_busy);
    ticket_present = true;
    ASSERT_TRUE(coordinator.run_launch_prepare(*first.token, {}).completed());
    ASSERT_TRUE(coordinator.bind_launch(*first.token));
    EXPECT_EQ(coordinator.admit_launch({ 22, "cert:b" }).reason, reject_reason_e::owner_busy);

    ticket_present = false;
    const auto second = coordinator.admit_launch({ 22, "cert:b" });
    EXPECT_TRUE(second) << to_string(second.reason);
    EXPECT_EQ(coordinator.status().native_restores_posted, 0u);
    ASSERT_TRUE(flush(coordinator).completed());
    EXPECT_EQ(native_restores.load(), 0) << "a legacy launch posted a native restore";
    std::lock_guard lock { probed_mutex };
    EXPECT_EQ(probed, (std::vector<std::uint32_t> { 21, 21 }));
  }

  TEST(DisplayTopologyCoordinator, ActivationEndsALegacyLeaseAndActivatesAManagedOne) {
    fake_clock_t clock;
    {
      coordinator_t legacy { make_options(clock) };
      ASSERT_TRUE(start_ready(legacy).completed());

      const auto before_bind = legacy.admit_launch({ 31, "cert:a" });
      ASSERT_TRUE(before_bind);
      ASSERT_TRUE(legacy.run_launch_prepare(*before_bind.token, {}).completed());
      EXPECT_FALSE(legacy.launch_activated(*before_bind.token));
      ASSERT_TRUE(legacy.status().lease);
      EXPECT_EQ(legacy.status().lease->stage, lease_stage_e::preparing);
      ASSERT_TRUE(legacy.bind_launch(*before_bind.token));
      EXPECT_TRUE(legacy.status().lease) << "pre-bind activation is refused";
      EXPECT_TRUE(legacy.launch_activated(*before_bind.token));
      EXPECT_FALSE(legacy.status().lease);

      const auto after_bind = legacy.admit_launch({ 32, "cert:a" });
      ASSERT_TRUE(after_bind);
      ASSERT_TRUE(prepare_and_bind(legacy, *after_bind.token));
      auto unknown = *after_bind.token;
      unknown.launch_session_id = 999;
      EXPECT_FALSE(legacy.launch_activated(unknown));
      ASSERT_TRUE(legacy.status().lease);
      EXPECT_EQ(legacy.status().lease->stage, lease_stage_e::bound);
      EXPECT_TRUE(legacy.launch_activated(*after_bind.token));
      EXPECT_FALSE(legacy.status().lease);
    }
    {
      coordinator_t managed { make_options(clock, true) };
      ASSERT_TRUE(start_ready(managed).completed());

      const auto before_bind = managed.admit_launch({ 33, "cert:a" });
      ASSERT_TRUE(before_bind);
      ASSERT_TRUE(managed.run_launch_prepare(*before_bind.token, {}).completed());
      EXPECT_FALSE(managed.launch_activated(*before_bind.token));
      ASSERT_TRUE(managed.bind_launch(*before_bind.token));
      EXPECT_TRUE(managed.launch_activated(*before_bind.token));
      ASSERT_TRUE(managed.status().lease);
      EXPECT_EQ(managed.status().lease->stage, lease_stage_e::active);

      ASSERT_TRUE(managed.end_managed_launch(*before_bind.token));
      const auto after_bind = managed.admit_launch({ 34, "cert:a" });
      ASSERT_TRUE(after_bind);
      ASSERT_TRUE(prepare_and_bind(managed, *after_bind.token));
      ASSERT_TRUE(managed.status().lease);
      EXPECT_EQ(managed.status().lease->stage, lease_stage_e::bound);
      EXPECT_TRUE(managed.launch_activated(*after_bind.token));
      ASSERT_TRUE(managed.status().lease);
      EXPECT_EQ(managed.status().lease->stage, lease_stage_e::active);
    }
  }

  TEST(DisplayTopologyCoordinator, ASecondPrepareForTheSameLaunchIsStale) {
    fake_clock_t clock;
    std::atomic_int prepares {};
    coordinator_t coordinator { make_options(clock) };
    ASSERT_TRUE(start_ready(coordinator).completed());

    const auto launch = coordinator.admit_launch({ 41, "cert:a" });
    ASSERT_TRUE(launch);
    EXPECT_TRUE(coordinator.run_launch_prepare(*launch.token, counting(prepares)).completed());
    const auto again = coordinator.run_launch_prepare(*launch.token, counting(prepares));
    EXPECT_EQ(again.outcome, outcome_e::rejected);
    EXPECT_EQ(again.reason, reject_reason_e::stale_transaction);
    EXPECT_EQ(prepares.load(), 1);
  }

  TEST(DisplayTopologyCoordinator, BindRejectsExpiredPreparingLeaseEvenWhenTokenDeadlineIsForged) {
    fake_clock_t clock;
    std::atomic_int restored {};
    auto options = make_options(clock, true);
    options.native_restore = counting(restored);
    coordinator_t coordinator { options };
    ASSERT_TRUE(start_ready(coordinator).completed());
    const auto launch = coordinator.admit_launch({ 301, "client" });
    ASSERT_TRUE(launch);
    ASSERT_TRUE(coordinator.run_launch_prepare(*launch.token, {}).completed());
    auto forged = *launch.token;
    forged.setup_deadline = monotonic_time_t::max();
    clock.advance(1000ms);
    EXPECT_FALSE(coordinator.bind_launch(forged));
    EXPECT_FALSE(coordinator.status().lease);
    ASSERT_TRUE(flush(coordinator).completed());
    EXPECT_EQ(restored, 1);
  }

  TEST(DisplayTopologyCoordinator, TickReapsUnboundPreparedManagedLeaseExactlyAtItsDeadline) {
    fake_clock_t clock;
    std::atomic_int restored {};
    auto options = make_options(clock, true);
    options.native_restore = counting(restored);
    coordinator_t coordinator { options };
    ASSERT_TRUE(start_ready(coordinator).completed());
    const auto launch = coordinator.admit_launch({ 302, "client" });
    ASSERT_TRUE(launch);
    ASSERT_TRUE(coordinator.run_launch_prepare(*launch.token, {}).completed());
    clock.advance(999ms);
    coordinator.tick();
    EXPECT_TRUE(coordinator.status().lease);
    EXPECT_EQ(restored, 0);
    clock.advance(1ms);
    coordinator.tick();
    coordinator.tick();
    EXPECT_FALSE(coordinator.status().lease);
    EXPECT_FALSE(coordinator.bind_launch(*launch.token));
    EXPECT_FALSE(coordinator.abort_launch(*launch.token, {}));
    ASSERT_TRUE(flush(coordinator).completed());
    EXPECT_EQ(restored, 1);
  }

  TEST(DisplayTopologyCoordinator, BindCannotPublishAfterDrainingOrStop) {
    for (const bool stop : { false, true }) {
      for (const bool early_activation : { false, true }) {
        SCOPED_TRACE(stop ? "stopped" : "draining");
        SCOPED_TRACE(early_activation ? "early activation" : "not activated");
        fake_clock_t clock;
        coordinator_t coordinator { make_options(clock) };
        ASSERT_TRUE(start_ready(coordinator).completed());
        const auto launch = coordinator.admit_launch({ 303, "client" });
        ASSERT_TRUE(launch);
        ASSERT_TRUE(coordinator.run_launch_prepare(*launch.token, {}).completed());
        if (early_activation) {
          EXPECT_FALSE(coordinator.launch_activated(*launch.token));
        }
        if (stop)
          coordinator.stop();
        else
          coordinator.begin_draining();
        EXPECT_FALSE(coordinator.bind_launch(*launch.token));
      }
    }
  }

  TEST(DisplayTopologyCoordinator, AdmissionReapsExpiredPreparationBeforeItsReplacementCanMutate) {
    fake_clock_t clock;
    event_log_t log;
    auto options = make_options(clock, true);
    options.native_restore = logging(log, "restore");
    coordinator_t coordinator { options };
    ASSERT_TRUE(start_ready(coordinator).completed());
    const auto first = coordinator.admit_launch({ 304, "first" });
    ASSERT_TRUE(first);
    ASSERT_TRUE(coordinator.run_launch_prepare(*first.token, logging(log, "first")).completed());
    clock.advance(1000ms);
    const auto second = coordinator.admit_launch({ 305, "second" });
    ASSERT_TRUE(second);
    EXPECT_FALSE(coordinator.abort_launch(*first.token, {}));
    ASSERT_TRUE(coordinator.run_launch_prepare(*second.token, logging(log, "second")).completed());
    EXPECT_EQ(log.events(), (std::vector<std::string> { "first", "restore", "second" }));
  }

  TEST(DisplayTopologyCoordinator, SetupExpiryCancelsOnlyItsRunningPrepareAndOrdersLegacyCleanup) {
    fake_clock_t clock;
    event_log_t log;
    auto options = make_options(clock);
    options.poll_interval = 5s;
    coordinator_t coordinator { options };
    ASSERT_TRUE(start_ready(coordinator).completed());
    gate_t worker;
    const auto first = coordinator.admit_launch({ 306, "first" });
    ASSERT_TRUE(first);
    std::atomic_bool cancelled {};
    auto prepare = std::async(std::launch::async, [&]() {
      return coordinator.run_launch_prepare(*first.token, [&](const mutation_capability_t &, const command_context_t &context) {
          worker.hook()();
          cancelled = context.cancellation_requested();
          log.add("prepare"); }, logging(log, "cleanup"));
    });
    const finally_t release { [&]() { worker.release(); } };
    ASSERT_TRUE(worker.wait_started());
    const auto unrelated = coordinator.post_tracked(command_kind_e::rotate, logging(log, "unrelated"));
    clock.advance(1000ms);
    coordinator.tick();
    EXPECT_FALSE(coordinator.status().lease);
    EXPECT_FALSE(coordinator.abort_launch(*first.token, logging(log, "duplicate")));
    const auto second = coordinator.admit_launch({ 307, "second" });
    ASSERT_TRUE(second);
    worker.release();
    EXPECT_FALSE(prepare.get().completed());
    EXPECT_TRUE(cancelled);
    EXPECT_TRUE(unrelated.completion.get().completed());
    ASSERT_TRUE(coordinator.run_launch_prepare(*second.token, logging(log, "second")).completed());
    EXPECT_EQ(log.events(), (std::vector<std::string> { "prepare", "unrelated", "cleanup", "second" }));
  }

  TEST(DisplayTopologyCoordinator, SetupExpiryBeforeTheBodyStartsMakesNoBackendOrCleanupCalls) {
    for (const bool managed : { false, true }) {
      fake_clock_t clock;
      std::atomic_int calls {};
      auto options = make_options(clock, managed);
      options.poll_interval = 5s;
      if (managed) options.native_restore = counting(calls);
      coordinator_t coordinator { options };
      ASSERT_TRUE(start_ready(coordinator).completed());
      const auto unsubmitted = coordinator.admit_launch({ 308, "unsubmitted" });
      ASSERT_TRUE(unsubmitted);
      clock.advance(1000ms);
      coordinator.tick();
      EXPECT_FALSE(coordinator.status().lease);
      gate_t worker;
      const auto blocker = coordinator.post_tracked(command_kind_e::rotate, worker.body());
      ASSERT_TRUE(worker.wait_started());
      const auto launch = coordinator.admit_launch({ 309, "queued" });
      ASSERT_TRUE(launch);
      const auto before = submitted(coordinator);
      auto prepare = std::async(std::launch::async, [&]() {
        return coordinator.run_launch_prepare(*launch.token, counting(calls), counting(calls));
      });
      const finally_t release { [&]() { worker.release(); } };
      ASSERT_TRUE(wait_until([&]() { return submitted(coordinator) == before + 1; }));
      clock.advance(1000ms);
      coordinator.tick();
      EXPECT_FALSE(coordinator.status().lease);
      worker.release();
      EXPECT_EQ(prepare.get().outcome, outcome_e::expired_before_start);
      ASSERT_TRUE(flush(coordinator).completed());
      EXPECT_EQ(calls, 0);
    }
  }

  TEST(DisplayTopologyCoordinator, SuccessfulBindEndsSetupExpiryAndKeepsTheTicketWindow) {
    fake_clock_t clock;
    auto options = make_options(clock);
    options.probes.ticket_present = [](std::uint32_t) { return true; };
    coordinator_t coordinator { options };
    ASSERT_TRUE(start_ready(coordinator).completed());
    const auto launch = coordinator.admit_launch({ 310, "client" });
    ASSERT_TRUE(launch);
    ASSERT_TRUE(coordinator.run_launch_prepare(*launch.token, {}).completed());
    clock.advance(999ms);
    ASSERT_TRUE(coordinator.bind_launch(*launch.token));
    clock.advance(5000ms);
    coordinator.tick();
    EXPECT_EQ(coordinator.admit_launch({ 311, "another" }).reason, reject_reason_e::owner_busy);
    ASSERT_TRUE(coordinator.status().lease);
    EXPECT_EQ(coordinator.status().lease->stage, lease_stage_e::bound);
  }

  TEST(DisplayTopologyCoordinator, AbortAfterStopReleasesOnlyTheLeaseWithoutSubmittingCleanup) {
    fake_clock_t clock;
    std::atomic_int cleanup {};
    coordinator_t coordinator { make_options(clock) };
    ASSERT_TRUE(start_ready(coordinator).completed());
    const auto launch = coordinator.admit_launch({ 312, "client" });
    ASSERT_TRUE(launch);
    ASSERT_TRUE(coordinator.run_launch_prepare(*launch.token, {}).completed());
    coordinator.stop();
    EXPECT_TRUE(coordinator.abort_launch(*launch.token, counting(cleanup)));
    EXPECT_FALSE(coordinator.status().lease);
    EXPECT_EQ(cleanup, 0);
  }

  TEST(DisplayTopologyCoordinator, BindNeedsACompletedPrepare) {
    fake_clock_t clock;
    coordinator_t coordinator { make_options(clock) };
    ASSERT_TRUE(start_ready(coordinator).completed());
    gate_t prepare_gate;
    std::future<command_result_t> prepare;
    const finally_t release { [&]() { prepare_gate.release(); } };

    const auto launch = coordinator.admit_launch({ 241, "cert:a" });
    ASSERT_TRUE(launch);
    EXPECT_FALSE(coordinator.bind_launch(*launch.token)) << "bound before any prepare";
    prepare = std::async(std::launch::async, [&]() {
      return coordinator.run_launch_prepare(*launch.token, prepare_gate.body());
    });
    ASSERT_TRUE(prepare_gate.wait_started());
    EXPECT_FALSE(coordinator.bind_launch(*launch.token)) << "bound while the prepare was still running";
    prepare_gate.release();
    EXPECT_TRUE(prepare.get().completed());
    EXPECT_TRUE(coordinator.bind_launch(*launch.token));
    EXPECT_FALSE(coordinator.bind_launch(*launch.token)) << "bound twice";
    EXPECT_TRUE(coordinator.launch_activated(*launch.token));
    ASSERT_FALSE(coordinator.status().lease);

    const auto failing = coordinator.admit_launch({ 242, "cert:a" });
    ASSERT_TRUE(failing);
    const auto failed = coordinator.run_launch_prepare(*failing.token,
      [](const mutation_capability_t &, const command_context_t &) {
        throw std::runtime_error { "apply failed" };
      });
    EXPECT_EQ(failed.outcome, outcome_e::failed);
    EXPECT_TRUE(failed.exception) << "the prepare's exception was dropped";
    EXPECT_FALSE(coordinator.bind_launch(*failing.token)) << "bound after a failed prepare";
    EXPECT_TRUE(coordinator.abort_launch(*failing.token, {}));
  }

  TEST(DisplayTopologyCoordinator, PrepareWithAStaleTokenNeverRuns) {
    fake_clock_t clock;
    std::atomic_int prepares {};
    coordinator_t coordinator { make_options(clock) };
    ASSERT_TRUE(start_ready(coordinator).completed());

    const auto first = coordinator.admit_launch({ 301, "cert:a" });
    ASSERT_TRUE(first);
    ASSERT_TRUE(prepare_and_bind(coordinator, *first.token));
    EXPECT_EQ(coordinator.run_launch_prepare(*first.token, counting(prepares)).reason, reject_reason_e::stale_transaction);

    const auto second = coordinator.admit_launch({ 302, "cert:a" });
    ASSERT_TRUE(second) << "the same client supersedes its own bound launch";
    EXPECT_EQ(coordinator.run_launch_prepare(*first.token, counting(prepares)).reason, reject_reason_e::stale_transaction);
    EXPECT_EQ(prepares.load(), 0);
    EXPECT_FALSE(coordinator.abort_launch(*first.token, {})) << "a stale token aborted the launch that replaced it";
    EXPECT_TRUE(coordinator.run_launch_prepare(*second.token, counting(prepares)).completed());
    EXPECT_EQ(prepares.load(), 1);
    EXPECT_FALSE(coordinator.bind_launch(*first.token)) << "a stale token bound the launch that replaced it";
    EXPECT_TRUE(coordinator.bind_launch(*second.token));
  }

  TEST(DisplayTopologyCoordinator, AManagedLeaseIsNeverSupersededEvenByTheSameClient) {
    fake_clock_t clock;
    std::atomic_int restores {};
    auto options = make_options(clock, true);
    options.native_restore = counting(restores);
    coordinator_t coordinator { std::move(options) };
    ASSERT_TRUE(start_ready(coordinator).completed());

    const auto launch = coordinator.admit_launch({ 291, "cert:a" });
    ASSERT_TRUE(launch);
    ASSERT_TRUE(prepare_and_bind(coordinator, *launch.token));
    EXPECT_EQ(coordinator.admit_launch({ 292, "cert:a" }).reason, reject_reason_e::owner_busy);
    EXPECT_TRUE(coordinator.launch_activated(*launch.token));
    EXPECT_EQ(coordinator.admit_launch({ 293, "cert:a" }).reason, reject_reason_e::owner_busy);
    ASSERT_TRUE(flush(coordinator).completed());

    const auto status = coordinator.status();
    ASSERT_TRUE(status.lease);
    EXPECT_EQ(status.lease->launch_session_id, 291u);
    EXPECT_EQ(status.native_restores_posted, 0u);
    EXPECT_EQ(restores.load(), 0);
  }

  TEST(DisplayTopologyCoordinator, ManagedLaunchNeedsHealthyNativeStateAndNoDeferredLegacyRestore) {
    fake_clock_t clock;
    std::atomic_bool legacy_pending {};
    std::atomic_int prepares {};
    auto options = make_options(clock, true);
    options.probes.legacy_restore_pending = [&]() {
      return legacy_pending.load();
    };
    coordinator_t coordinator { std::move(options) };
    ASSERT_TRUE(start_ready(coordinator).completed());

    coordinator.publish_native_health({ true, true, "Journal version 7 is unknown" });
    EXPECT_EQ(coordinator.admit_launch({ 51, "cert:a" }).reason, reject_reason_e::recovery_required);
    EXPECT_EQ(coordinator.status().native.reason, "Journal version 7 is unknown");
    coordinator.publish_native_health({});

    legacy_pending = true;
    EXPECT_EQ(coordinator.admit_launch({ 52, "cert:a" }).reason, reject_reason_e::legacy_restore_pending);
    legacy_pending = false;
    const auto launch = coordinator.admit_launch({ 53, "cert:a" });
    ASSERT_TRUE(launch);

    // A deferred legacy restore that appears after admission is caught on the worker.
    legacy_pending = true;
    const auto result = coordinator.run_launch_prepare(*launch.token, counting(prepares));
    EXPECT_EQ(result.outcome, outcome_e::rejected);
    EXPECT_EQ(result.reason, reject_reason_e::legacy_restore_pending);
    EXPECT_EQ(prepares.load(), 0);
  }

  TEST(DisplayTopologyCoordinator, LegacyLaunchMayMergeWithADeferredLegacyRestore) {
    fake_clock_t clock;
    std::atomic_int prepares {};
    auto options = make_options(clock);
    options.probes.legacy_restore_pending = []() {
      return true;
    };
    coordinator_t coordinator { std::move(options) };
    ASSERT_TRUE(start_ready(coordinator).completed());

    const auto launch = coordinator.admit_launch({ 54, "cert:a" });
    ASSERT_TRUE(launch) << to_string(launch.reason);
    EXPECT_TRUE(coordinator.run_launch_prepare(*launch.token, counting(prepares)).completed());
    EXPECT_EQ(prepares.load(), 1);
  }

  TEST(DisplayTopologyCoordinator, ManagedLeaseRefusesNonCleanupCommandsAndExemptsCleanup) {
    fake_clock_t clock;
    std::map<command_kind_e, std::atomic_int> calls;
    for (const auto command : all_kinds) {
      calls[command];
    }
    coordinator_t coordinator { make_options(clock, true) };
    ASSERT_TRUE(start_ready(coordinator).completed());
    ASSERT_TRUE(coordinator.admit_launch({ 61, "cert:a" }));

    for (const auto command : all_kinds) {
      if (command == command_kind_e::startup_recovery || command == command_kind_e::legacy_startup_restore ||
          command == command_kind_e::launch_prepare) {
        continue;
      }
      const auto expected = expected_cleanup_kinds.contains(command) ? reject_reason_e::none : reject_reason_e::managed_session;
      EXPECT_EQ(coordinator.post(command, counting(calls[command])), expected) << to_string(command);
    }
    ASSERT_TRUE(flush(coordinator).completed());
    for (const auto command : all_kinds) {
      if (command == command_kind_e::startup_recovery || command == command_kind_e::legacy_startup_restore ||
          command == command_kind_e::launch_prepare) {
        continue;
      }
      EXPECT_EQ(calls[command].load(), expected_cleanup_kinds.contains(command) ? 1 : 0) << to_string(command);
    }
  }

  TEST(DisplayTopologyCoordinator, LegacyLeaseLeavesOtherRoutesSerializedButAllowed) {
    fake_clock_t clock;
    std::atomic_int calls {};
    coordinator_t coordinator { make_options(clock) };
    ASSERT_TRUE(start_ready(coordinator).completed());
    ASSERT_TRUE(coordinator.admit_launch({ 62, "cert:a" }));

    for (const auto command : all_kinds) {
      if (command == command_kind_e::startup_recovery || command == command_kind_e::legacy_startup_restore ||
          command == command_kind_e::launch_prepare) {
        continue;
      }
      EXPECT_EQ(coordinator.post(command, counting(calls)), reject_reason_e::none) << to_string(command);
    }
    ASSERT_TRUE(flush(coordinator).completed());
    EXPECT_EQ(calls.load(), static_cast<int>(all_kinds.size()) - 3);
  }

  TEST(DisplayTopologyCoordinator, WorkerReevaluatesPolicyBeforeRunningAQueuedBody) {
    fake_clock_t clock;
    posted_log_t posted;
    std::atomic_int scales {};
    auto options = make_options(clock, true);
    options.posted_observer = posted.observer();
    coordinator_t coordinator { std::move(options) };
    ASSERT_TRUE(start_ready(coordinator).completed());
    gate_t busy;
    std::future<command_result_t> blocker;
    const finally_t release { [&]() { busy.release(); } };

    blocker = std::async(std::launch::async, [&]() {
      return coordinator.run(command_kind_e::rotate, busy.body());
    });
    ASSERT_TRUE(busy.wait_started());
    ASSERT_EQ(coordinator.post(command_kind_e::scale, counting(scales)), reject_reason_e::none);
    ASSERT_TRUE(coordinator.admit_launch({ 63, "cert:a" }));
    busy.release();
    EXPECT_TRUE(blocker.get().completed());
    ASSERT_TRUE(flush(coordinator).completed());

    EXPECT_EQ(scales.load(), 0);
    const auto records = posted.records();
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].kind, command_kind_e::scale);
    EXPECT_EQ(records[0].outcome, outcome_e::rejected);
    EXPECT_EQ(records[0].reason, reject_reason_e::managed_session);
  }

  // ---------------------------------------------------------------------------
  // Deadlines, cancellation and cleanup
  // ---------------------------------------------------------------------------

  TEST(DisplayTopologyCoordinator, PrepareThatExpiresBeforeStartingNeverRuns) {
    fake_clock_t clock;
    std::atomic_int prepares {};
    coordinator_t coordinator { make_options(clock) };
    ASSERT_TRUE(start_ready(coordinator).completed());
    gate_t busy;
    std::future<command_result_t> blocker;
    std::future<command_result_t> prepare;
    const finally_t release { [&]() { busy.release(); } };

    blocker = std::async(std::launch::async, [&]() {
      return coordinator.run(command_kind_e::rotate, busy.body());
    });
    ASSERT_TRUE(busy.wait_started());
    const auto launch = coordinator.admit_launch({ 71, "cert:a" });
    ASSERT_TRUE(launch);
    const auto before = submitted(coordinator);
    prepare = std::async(std::launch::async, [&]() {
      return coordinator.run_launch_prepare(*launch.token, counting(prepares));
    });
    ASSERT_TRUE(wait_until([&]() { return submitted(coordinator) == before + 1; }));

    clock.advance(1000ms);
    ASSERT_EQ(prepare.wait_for(real_timeout), std::future_status::ready) << "the launch kept waiting past its setup deadline";
    EXPECT_EQ(prepare.get().outcome, outcome_e::expired_before_start);
    EXPECT_TRUE(coordinator.abort_launch(*launch.token, {}));

    busy.release();
    EXPECT_TRUE(blocker.get().body_started());
    ASSERT_TRUE(flush(coordinator).completed());
    EXPECT_EQ(prepares.load(), 0);
    EXPECT_FALSE(coordinator.status().lease);
  }

  TEST(DisplayTopologyCoordinator, PrepareSubmittedAfterItsDeadlineNeverRuns) {
    fake_clock_t clock;
    std::atomic_int prepares {};
    coordinator_t coordinator { make_options(clock) };
    ASSERT_TRUE(start_ready(coordinator).completed());

    const auto launch = coordinator.admit_launch({ 75, "cert:a" });
    ASSERT_TRUE(launch);
    clock.advance(1000ms);
    EXPECT_EQ(coordinator.run_launch_prepare(*launch.token, counting(prepares)).outcome, outcome_e::expired_before_start);
    EXPECT_FALSE(coordinator.bind_launch(*launch.token)) << "bound a launch whose prepare never ran";
    ASSERT_TRUE(flush(coordinator).completed());
    EXPECT_EQ(prepares.load(), 0);
  }

  TEST(DisplayTopologyCoordinator, PrepareAbandonedAsTheWorkerPicksItUpNeverRuns) {
    fake_clock_t clock;
    std::atomic_int prepares {};
    std::atomic_int probe_calls {};
    gate_t probe_gate;
    auto options = make_options(clock, true);
    options.probes.legacy_restore_pending = [&probe_calls, hold = probe_gate.hook()]() {
      // The first call comes from admission; the second from the worker, before its start CAS.
      if (++probe_calls == 2) {
        hold();
      }
      return false;
    };
    coordinator_t coordinator { std::move(options) };
    ASSERT_TRUE(start_ready(coordinator).completed());
    std::future<command_result_t> prepare;
    const finally_t release { [&]() { probe_gate.release(); } };

    const auto launch = coordinator.admit_launch({ 72, "cert:a" });
    ASSERT_TRUE(launch);
    prepare = std::async(std::launch::async, [&]() {
      return coordinator.run_launch_prepare(*launch.token, counting(prepares));
    });
    ASSERT_TRUE(probe_gate.wait_started());

    clock.advance(1000ms);
    ASSERT_EQ(prepare.wait_for(real_timeout), std::future_status::ready);
    EXPECT_EQ(prepare.get().outcome, outcome_e::expired_before_start);
    probe_gate.release();
    // The worker finishes with the prepare while the lease is still held, so only the
    // abandon check (not a released lease) can keep the body from running.
    ASSERT_TRUE(flush(coordinator).completed());
    EXPECT_EQ(prepares.load(), 0);
    EXPECT_TRUE(coordinator.abort_launch(*launch.token, {}));
    ASSERT_TRUE(flush(coordinator).completed());
    EXPECT_EQ(prepares.load(), 0);
  }

  TEST(DisplayTopologyCoordinator, PrepareExpiringWhileRunningIsCancelledAloneAndCleanupRunsAfterIt) {
    fake_clock_t clock;
    event_log_t log;
    coordinator_t coordinator { make_options(clock) };
    ASSERT_TRUE(start_ready(coordinator).completed());
    std::promise<void> prepare_started;
    auto prepare_started_future = prepare_started.get_future();
    std::future<command_result_t> prepare;

    const auto launch = coordinator.admit_launch({ 81, "cert:a" });
    ASSERT_TRUE(launch);
    prepare = std::async(std::launch::async, [&]() {
      return coordinator.run_launch_prepare(*launch.token, until_cancelled(log, prepare_started));
    });
    ASSERT_EQ(prepare_started_future.wait_for(real_timeout), std::future_status::ready);
    ASSERT_EQ(coordinator.post(command_kind_e::rotate, logging(log, "rotate")), reject_reason_e::none);

    clock.advance(1000ms);
    ASSERT_EQ(prepare.wait_for(real_timeout), std::future_status::ready);
    EXPECT_EQ(prepare.get().outcome, outcome_e::expired_while_running);
    // Binding decision 4: the expiry alone cancels the running prepare; abort_launch below would mask it.
    ASSERT_TRUE(wait_until([&]() { return log.contains("prepare:cancelled"); }))
      << "expiry alone did not cancel the running prepare";
    EXPECT_TRUE(coordinator.abort_launch(*launch.token, logging(log, "cleanup")));
    ASSERT_TRUE(flush(coordinator).completed());

    EXPECT_EQ(log.events(), (std::vector<std::string> { "prepare:start", "prepare:cancelled", "rotate", "cleanup" }));
  }

  TEST(DisplayTopologyCoordinator, PrepareUsesTheDeadlineRecordedAtAdmissionNotTheTokenCopy) {
    fake_clock_t clock;
    std::atomic_bool deadline_seen {};
    coordinator_t coordinator { make_options(clock) };
    ASSERT_TRUE(start_ready(coordinator).completed());
    gate_t prepare_gate;
    std::future<command_result_t> prepare;
    const finally_t release { [&]() { prepare_gate.release(); } };

    const auto launch = coordinator.admit_launch({ 191, "cert:a" });
    ASSERT_TRUE(launch);
    auto tampered = *launch.token;
    tampered.setup_deadline = monotonic_time_t::max();
    prepare = std::async(std::launch::async, [&]() {
      return coordinator.run_launch_prepare(tampered,
        [&deadline_seen, hold = prepare_gate.hook()](const mutation_capability_t &, const command_context_t &context) {
          hold();
          deadline_seen = context.deadline_expired();
        });
    });
    ASSERT_TRUE(prepare_gate.wait_started());

    clock.advance(1000ms);
    ASSERT_EQ(prepare.wait_for(real_timeout), std::future_status::ready) << "the route waited on the token's deadline";
    EXPECT_EQ(prepare.get().outcome, outcome_e::expired_while_running);
    prepare_gate.release();
    ASSERT_TRUE(flush(coordinator).completed());
    EXPECT_TRUE(deadline_seen) << "the running prepare saw the token's deadline";
  }

  TEST(DisplayTopologyCoordinator, AbortCancelsItsOwnRunningPrepareAndCleanupRunsAfterIt) {
    fake_clock_t clock;
    event_log_t log;
    coordinator_t coordinator { make_options(clock) };
    ASSERT_TRUE(start_ready(coordinator).completed());
    std::promise<void> prepare_started;
    auto prepare_started_future = prepare_started.get_future();
    std::future<command_result_t> prepare;

    const auto launch = coordinator.admit_launch({ 261, "cert:a" });
    ASSERT_TRUE(launch);
    prepare = std::async(std::launch::async, [&]() {
      return coordinator.run_launch_prepare(*launch.token, until_cancelled(log, prepare_started));
    });
    ASSERT_EQ(prepare_started_future.wait_for(real_timeout), std::future_status::ready);
    ASSERT_EQ(coordinator.post(command_kind_e::rotate, logging(log, "rotate")), reject_reason_e::none);

    EXPECT_TRUE(coordinator.abort_launch(*launch.token, logging(log, "cleanup")));
    ASSERT_EQ(prepare.wait_for(real_timeout), std::future_status::ready);
    EXPECT_EQ(prepare.get().outcome, outcome_e::completed_late);
    ASSERT_TRUE(flush(coordinator).completed());

    EXPECT_EQ(log.events(), (std::vector<std::string> { "prepare:start", "prepare:cancelled", "rotate", "cleanup" }));
  }

  TEST(DisplayTopologyCoordinator, AbortLaunchOnlyReleasesAPreparingLaunch) {
    fake_clock_t clock;
    {
      std::atomic_int cleanups {};
      std::vector<command_kind_e> cleanup_kinds;
      std::mutex kinds_mutex;
      coordinator_t legacy { make_options(clock) };
      ASSERT_TRUE(start_ready(legacy).completed());

      const auto failing = legacy.admit_launch({ 91, "cert:a" });
      ASSERT_TRUE(failing);
      EXPECT_TRUE(legacy.abort_launch(*failing.token,
        [&](const mutation_capability_t &capability, const command_context_t &) {
          std::lock_guard lock { kinds_mutex };
          cleanup_kinds.push_back(capability.kind());
          ++cleanups;
        }));
      EXPECT_FALSE(legacy.status().lease);

      const auto bound = legacy.admit_launch({ 92, "cert:a" });
      ASSERT_TRUE(bound);
      ASSERT_TRUE(prepare_and_bind(legacy, *bound.token));
      EXPECT_FALSE(legacy.abort_launch(*bound.token, counting(cleanups)));
      auto stale = *bound.token;
      stale.transaction += 100;
      EXPECT_FALSE(legacy.abort_launch(stale, counting(cleanups)));
      ASSERT_TRUE(flush(legacy).completed());

      EXPECT_EQ(cleanups.load(), 1);
      std::lock_guard lock { kinds_mutex };
      EXPECT_EQ(cleanup_kinds, (std::vector<command_kind_e> { command_kind_e::launch_failed_restore }));
      ASSERT_TRUE(legacy.status().lease);
      EXPECT_EQ(legacy.status().lease->stage, lease_stage_e::bound);
    }
    {
      std::atomic_int legacy_cleanups {};
      std::vector<command_kind_e> restore_kinds;
      std::mutex kinds_mutex;
      auto options = make_options(clock, true);
      options.native_restore = [&](const mutation_capability_t &capability, const command_context_t &) {
        capability.require(operation_e::native_restore);
        std::lock_guard lock { kinds_mutex };
        restore_kinds.push_back(capability.kind());
      };
      coordinator_t managed { std::move(options) };
      ASSERT_TRUE(start_ready(managed).completed());

      const auto failing = managed.admit_launch({ 93, "cert:a" });
      ASSERT_TRUE(failing);
      EXPECT_TRUE(managed.abort_launch(*failing.token, counting(legacy_cleanups)));
      EXPECT_FALSE(managed.abort_launch(*failing.token, counting(legacy_cleanups)));
      ASSERT_TRUE(flush(managed).completed());

      EXPECT_EQ(legacy_cleanups.load(), 0);
      EXPECT_EQ(managed.status().native_restores_posted, 1u);
      EXPECT_FALSE(managed.status().lease);
      std::lock_guard lock { kinds_mutex };
      EXPECT_EQ(restore_kinds, (std::vector<command_kind_e> { command_kind_e::launch_failed_restore }));
    }
  }

  TEST(DisplayTopologyCoordinator, ManagedAbandonedTicketPostsExactlyOneNativeRestore) {
    fake_clock_t clock;
    std::atomic_bool ticket_present { true };
    std::atomic_int restores {};
    posted_log_t posted;
    auto options = make_options(clock, true);
    options.probes.ticket_present = [&](std::uint32_t) {
      return ticket_present.load();
    };
    options.native_restore = [&](const mutation_capability_t &capability, const command_context_t &) {
      capability.require(operation_e::native_restore);
      ++restores;
    };
    options.posted_observer = posted.observer();
    coordinator_t coordinator { std::move(options) };
    ASSERT_TRUE(start_ready(coordinator).completed());

    const auto launch = coordinator.admit_launch({ 101, "cert:a" });
    ASSERT_TRUE(launch);
    ASSERT_TRUE(coordinator.run_launch_prepare(*launch.token, {}).completed());
    ASSERT_TRUE(coordinator.bind_launch(*launch.token));
    coordinator.tick();
    ASSERT_TRUE(coordinator.status().lease);

    ticket_present = false;
    coordinator.tick();
    coordinator.tick();
    ASSERT_TRUE(flush(coordinator).completed());

    EXPECT_EQ(restores.load(), 1);
    EXPECT_EQ(coordinator.status().native_restores_posted, 1u);
    EXPECT_FALSE(coordinator.status().lease);
    const auto records = posted.records();
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].kind, command_kind_e::native_restore);
    EXPECT_EQ(records[0].outcome, outcome_e::completed);
  }

  TEST(DisplayTopologyCoordinator, AdmissionRestoresAnAbandonedManagedTicketBeforeTheNextPrepare) {
    fake_clock_t clock;
    std::atomic_bool ticket_present { true };
    event_log_t log;
    auto options = make_options(clock, true);
    options.probes.ticket_present = [&](std::uint32_t) {
      return ticket_present.load();
    };
    options.native_restore = [&](const mutation_capability_t &capability, const command_context_t &) {
      capability.require(operation_e::native_restore);
      log.add("restore");
    };
    coordinator_t coordinator { std::move(options) };
    ASSERT_TRUE(start_ready(coordinator).completed());

    const auto first = coordinator.admit_launch({ 341, "cert:a" });
    ASSERT_TRUE(first);
    ASSERT_TRUE(prepare_and_bind(coordinator, *first.token));
    EXPECT_EQ(coordinator.admit_launch({ 342, "cert:b" }).reason, reject_reason_e::owner_busy);

    ticket_present = false;
    const auto second = coordinator.admit_launch({ 342, "cert:b" });
    ASSERT_TRUE(second) << to_string(second.reason);
    EXPECT_TRUE(coordinator.run_launch_prepare(*second.token, logging(log, "prepare")).completed());
    EXPECT_EQ(coordinator.status().native_restores_posted, 1u);
    EXPECT_EQ(log.events(), (std::vector<std::string> { "restore", "prepare" }));
  }

  TEST(DisplayTopologyCoordinator, AStaleTicketAnswerNeverEndsALeaseThatChangedWhileItWasProbed) {
    struct case_t {
      const char *name;
      bool managed;
      bool via_tick;
      bool replaced;  // Otherwise the stream connects (and consumes the ticket) during the probe.
    };
    const std::vector<case_t> cases {
      { "admission, lease replaced", false, false, true },
      { "admission, stream connected", true, false, false },
      { "tick, lease replaced", false, true, true },
      { "tick, stream connected", true, true, false },
    };
    for (const auto &test_case : cases) {
      SCOPED_TRACE(test_case.name);
      fake_clock_t clock;
      gate_t probe_gate;
      std::atomic_int probes {};
      std::atomic_int restores {};
      auto options = make_options(clock, test_case.managed);
      options.native_restore = counting(restores);
      // The first answer is "gone", and it arrives only after the lease has changed.
      options.probes.ticket_present = [&probes, hold = probe_gate.hook()](std::uint32_t) {
        if (++probes == 1) {
          hold();
          return false;
        }
        return true;
      };
      coordinator_t coordinator { std::move(options) };
      ASSERT_TRUE(start_ready(coordinator).completed());
      std::future<reject_reason_e> probing;
      const finally_t release { [&]() { probe_gate.release(); } };

      const auto first = coordinator.admit_launch({ 311, "cert:a" });
      ASSERT_TRUE(first);
      ASSERT_TRUE(prepare_and_bind(coordinator, *first.token));
      probing = std::async(std::launch::async, [&]() {
        if (test_case.via_tick) {
          coordinator.tick();
          return reject_reason_e::none;
        }
        return coordinator.admit_launch({ 313, "cert:c" }).reason;
      });
      ASSERT_TRUE(probe_gate.wait_started());

      EXPECT_TRUE(coordinator.launch_activated(*first.token));
      std::uint32_t expected_id = 311;
      auto expected_stage = lease_stage_e::active;
      if (test_case.replaced) {
        const auto second = coordinator.admit_launch({ 312, "cert:b" });
        ASSERT_TRUE(second) << to_string(second.reason);
        ASSERT_TRUE(prepare_and_bind(coordinator, *second.token));
        expected_id = 312;
        expected_stage = lease_stage_e::bound;
      }
      probe_gate.release();

      ASSERT_EQ(probing.wait_for(real_timeout), std::future_status::ready);
      if (!test_case.via_tick) {
        EXPECT_EQ(probing.get(), reject_reason_e::owner_busy);
      }
      ASSERT_TRUE(flush(coordinator).completed());
      const auto status = coordinator.status();
      ASSERT_TRUE(status.lease) << "a stale 'ticket gone' answer ended the lease";
      EXPECT_EQ(status.lease->launch_session_id, expected_id);
      EXPECT_EQ(status.lease->stage, expected_stage);
      EXPECT_EQ(status.native_restores_posted, 0u);
      EXPECT_EQ(restores.load(), 0);
    }
  }

  TEST(DisplayTopologyCoordinator, AdmissionRechecksThePhaseAfterItsUnlockedProbes) {
    fake_clock_t clock;
    gate_t probe_gate;
    auto options = make_options(clock);
    // The bound ticket is gone, but the answer arrives only after draining began.
    options.probes.ticket_present = [hold = probe_gate.hook()](std::uint32_t) {
      hold();
      return false;
    };
    coordinator_t coordinator { std::move(options) };
    ASSERT_TRUE(start_ready(coordinator).completed());
    std::future<launch_admission_t> admission;
    const finally_t release { [&]() { probe_gate.release(); } };

    const auto first = coordinator.admit_launch({ 351, "cert:a" });
    ASSERT_TRUE(first);
    ASSERT_TRUE(prepare_and_bind(coordinator, *first.token));
    admission = std::async(std::launch::async, [&]() {
      return coordinator.admit_launch({ 352, "cert:b" });
    });
    ASSERT_TRUE(probe_gate.wait_started());
    coordinator.begin_draining();
    probe_gate.release();

    ASSERT_EQ(admission.wait_for(real_timeout), std::future_status::ready);
    const auto result = admission.get();
    EXPECT_FALSE(result) << "a launch was admitted after draining began";
    EXPECT_EQ(result.reason, reject_reason_e::draining);
    const auto status = coordinator.status();
    ASSERT_TRUE(status.lease) << "a refused admission changed the lease";
    EXPECT_EQ(status.lease->launch_session_id, 351u);
  }

  TEST(DisplayTopologyCoordinator, ManagedIdleStreamIsReleasedOnlyAfterTheGracePeriod) {
    fake_clock_t clock;
    std::atomic<std::size_t> streams { 1 };
    std::atomic_int restores {};
    auto options = make_options(clock, true);
    options.probes.active_streams = [&]() {
      return streams.load();
    };
    options.native_restore = counting(restores);
    coordinator_t coordinator { std::move(options) };
    ASSERT_TRUE(start_ready(coordinator).completed());

    const auto launch = coordinator.admit_launch({ 111, "cert:a" });
    ASSERT_TRUE(launch);
    ASSERT_TRUE(prepare_and_bind(coordinator, *launch.token));
    EXPECT_TRUE(coordinator.launch_activated(*launch.token));
    const auto held = [&]() {
      return coordinator.status().lease.has_value();
    };

    coordinator.tick();
    EXPECT_TRUE(held()) << "a live stream must keep the lease";
    streams = 0;
    coordinator.tick();
    EXPECT_TRUE(held()) << "the first idle tick only starts the grace period";
    clock.advance(499ms);
    coordinator.tick();
    EXPECT_TRUE(held()) << "released before the grace period elapsed";
    streams = 1;
    coordinator.tick();
    streams = 0;
    coordinator.tick();
    clock.advance(499ms);
    coordinator.tick();
    EXPECT_TRUE(held()) << "a stream reconnect must restart the grace period";
    clock.advance(1ms);
    coordinator.tick();
    EXPECT_FALSE(held());
    ASSERT_TRUE(flush(coordinator).completed());
    EXPECT_EQ(restores.load(), 1);
  }

  // Legacy parity: a paused application keeps its display; only its exit starts the grace period.
  TEST(DisplayTopologyCoordinator, ARunningApplicationHoldsAnIdleManagedLease) {
    fake_clock_t clock;
    std::atomic_bool app { true };
    std::atomic_int restores {};
    auto options = make_options(clock, true);
    options.probes.active_streams = []() { return std::size_t { 0 }; };
    options.probes.app_running = [&]() { return app.load(); };
    options.native_restore = counting(restores);
    coordinator_t coordinator { std::move(options) };
    ASSERT_TRUE(start_ready(coordinator).completed());
    const auto launch = coordinator.admit_launch({ 112, "cert:a" });
    ASSERT_TRUE(launch);
    ASSERT_TRUE(prepare_and_bind(coordinator, *launch.token));
    ASSERT_TRUE(coordinator.launch_activated(*launch.token));

    coordinator.tick();
    clock.advance(10s);
    coordinator.tick();
    EXPECT_TRUE(coordinator.status().lease) << "released while the application was still running";
    app = false;
    coordinator.tick();
    clock.advance(499ms);
    coordinator.tick();
    EXPECT_TRUE(coordinator.status().lease) << "the application's exit must start, not skip, the grace period";
    clock.advance(1ms);
    coordinator.tick();
    EXPECT_FALSE(coordinator.status().lease);
    ASSERT_TRUE(flush(coordinator).completed());
    EXPECT_EQ(restores.load(), 1);
  }

  TEST(DisplayTopologyCoordinator, ResumeJoinsALeaseHeldOnlyByItsRunningApplication) {
    fake_clock_t clock;
    std::atomic_bool app { true };
    std::atomic_int restores {};
    auto options = make_options(clock, true);
    options.probes.active_streams = []() { return std::size_t { 0 }; };
    options.probes.app_running = [&]() { return app.load(); };
    options.native_restore = counting(restores);
    coordinator_t coordinator { std::move(options) };
    ASSERT_TRUE(start_ready(coordinator).completed());
    const auto owner = coordinator.admit_launch({ 113, "cert:a" });
    ASSERT_TRUE(owner);
    ASSERT_TRUE(prepare_and_bind(coordinator, *owner.token));
    ASSERT_TRUE(coordinator.launch_activated(*owner.token));
    ASSERT_TRUE(coordinator.end_managed_launch(*owner.token));
    ASSERT_TRUE(coordinator.status().lease) << "the paused application keeps its display";
    EXPECT_EQ(coordinator.status().lease->attachments, 0u);

    const auto resumed = coordinator.admit_managed_join({ 114, "cert:a" });
    ASSERT_TRUE(resumed) << to_string(resumed.reason);
    ASSERT_TRUE(coordinator.bind_launch(*resumed.token));
    ASSERT_TRUE(coordinator.launch_activated(*resumed.token));
    ASSERT_TRUE(coordinator.end_managed_launch(*resumed.token));
    ASSERT_TRUE(coordinator.status().lease);
    app = false;
    coordinator.tick();
    EXPECT_FALSE(coordinator.status().lease) << "a quiescent lease ends with its application";
    EXPECT_EQ(coordinator.admit_managed_join({ 115, "cert:a" }).reason, reject_reason_e::stale_transaction);
    ASSERT_TRUE(flush(coordinator).completed());
    EXPECT_EQ(restores.load(), 1);
  }

  TEST(DisplayTopologyCoordinator, OnlyThePreparedOwnerRecordsTheManagedTargetAndJoinersReadIt) {
    fake_clock_t clock;
    coordinator_t coordinator { make_options(clock, true) };
    ASSERT_TRUE(start_ready(coordinator).completed());
    const managed_target_t target { "{zako-head}", true };
    const auto owner = coordinator.admit_launch({ 501, "a" });
    ASSERT_TRUE(owner);
    EXPECT_FALSE(coordinator.record_managed_display(*owner.token, target)) << "nothing was prepared yet";
    EXPECT_FALSE(coordinator.managed_display(*owner.token));
    ASSERT_TRUE(coordinator.run_launch_prepare(*owner.token, {}).completed());
    auto stale = *owner.token;
    ++stale.transaction;
    EXPECT_FALSE(coordinator.record_managed_display(stale, target));
    EXPECT_FALSE(coordinator.record_managed_display(*owner.token, { "", false })) << "an empty display is never a target";
    ASSERT_TRUE(coordinator.record_managed_display(*owner.token, target));
    EXPECT_EQ(coordinator.managed_display(*owner.token), target);
    EXPECT_FALSE(coordinator.managed_display(stale));
    ASSERT_TRUE(coordinator.bind_launch(*owner.token));
    EXPECT_FALSE(coordinator.record_managed_display(*owner.token, { "{other}", false })) << "a bound lease keeps its target";
    ASSERT_TRUE(coordinator.launch_activated(*owner.token));

    const auto join = coordinator.admit_managed_join({ 502, "b" });
    ASSERT_TRUE(join);
    EXPECT_FALSE(coordinator.record_managed_display(*join.token, { "{other}", false })) << "a joiner never prepares";
    EXPECT_EQ(coordinator.managed_display(*join.token), target);
    ASSERT_TRUE(coordinator.abort_launch(*join.token, {}));
    EXPECT_FALSE(coordinator.managed_display(*join.token)) << "an ended reference no longer reads the target";
    ASSERT_TRUE(coordinator.end_managed_launch(*owner.token));
    EXPECT_FALSE(coordinator.managed_display(*owner.token));
  }

  TEST(DisplayTopologyCoordinator, ALegacyLeaseHasNoManagedTarget) {
    fake_clock_t clock;
    coordinator_t coordinator { make_options(clock, false) };
    ASSERT_TRUE(start_ready(coordinator).completed());
    const auto owner = coordinator.admit_launch({ 503, "a" });
    ASSERT_TRUE(owner);
    ASSERT_TRUE(coordinator.run_launch_prepare(*owner.token, {}).completed());
    EXPECT_FALSE(coordinator.record_managed_display(*owner.token, { "{zako-head}", true }));
    EXPECT_FALSE(coordinator.managed_display(*owner.token));
    EXPECT_TRUE(coordinator.abort_launch(*owner.token, {}));
  }

  TEST(DisplayTopologyCoordinator, EndManagedLeasePostsTheRestoreExactlyOnce) {
    fake_clock_t clock;
    std::vector<command_kind_e> restore_kinds;
    std::mutex kinds_mutex;
    {
      auto options = make_options(clock, true);
      options.native_restore = [&](const mutation_capability_t &capability, const command_context_t &) {
        capability.require(operation_e::native_restore);
        std::lock_guard lock { kinds_mutex };
        restore_kinds.push_back(capability.kind());
      };
      coordinator_t managed { std::move(options) };
      ASSERT_TRUE(start_ready(managed).completed());

      const auto launch = managed.admit_launch({ 121, "cert:a" });
      ASSERT_TRUE(launch);
      EXPECT_FALSE(managed.end_managed_launch(*launch.token)) << "a preparing reference ends through abort_launch";
      ASSERT_TRUE(prepare_and_bind(managed, *launch.token));
      EXPECT_TRUE(managed.launch_activated(*launch.token));
      EXPECT_THROW(static_cast<void>(managed.end_managed_launch(*launch.token, command_kind_e::rotate)), std::invalid_argument);
      EXPECT_TRUE(managed.end_managed_launch(*launch.token));
      EXPECT_FALSE(managed.end_managed_launch(*launch.token));
      ASSERT_TRUE(flush(managed).completed());
      EXPECT_EQ(managed.status().native_restores_posted, 1u);
    }
    {
      std::lock_guard lock { kinds_mutex };
      EXPECT_EQ(restore_kinds, (std::vector<command_kind_e> { command_kind_e::stream_restore }));
    }

    coordinator_t legacy { make_options(clock) };
    ASSERT_TRUE(start_ready(legacy).completed());
    const auto launch = legacy.admit_launch({ 122, "cert:a" });
    ASSERT_TRUE(launch);
    ASSERT_TRUE(prepare_and_bind(legacy, *launch.token));
    EXPECT_FALSE(legacy.end_managed_launch(*launch.token));
    EXPECT_TRUE(legacy.status().lease);
  }

  TEST(DisplayTopologyCoordinator, AStreamEndLeavesABoundManagedLeaseAlone) {
    fake_clock_t clock;
    std::atomic_int restores {};
    auto options = make_options(clock, true);
    options.native_restore = counting(restores);
    coordinator_t coordinator { std::move(options) };
    ASSERT_TRUE(start_ready(coordinator).completed());

    const auto previous = coordinator.admit_launch({ 210, "cert:a" });
    ASSERT_TRUE(previous);
    ASSERT_TRUE(prepare_and_bind(coordinator, *previous.token));
    ASSERT_TRUE(coordinator.launch_activated(*previous.token));
    ASSERT_TRUE(coordinator.end_managed_launch(*previous.token));

    // A duplicate terminal callback from the previous stream cannot end this bound reference.
    const auto launch = coordinator.admit_launch({ 211, "cert:a" });
    ASSERT_TRUE(launch);
    ASSERT_TRUE(prepare_and_bind(coordinator, *launch.token));
    EXPECT_FALSE(coordinator.end_managed_launch(*previous.token)) << "a late stream end released a bound lease";
    auto status = coordinator.status();
    ASSERT_TRUE(status.lease);
    EXPECT_EQ(status.lease->launch_session_id, 211u);
    EXPECT_EQ(status.lease->stage, lease_stage_e::bound);
    EXPECT_EQ(status.native_restores_posted, 1u);

    EXPECT_TRUE(coordinator.launch_activated(*launch.token));
    EXPECT_TRUE(coordinator.end_managed_launch(*launch.token));
    ASSERT_TRUE(flush(coordinator).completed());
    EXPECT_EQ(restores.load(), 2);
  }

  TEST(DisplayTopologyCoordinator, ALateStreamEndCannotEndTheNextLaunchsLease) {
    fake_clock_t clock;
    std::atomic_int restores {};
    auto options = make_options(clock, true);
    options.native_restore = counting(restores);
    coordinator_t coordinator { std::move(options) };
    ASSERT_TRUE(start_ready(coordinator).completed());

    const auto first = coordinator.admit_launch({ 201, "cert:a" });
    ASSERT_TRUE(first);
    ASSERT_TRUE(prepare_and_bind(coordinator, *first.token));
    EXPECT_TRUE(coordinator.launch_activated(*first.token));
    ASSERT_TRUE(coordinator.end_managed_launch(*first.token));

    const auto second = coordinator.admit_launch({ 202, "cert:b" });
    ASSERT_TRUE(second);
    ASSERT_TRUE(prepare_and_bind(coordinator, *second.token));
    EXPECT_TRUE(coordinator.launch_activated(*second.token));
    EXPECT_FALSE(coordinator.end_managed_launch(*first.token)) << "launch 201's stream end released launch 202";
    const auto status = coordinator.status();
    ASSERT_TRUE(status.lease);
    EXPECT_EQ(status.lease->launch_session_id, 202u);
    EXPECT_EQ(status.lease->stage, lease_stage_e::active);
    EXPECT_EQ(status.native_restores_posted, 1u);

    EXPECT_TRUE(coordinator.end_managed_launch(*second.token));
    ASSERT_TRUE(flush(coordinator).completed());
    EXPECT_EQ(restores.load(), 2);
  }

  TEST(DisplayTopologyCoordinator, AFailedNativeRestoreMarksNativeInConflictBeforeTheNextPrepare) {
    fake_clock_t clock;
    posted_log_t posted;
    std::atomic_int prepares {};
    auto options = make_options(clock, true);
    options.native_restore = [](const mutation_capability_t &, const command_context_t &) {
      throw std::runtime_error { "virtual display removal failed" };
    };
    options.posted_observer = posted.observer();
    coordinator_t coordinator { std::move(options) };
    ASSERT_TRUE(start_ready(coordinator).completed());
    gate_t busy;
    std::future<command_result_t> blocker;
    std::future<command_result_t> prepare;
    const finally_t release { [&]() { busy.release(); } };

    const auto first = coordinator.admit_launch({ 271, "cert:a" });
    ASSERT_TRUE(first);
    ASSERT_TRUE(prepare_and_bind(coordinator, *first.token));
    EXPECT_TRUE(coordinator.launch_activated(*first.token));
    blocker = std::async(std::launch::async, [&]() {
      return coordinator.run(command_kind_e::stream_restore, busy.body());
    });
    ASSERT_TRUE(busy.wait_started());
    ASSERT_TRUE(coordinator.end_managed_launch(*first.token));

    // Admitted before the restore ran, so only the worker can see that it failed.
    const auto second = coordinator.admit_launch({ 272, "cert:a" });
    ASSERT_TRUE(second) << to_string(second.reason);
    const auto before = submitted(coordinator);
    prepare = std::async(std::launch::async, [&]() {
      return coordinator.run_launch_prepare(*second.token, counting(prepares));
    });
    ASSERT_TRUE(wait_until([&]() { return submitted(coordinator) == before + 1; }));
    busy.release();

    const auto result = prepare.get();
    EXPECT_EQ(result.outcome, outcome_e::rejected);
    EXPECT_EQ(result.reason, reject_reason_e::recovery_required);
    EXPECT_EQ(prepares.load(), 0);
    const auto status = coordinator.status();
    EXPECT_TRUE(status.native.in_conflict);
    EXPECT_EQ(status.native.reason, "Native restore failed");
    const auto records = posted.records();
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].kind, command_kind_e::stream_restore);
    EXPECT_EQ(records[0].outcome, outcome_e::failed);
    EXPECT_TRUE(records[0].has_exception);
  }

  TEST(DisplayTopologyCoordinator, TickThreadReleasesAnAbandonedManagedLease) {
    fake_clock_t clock;
    std::atomic_bool ticket_present { true };
    std::atomic_int restores {};
    auto options = make_options(clock, true);
    options.tick_interval = 1ms;
    options.probes.ticket_present = [&](std::uint32_t) {
      return ticket_present.load();
    };
    options.native_restore = counting(restores);
    coordinator_t coordinator { std::move(options) };
    ASSERT_TRUE(start_ready(coordinator).completed());

    const auto launch = coordinator.admit_launch({ 131, "cert:a" });
    ASSERT_TRUE(launch);
    ASSERT_TRUE(prepare_and_bind(coordinator, *launch.token));
    ticket_present = false;

    EXPECT_TRUE(wait_until([&]() { return coordinator.status().native_restores_posted == 1; }));
    ASSERT_TRUE(flush(coordinator).completed());
    EXPECT_EQ(restores.load(), 1);
  }

  TEST(DisplayTopologyCoordinator, ExpiryCancelsOnlyItsOwnCommand) {
    fake_clock_t clock;
    event_log_t log;
    coordinator_t coordinator { make_options(clock) };
    ASSERT_TRUE(start_ready(coordinator).completed());
    gate_t busy;
    std::future<command_result_t> blocker;
    std::future<command_result_t> prepare;
    const finally_t release { [&]() { busy.release(); } };

    blocker = std::async(std::launch::async, [&]() {
      return coordinator.run(command_kind_e::rotate, busy.body());
    });
    ASSERT_TRUE(busy.wait_started());
    ASSERT_EQ(coordinator.post(command_kind_e::scale, logging(log, "scale-before")), reject_reason_e::none);
    const auto launch = coordinator.admit_launch({ 141, "cert:a" });
    ASSERT_TRUE(launch);
    const auto before = submitted(coordinator);
    prepare = std::async(std::launch::async, [&]() {
      return coordinator.run_launch_prepare(*launch.token, logging(log, "prepare"));
    });
    ASSERT_TRUE(wait_until([&]() { return submitted(coordinator) == before + 1; }));
    ASSERT_EQ(coordinator.post(command_kind_e::scale, logging(log, "scale-after")), reject_reason_e::none);

    clock.advance(1000ms);
    ASSERT_EQ(prepare.wait_for(real_timeout), std::future_status::ready);
    EXPECT_EQ(prepare.get().outcome, outcome_e::expired_before_start);
    busy.release();
    EXPECT_EQ(blocker.get().outcome, outcome_e::completed);
    ASSERT_TRUE(flush(coordinator).completed());

    EXPECT_EQ(log.events(), (std::vector<std::string> { "scale-before", "scale-after" }));
  }

  // ---------------------------------------------------------------------------
  // Draining, stop, fence, reentrancy and posted failures
  // ---------------------------------------------------------------------------

  TEST(DisplayTopologyCoordinator, DrainingRefusesNewWorkCancelsThePreparingLaunchAndAllowsCleanup) {
    fake_clock_t clock;
    std::atomic_bool observed_cancel {};
    std::atomic_int cleanups {};
    std::atomic_int others {};
    coordinator_t coordinator { make_options(clock) };
    ASSERT_TRUE(start_ready(coordinator).completed());
    std::promise<void> prepare_started;
    auto prepare_started_future = prepare_started.get_future();
    std::future<command_result_t> prepare;

    const auto launch = coordinator.admit_launch({ 151, "cert:a" });
    ASSERT_TRUE(launch);
    prepare = std::async(std::launch::async, [&]() {
      return coordinator.run_launch_prepare(*launch.token,
        [&](const mutation_capability_t &, const command_context_t &context) {
          prepare_started.set_value();
          const auto give_up = std::chrono::steady_clock::now() + real_timeout;
          while (!context.cancellation_requested() && std::chrono::steady_clock::now() < give_up) {
            std::this_thread::sleep_for(1ms);
          }
          observed_cancel = context.cancellation_requested();
        });
    });
    ASSERT_EQ(prepare_started_future.wait_for(real_timeout), std::future_status::ready);

    coordinator.begin_draining();
    EXPECT_EQ(prepare.get().outcome, outcome_e::completed_late);
    EXPECT_TRUE(observed_cancel);
    EXPECT_EQ(coordinator.status().phase, coordinator_phase_e::draining);
    EXPECT_EQ(coordinator.admit_launch({ 152, "cert:b" }).reason, reject_reason_e::draining);
    EXPECT_EQ(coordinator.post(command_kind_e::rotate, counting(others)), reject_reason_e::draining);
    EXPECT_EQ(coordinator.run(command_kind_e::scale, counting(others)).reason, reject_reason_e::draining);

    EXPECT_TRUE(coordinator.abort_launch(*launch.token, counting(cleanups)));
    EXPECT_TRUE(coordinator.run(command_kind_e::shutdown_restore, counting(cleanups)).completed());
    EXPECT_TRUE(coordinator.run(command_kind_e::shutdown_vdd_destroy, counting(cleanups)).completed());
    EXPECT_EQ(cleanups.load(), 3);
    EXPECT_EQ(others.load(), 0);
  }

  TEST(DisplayTopologyCoordinator, StopCompletesQueuedWaitersAsStopped) {
    fake_clock_t clock;
    std::atomic_int scales {};
    coordinator_t coordinator { make_options(clock) };
    ASSERT_TRUE(start_ready(coordinator).completed());
    gate_t busy;
    std::future<command_result_t> blocker;
    std::future<command_result_t> queued;
    std::future<void> stopper;
    const finally_t release { [&]() { busy.release(); } };

    blocker = std::async(std::launch::async, [&]() {
      return coordinator.run(command_kind_e::rotate, busy.body());
    });
    ASSERT_TRUE(busy.wait_started());
    const auto before = submitted(coordinator);
    queued = std::async(std::launch::async, [&]() {
      return coordinator.run(command_kind_e::scale, counting(scales));
    });
    ASSERT_TRUE(wait_until([&]() { return submitted(coordinator) == before + 1; }));

    stopper = std::async(std::launch::async, [&]() {
      coordinator.stop();
    });
    ASSERT_TRUE(wait_until([&]() { return coordinator.status().phase == coordinator_phase_e::stopped; }));
    busy.release();

    EXPECT_EQ(blocker.get().outcome, outcome_e::completed_late);
    EXPECT_EQ(queued.get().outcome, outcome_e::stopped);
    stopper.get();
    EXPECT_EQ(scales.load(), 0);
    EXPECT_EQ(coordinator.run(command_kind_e::rotate, {}).reason, reject_reason_e::stopped);
    EXPECT_EQ(coordinator.post(command_kind_e::stream_restore, {}), reject_reason_e::stopped);
    EXPECT_EQ(coordinator.admit_launch({ 161, "cert:a" }).reason, reject_reason_e::stopped);
  }

  TEST(DisplayTopologyCoordinator, ShutdownNeverDestroysAVirtualDisplayTheNativeJournalMayOwn) {
    fake_clock_t clock;
    std::atomic_int destroys {};
    std::atomic_int user_destroys {};
    coordinator_t coordinator { make_options(clock) };
    ASSERT_TRUE(start_ready(coordinator).completed());

    coordinator.publish_native_health({ true, false, "Journal present" });
    EXPECT_TRUE(coordinator.run(command_kind_e::vdd_destroy, counting(user_destroys)).completed()) << "an explicit user action is not gated";
    EXPECT_EQ(user_destroys.load(), 1);

    coordinator.begin_draining();
    for (const native_health_t &health : { native_health_t { true, false, "Journal present" },
           native_health_t { false, true, "Native restore failed" } }) {
      coordinator.publish_native_health(health);
      const auto result = coordinator.run(command_kind_e::shutdown_vdd_destroy, counting(destroys));
      EXPECT_EQ(result.outcome, outcome_e::rejected) << health.reason;
      EXPECT_EQ(result.reason, reject_reason_e::recovery_required) << health.reason;
    }
    EXPECT_EQ(destroys.load(), 0);

    coordinator.publish_native_health({});
    EXPECT_TRUE(coordinator.run(command_kind_e::shutdown_vdd_destroy, counting(destroys)).completed());
    EXPECT_EQ(destroys.load(), 1);
  }

  TEST(DisplayTopologyCoordinator, StopFromTheWorkerIsRefusedAndTheCoordinatorKeepsRunning) {
    fake_clock_t clock;
    std::atomic_bool refused {};
    coordinator_t coordinator { make_options(clock) };
    ASSERT_TRUE(start_ready(coordinator).completed());

    const auto result = coordinator.run(command_kind_e::rotate, [&](const mutation_capability_t &, const command_context_t &) {
      try {
        coordinator.stop();
      }
      catch (const std::logic_error &) {
        refused = true;
      }
    });
    EXPECT_TRUE(result.completed()) << to_string(result.outcome);
    EXPECT_TRUE(refused);
    EXPECT_EQ(coordinator.status().phase, coordinator_phase_e::running);
    EXPECT_TRUE(coordinator.run(command_kind_e::scale, {}).completed());
    coordinator.stop();
    EXPECT_EQ(coordinator.status().phase, coordinator_phase_e::stopped);
  }

  TEST(DisplayTopologyCoordinator, ConcurrentStopsReturnOnlyAfterTheWorkerFinished) {
    fake_clock_t clock;
    coordinator_t coordinator { make_options(clock) };
    ASSERT_TRUE(start_ready(coordinator).completed());
    gate_t busy;
    std::future<command_result_t> blocker;
    std::future<void> first_stop;
    std::future<void> second_stop;
    const finally_t release { [&]() { busy.release(); } };

    blocker = std::async(std::launch::async, [&]() {
      return coordinator.run(command_kind_e::rotate, busy.body());
    });
    ASSERT_TRUE(busy.wait_started());
    first_stop = std::async(std::launch::async, [&]() {
      coordinator.stop();
    });
    ASSERT_TRUE(wait_until([&]() { return coordinator.status().phase == coordinator_phase_e::stopped; }));
    second_stop = std::async(std::launch::async, [&]() {
      coordinator.stop();
    });
    EXPECT_EQ(second_stop.wait_for(50ms), std::future_status::timeout) << "a concurrent stop() returned while a command was still running";

    busy.release();
    EXPECT_EQ(first_stop.wait_for(real_timeout), std::future_status::ready);
    EXPECT_EQ(second_stop.wait_for(real_timeout), std::future_status::ready);
    EXPECT_EQ(blocker.get().outcome, outcome_e::completed_late);
  }

  TEST(DisplayTopologyCoordinator, StopFromTheTickThreadIsRefused) {
    fake_clock_t clock;
    std::atomic_bool attempted {};
    std::atomic_bool refused {};
    std::atomic_bool finished {};
    coordinator_t *self = nullptr;
    auto options = make_options(clock);
    options.tick_interval = 1ms;
    options.probes.ticket_present = [&](std::uint32_t) {
      if (!attempted.exchange(true)) {
        try {
          self->stop();
        }
        catch (const std::logic_error &) {
          refused = true;
        }
        catch (...) {
        }
        finished = true;
      }
      return true;
    };
    coordinator_t coordinator { std::move(options) };
    self = &coordinator;
    ASSERT_TRUE(start_ready(coordinator).completed());

    const auto launch = coordinator.admit_launch({ 251, "cert:a" });
    ASSERT_TRUE(launch);
    ASSERT_TRUE(prepare_and_bind(coordinator, *launch.token));
    ASSERT_TRUE(wait_until([&]() { return finished.load(); }));
    EXPECT_TRUE(refused);
    EXPECT_EQ(coordinator.status().phase, coordinator_phase_e::running);
  }

  TEST(DisplayTopologyCoordinator, OverlappingFencesCannotReleaseEachOther) {
    fake_clock_t clock;
    coordinator_t coordinator { make_options(clock) };
    ASSERT_TRUE(start_ready(coordinator).completed());
    std::atomic_bool first {}, second {};
    coordinator.install_fence([&]() { return first.load(); });
    coordinator.install_fence([&]() { return second.load(); });
    second = true;
    EXPECT_EQ(coordinator.run(command_kind_e::rotate, {}).reason, reject_reason_e::fence_active);
    EXPECT_TRUE(coordinator.status().fence_active);
    second = false;
    first = true;
    EXPECT_TRUE(coordinator.run(command_kind_e::rotate, {}).completed())
      << "each settled fence is retired independently, even while another fence remains";
    EXPECT_FALSE(coordinator.status().fence_active);
  }

  TEST(DisplayTopologyCoordinator, TeardownFenceBridgesRegistryRemovalAndCleanupSubmission) {
    fake_clock_t clock;
    event_log_t log;
    coordinator_t coordinator { make_options(clock) };
    ASSERT_TRUE(start_ready(coordinator).completed());
    auto teardown = coordinator.hold_fence();
    log.add("remove session");
    EXPECT_EQ(coordinator.admit_launch({ 320, "early resume" }).reason, reject_reason_e::fence_active);
    std::atomic_int mutations {};
    EXPECT_EQ(coordinator.run(command_kind_e::rotate, counting(mutations)).reason, reject_reason_e::fence_active);
    auto stream_cleanup = coordinator.post_tracked(command_kind_e::stream_restore, logging(log, "stream restore"));
    auto app_cleanup = coordinator.post_tracked(command_kind_e::stream_restore, logging(log, "app restore"));
    EXPECT_EQ(stream_cleanup.reason, reject_reason_e::none);
    EXPECT_EQ(app_cleanup.reason, reject_reason_e::none);
    teardown.reset();
    const auto resume = coordinator.admit_launch({ 321, "resume" });
    ASSERT_TRUE(resume);
    ASSERT_TRUE(coordinator.run_launch_prepare(*resume.token, logging(log, "prepare")).completed());
    EXPECT_EQ(mutations, 0);
    EXPECT_EQ(log.events(), (std::vector<std::string> { "remove session", "stream restore", "app restore", "prepare" }));
  }

  TEST(DisplayTopologyCoordinator, SharedAndIndependentTeardownFencesAllHaveToFinish) {
    fake_clock_t clock;
    coordinator_t coordinator { make_options(clock) };
    ASSERT_TRUE(start_ready(coordinator).completed());
    auto first = coordinator.hold_fence();
    auto callback = first;
    auto second = coordinator.hold_fence();
    first.reset();
    second.reset();
    EXPECT_EQ(coordinator.admit_launch({ 322, "waiting" }).reason, reject_reason_e::fence_active);
    EXPECT_TRUE(coordinator.status().fence_active);
    callback.reset();
    EXPECT_TRUE(coordinator.admit_launch({ 323, "ready" }));
    EXPECT_FALSE(coordinator.status().fence_active);
  }

  TEST(DisplayTopologyCoordinator, EmptyFenceCannotClearAnExistingFence) {
    fake_clock_t clock;
    coordinator_t coordinator { make_options(clock) };
    ASSERT_TRUE(start_ready(coordinator).completed());
    const auto held = coordinator.hold_fence();
    EXPECT_THROW(coordinator.install_fence({}), std::invalid_argument);
    EXPECT_EQ(coordinator.run(command_kind_e::scale, {}).reason, reject_reason_e::fence_active);
  }

  using external_hold_t = std::shared_ptr<const external_mutation_fence_t>;

  TEST(DisplayTopologyCoordinator, ExternalMutationBlocksCurrentCleanupUntilItsLastHolderSettles) {
    fake_clock_t clock;
    coordinator_t coordinator { make_options(clock) };
    ASSERT_TRUE(start_ready(coordinator).completed());
    std::size_t mutations {};
    ASSERT_TRUE(coordinator.run(command_kind_e::reconfigure, [&](const auto &capability, const auto &) {
      auto held = coordinator.hold_external_mutation(capability);
      auto monitor = held;
      const auto restore_after_create_failed = [&]() {
        capability.require(operation_e::restore_state);
        ++mutations;
        capability.require(operation_e::vdd_destroy);
        ++mutations;
      };
      EXPECT_THROW(restore_after_create_failed(), capability_error_t);
      EXPECT_EQ(mutations, 0u);
      for (const auto operation : all_operations) {
        if (kind_allows(capability.kind(), operation, false)) {
          SCOPED_TRACE(to_string(operation));
          EXPECT_THROW(capability.require(operation), capability_error_t);
        }
      }
      EXPECT_TRUE(coordinator.status().fence_active);
      held.reset();
      EXPECT_THROW(capability.require(operation_e::restore_state), capability_error_t);
      monitor.reset();
      EXPECT_NO_THROW(capability.require(operation_e::restore_state));
      EXPECT_NO_THROW(capability.require(operation_e::vdd_destroy));
    }).completed());
    EXPECT_EQ(mutations, 0u);
    EXPECT_FALSE(coordinator.status().fence_active);
  }

  TEST(DisplayTopologyCoordinator, ExternalMutationQueuesCleanupThroughDrainingAndRejectsAdmission) {
    fake_clock_t clock;
    coordinator_t coordinator { make_options(clock) };
    ASSERT_TRUE(start_ready(coordinator).completed());
    external_hold_t held;
    ASSERT_TRUE(coordinator.run(command_kind_e::reconfigure, [&](const auto &capability, const auto &) {
      held = coordinator.hold_external_mutation(capability);
    }).completed());
    auto monitor = held;
    std::atomic_size_t mutations {};
    EXPECT_TRUE(coordinator.status().fence_active);
    EXPECT_EQ(coordinator.admit_launch({ 430, "external operation" }).reason, reject_reason_e::fence_active);
    EXPECT_EQ(coordinator.post_tracked(command_kind_e::rotate, {}).reason, reject_reason_e::fence_active);
    std::vector<posted_command_t> cleanup;
    for (const auto kind : expected_cleanup_kinds) {
      cleanup.push_back(coordinator.post_tracked(kind, [&](const auto &capability, const auto &) {
        capability.require(operation_e::vdd_destroy);
        ++mutations;
      }));
      EXPECT_EQ(cleanup.back().reason, reject_reason_e::none);
    }
    held.reset();
    coordinator.begin_draining();
    for (const auto &command : cleanup) {
      EXPECT_EQ(command.completion.wait_for(50ms), std::future_status::timeout);
    }
    EXPECT_EQ(mutations.load(), 0u);
    monitor.reset();
    for (const auto &command : cleanup) {
      EXPECT_TRUE(command.completion.get().completed());
    }
    EXPECT_EQ(mutations.load(), expected_cleanup_kinds.size());
    EXPECT_FALSE(coordinator.status().fence_active);
  }

  TEST(DisplayTopologyCoordinator, ExternalMutationAcquisitionRejectsInvalidAuthorityWithoutChangingExclusion) {
    fake_clock_t clock;
    coordinator_t coordinator { make_options(clock) };
    coordinator_t other { make_options(clock) };
    ASSERT_TRUE(start_ready(coordinator).completed());
    ASSERT_TRUE(start_ready(other).completed());
    for (const auto kind : { command_kind_e::rotate, command_kind_e::stream_restore }) {
      EXPECT_TRUE(coordinator.run(kind, [&](const auto &capability, const auto &) {
        EXPECT_THROW(coordinator.hold_external_mutation(capability), capability_error_t);
      }).completed());
      EXPECT_FALSE(coordinator.status().fence_active);
    }
    EXPECT_TRUE(coordinator.run(command_kind_e::reconfigure, [&](const auto &capability, const auto &) {
      std::thread outsider { [&]() {
        EXPECT_THROW(coordinator.hold_external_mutation(capability), capability_error_t);
      } };
      outsider.join();
      EXPECT_THROW(other.hold_external_mutation(capability), std::logic_error);
      auto held = coordinator.hold_external_mutation(capability);
      EXPECT_THROW(coordinator.hold_external_mutation(capability), capability_error_t);
      EXPECT_THROW(capability.require(operation_e::restore_state), capability_error_t);
      EXPECT_TRUE(coordinator.status().fence_active);
      held.reset();
      EXPECT_NO_THROW(capability.require(operation_e::restore_state));
    }).completed());
    EXPECT_FALSE(coordinator.status().fence_active);
    EXPECT_FALSE(other.status().fence_active);
    EXPECT_TRUE(coordinator.run(command_kind_e::rotate, {}).completed());
  }

  TEST(DisplayTopologyCoordinator, ExternalMutationIsolationPreservesOtherOwnersAndExistingFences) {
    fake_clock_t clock;
    coordinator_t coordinator { make_options(clock) };
    coordinator_t other { make_options(clock) };
    ASSERT_TRUE(start_ready(coordinator).completed());
    ASSERT_TRUE(start_ready(other).completed());
    std::shared_ptr<const admission_fence_t> teardown;
    std::shared_ptr<const probe_fence_t> probe;
    EXPECT_TRUE(coordinator.run(command_kind_e::reconfigure, [&](const auto &capability, const auto &) {
      teardown = coordinator.hold_fence();
      probe = coordinator.hold_probe_fence();
      EXPECT_NO_THROW(capability.require(operation_e::restore_state));
      auto external = coordinator.hold_external_mutation(capability);
      EXPECT_TRUE(other.run(command_kind_e::reconfigure, [](const auto &other_capability, const auto &) {
        EXPECT_NO_THROW(other_capability.require(operation_e::restore_state));
      }).completed());
      EXPECT_THROW(capability.require(operation_e::restore_state), capability_error_t);
      external.reset();
      EXPECT_NO_THROW(capability.require(operation_e::restore_state));
    }).completed());
    teardown.reset();
    EXPECT_TRUE(coordinator.status().fence_active);
    probe.reset();
    EXPECT_FALSE(coordinator.status().fence_active);
    EXPECT_TRUE(coordinator.run(command_kind_e::rotate, {}).completed());
  }

  TEST(DisplayTopologyCoordinator, ExternalMutationHolderOutlivesStoppedOwnerWithoutRunningCleanup) {
    fake_clock_t clock;
    auto coordinator = std::make_unique<coordinator_t>(make_options(clock));
    ASSERT_TRUE(start_ready(*coordinator).completed());
    external_hold_t held;
    ASSERT_TRUE(coordinator->run(command_kind_e::reconfigure, [&](const auto &capability, const auto &) {
      held = coordinator->hold_external_mutation(capability);
    }).completed());
    std::atomic_int mutations {};
    const auto cleanup = coordinator->post_tracked(command_kind_e::stream_restore, counting(mutations));
    auto stopped = std::async(std::launch::async, [&]() { coordinator->stop(); });
    const auto before_release = stopped.wait_for(real_timeout);
    if (before_release != std::future_status::ready) held.reset();
    stopped.get();
    EXPECT_EQ(before_release, std::future_status::ready);
    EXPECT_EQ(cleanup.completion.get().outcome, outcome_e::stopped);
    coordinator.reset();
    std::thread late_monitor { [guard = std::move(held)]() mutable { guard.reset(); } };
    late_monitor.join();
    EXPECT_EQ(mutations.load(), 0);
  }

  TEST(DisplayTopologyCoordinator, ExternalMutationCannotStartAfterDrainingBegan) {
    fake_clock_t clock;
    coordinator_t coordinator { make_options(clock) };
    ASSERT_TRUE(start_ready(coordinator).completed());
    gate_t worker;
    auto running = std::async(std::launch::async, [&]() {
      return coordinator.run(command_kind_e::reconfigure, [&](const auto &capability, const auto &) {
        worker.hook()();
        EXPECT_THROW(coordinator.hold_external_mutation(capability), std::logic_error);
        EXPECT_NO_THROW(capability.require(operation_e::restore_state));
      });
    });
    const finally_t release { [&]() { worker.release(); } };
    ASSERT_TRUE(worker.wait_started());
    coordinator.begin_draining();
    worker.release();
    EXPECT_TRUE(running.get().completed());
    EXPECT_FALSE(coordinator.status().fence_active);
  }

  auto
  discharging(event_log_t &log, std::string name, bool result = true) {
    return [&log, name = std::move(name), result](const mutation_capability_t &capability, const command_context_t &) {
      EXPECT_EQ(capability.kind(), command_kind_e::launch_failed_restore);
      EXPECT_NO_THROW(capability.require(operation_e::vdd_destroy));
      log.add(name);
      return result;
    };
  }

  TEST(DisplayTopologyCoordinator, ExternalCleanupRunsAheadOfPrequeuedCleanupAndLiftsItsFenceOnlyWhenDischarged) {
    fake_clock_t clock;
    event_log_t log;
    coordinator_t coordinator { make_options(clock) };
    ASSERT_TRUE(start_ready(coordinator).completed());
    std::shared_ptr<external_cleanup_t> cleanup;
    std::shared_ptr<void> supervisor;
    ASSERT_TRUE(coordinator.run(command_kind_e::reconfigure, [&](const auto &capability, const auto &) {
      cleanup = coordinator.begin_external_cleanup(capability);
      supervisor = cleanup->exclusion();
      EXPECT_NE(supervisor, nullptr);
      EXPECT_THROW(capability.require(operation_e::restore_state), capability_error_t);
    }).completed());
    ASSERT_NE(cleanup, nullptr);
    EXPECT_TRUE(coordinator.status().fence_active);
    EXPECT_EQ(coordinator.admit_launch({ 460, "actor alive" }).reason, reject_reason_e::fence_active);
    const auto prequeued = coordinator.post_tracked(command_kind_e::stream_restore, logging(log, "prequeued"));
    ASSERT_EQ(prequeued.reason, reject_reason_e::none);
    EXPECT_EQ(prequeued.completion.wait_for(50ms), std::future_status::timeout);

    EXPECT_TRUE(cleanup->queue_first(discharging(log, "actor-cleanup")));
    EXPECT_EQ(cleanup->exclusion(), nullptr);
    // The supervisor's copy still holds dispatch.
    EXPECT_EQ(prequeued.completion.wait_for(50ms), std::future_status::timeout);
    EXPECT_TRUE(log.events().empty());
    EXPECT_FALSE(cleanup->discharged());
    supervisor.reset();
    ASSERT_EQ(prequeued.completion.wait_for(real_timeout), std::future_status::ready);
    EXPECT_TRUE(prequeued.completion.get().completed());
    EXPECT_EQ(log.events(), (std::vector<std::string> { "actor-cleanup", "prequeued" }));
    EXPECT_TRUE(cleanup->discharged());
    EXPECT_FALSE(coordinator.status().fence_active);
    EXPECT_TRUE(coordinator.admit_launch({ 461, "discharged" }));
  }

  TEST(DisplayTopologyCoordinator, UndischargedExternalCleanupKeepsAdmissionFencedButLetsCleanupRun) {
    for (const bool throws : { false, true }) {
      SCOPED_TRACE(throws);
      fake_clock_t clock;
      event_log_t log;
      coordinator_t coordinator { make_options(clock) };
      ASSERT_TRUE(start_ready(coordinator).completed());
      std::shared_ptr<external_cleanup_t> cleanup;
      ASSERT_TRUE(coordinator.run(command_kind_e::reconfigure, [&](const auto &capability, const auto &) {
        cleanup = coordinator.begin_external_cleanup(capability);
      }).completed());
      EXPECT_TRUE(cleanup->queue_first([&](const auto &, const auto &) -> bool {
        log.add("actor-cleanup");
        if (throws) throw std::runtime_error { "cleanup record could not be completed" };
        return false;
      }));
      const auto later = coordinator.post_tracked(command_kind_e::stream_restore, logging(log, "later"));
      ASSERT_EQ(later.completion.wait_for(real_timeout), std::future_status::ready);
      EXPECT_TRUE(later.completion.get().completed());
      EXPECT_EQ(log.events(), (std::vector<std::string> { "actor-cleanup", "later" }));
      EXPECT_FALSE(cleanup->discharged());
      cleanup.reset();
      EXPECT_TRUE(coordinator.status().fence_active);
      EXPECT_EQ(coordinator.admit_launch({ 462, "unknown cleanup" }).reason, reject_reason_e::fence_active);
      EXPECT_EQ(coordinator.run(command_kind_e::rotate, {}).reason, reject_reason_e::fence_active);
      EXPECT_TRUE(coordinator.run(command_kind_e::shutdown_restore, logging(log, "shutdown")).completed());
    }
  }

  TEST(DisplayTopologyCoordinator, DroppedExternalCleanupHandleNeverReleasesItsExclusion) {
    fake_clock_t clock;
    coordinator_t coordinator { make_options(clock) };
    ASSERT_TRUE(start_ready(coordinator).completed());
    std::weak_ptr<external_cleanup_t> weak;
    ASSERT_TRUE(coordinator.run(command_kind_e::reconfigure, [&](const auto &capability, const auto &) {
      weak = coordinator.begin_external_cleanup(capability);
    }).completed());
    EXPECT_FALSE(weak.expired());
    EXPECT_TRUE(coordinator.status().fence_active);
    std::atomic_int mutations {};
    const auto queued = coordinator.post_tracked(command_kind_e::stream_restore, counting(mutations));
    ASSERT_EQ(queued.reason, reject_reason_e::none);
    EXPECT_EQ(queued.completion.wait_for(50ms), std::future_status::timeout);
    coordinator.stop();
    EXPECT_EQ(queued.completion.get().outcome, outcome_e::stopped);
    EXPECT_EQ(mutations.load(), 0);
    EXPECT_TRUE(weak.expired());
  }

  TEST(DisplayTopologyCoordinator, ExternalCleanupQueuesOnceAndNeverAfterStopOrDestruction) {
    fake_clock_t clock;
    event_log_t log;
    {
      coordinator_t coordinator { make_options(clock) };
      ASSERT_TRUE(start_ready(coordinator).completed());
      std::shared_ptr<external_cleanup_t> cleanup;
      ASSERT_TRUE(coordinator.run(command_kind_e::reconfigure, [&](const auto &capability, const auto &) {
        cleanup = coordinator.begin_external_cleanup(capability);
      }).completed());
      EXPECT_FALSE(cleanup->queue_first({}));
      EXPECT_NE(cleanup->exclusion(), nullptr);
      EXPECT_TRUE(cleanup->queue_first(discharging(log, "first")));
      EXPECT_FALSE(cleanup->queue_first(discharging(log, "second")));
      const auto flushed = coordinator.post_tracked(command_kind_e::stream_restore, {});
      ASSERT_EQ(flushed.completion.wait_for(real_timeout), std::future_status::ready);
      EXPECT_TRUE(flushed.completion.get().completed());
      EXPECT_EQ(log.events(), (std::vector<std::string> { "first" }));
      EXPECT_TRUE(cleanup->discharged());
      // Once queued, the coordinator keeps no reference to the handle.
      std::weak_ptr<external_cleanup_t> weak = cleanup;
      cleanup.reset();
      EXPECT_TRUE(weak.expired());
    }
    auto coordinator = std::make_unique<coordinator_t>(make_options(clock));
    ASSERT_TRUE(start_ready(*coordinator).completed());
    std::shared_ptr<external_cleanup_t> cleanup;
    ASSERT_TRUE(coordinator->run(command_kind_e::reconfigure, [&](const auto &capability, const auto &) {
      cleanup = coordinator->begin_external_cleanup(capability);
    }).completed());
    auto supervisor = cleanup->exclusion();
    coordinator->stop();
    EXPECT_FALSE(cleanup->queue_first(discharging(log, "after-stop")));
    EXPECT_NE(cleanup->exclusion(), nullptr);
    coordinator.reset();
    EXPECT_FALSE(cleanup->queue_first(discharging(log, "after-destruction")));
    EXPECT_NE(cleanup->exclusion(), nullptr);
    std::thread late { [cleanup = std::move(cleanup), supervisor = std::move(supervisor)]() mutable {
      supervisor.reset();
      cleanup.reset();
    } };
    late.join();
    EXPECT_EQ(log.events(), (std::vector<std::string> { "first" }));
  }

  TEST(DisplayTopologyCoordinator, ExternalCleanupRequiresWorkerAuthorityAndOneOutstandingObligation) {
    fake_clock_t clock;
    event_log_t log;
    coordinator_t coordinator { make_options(clock) };
    coordinator_t other { make_options(clock) };
    ASSERT_TRUE(start_ready(coordinator).completed());
    ASSERT_TRUE(start_ready(other).completed());
    for (const auto kind : { command_kind_e::rotate, command_kind_e::stream_restore }) {
      EXPECT_TRUE(coordinator.run(kind, [&](const auto &capability, const auto &) {
        EXPECT_THROW(coordinator.begin_external_cleanup(capability), capability_error_t);
      }).completed());
      EXPECT_FALSE(coordinator.status().fence_active);
    }
    ASSERT_TRUE(coordinator.run(command_kind_e::reconfigure, [&](const auto &capability, const auto &) {
      std::thread outsider { [&]() {
        EXPECT_THROW(coordinator.begin_external_cleanup(capability), capability_error_t);
      } };
      outsider.join();
      EXPECT_THROW(other.begin_external_cleanup(capability), std::logic_error);
      EXPECT_FALSE(coordinator.status().fence_active);
      auto first = coordinator.begin_external_cleanup(capability);
      EXPECT_THROW(coordinator.begin_external_cleanup(capability), capability_error_t);
      // Queued from inside the body: it runs after the body returns.
      EXPECT_TRUE(first->queue_first(discharging(log, "actor-cleanup")));
      EXPECT_NO_THROW(capability.require(operation_e::restore_state));
      EXPECT_THROW(coordinator.begin_external_cleanup(capability), std::logic_error);
      log.add("body-returning");
    }).completed());
    EXPECT_TRUE(coordinator.run(command_kind_e::reconfigure, [&](const auto &capability, const auto &) {
      auto next = coordinator.begin_external_cleanup(capability);
      EXPECT_TRUE(next->queue_first(discharging(log, "next-cleanup")));
    }).completed());
    EXPECT_TRUE(coordinator.run(command_kind_e::stream_restore, {}).completed());
    EXPECT_EQ(log.events(), (std::vector<std::string> { "body-returning", "actor-cleanup", "next-cleanup" }));
    EXPECT_FALSE(coordinator.status().fence_active);
    EXPECT_FALSE(other.status().fence_active);

    gate_t worker;
    auto running = std::async(std::launch::async, [&]() {
      return coordinator.run(command_kind_e::reconfigure, [&](const auto &capability, const auto &) {
        worker.hook()();
        EXPECT_THROW(coordinator.begin_external_cleanup(capability), std::logic_error);
      });
    });
    const finally_t release { [&]() { worker.release(); } };
    ASSERT_TRUE(worker.wait_started());
    coordinator.begin_draining();
    worker.release();
    EXPECT_TRUE(running.get().completed());
    EXPECT_FALSE(coordinator.status().fence_active);
  }

  TEST(DisplayTopologyCoordinator, ExternalCleanupQueuedWhileDrainingRunsAheadOfShutdownCleanup) {
    fake_clock_t clock;
    event_log_t log;
    coordinator_t coordinator { make_options(clock) };
    ASSERT_TRUE(start_ready(coordinator).completed());
    std::shared_ptr<external_cleanup_t> cleanup;
    ASSERT_TRUE(coordinator.run(command_kind_e::reconfigure, [&](const auto &capability, const auto &) {
      cleanup = coordinator.begin_external_cleanup(capability);
    }).completed());
    coordinator.begin_draining();
    const auto shutdown = coordinator.post_tracked(command_kind_e::shutdown_restore, logging(log, "shutdown"));
    ASSERT_EQ(shutdown.reason, reject_reason_e::none);
    EXPECT_EQ(shutdown.completion.wait_for(50ms), std::future_status::timeout);
    EXPECT_TRUE(cleanup->queue_first(discharging(log, "actor-cleanup")));
    ASSERT_EQ(shutdown.completion.wait_for(real_timeout), std::future_status::ready);
    EXPECT_TRUE(shutdown.completion.get().completed());
    EXPECT_EQ(log.events(), (std::vector<std::string> { "actor-cleanup", "shutdown" }));
    EXPECT_TRUE(cleanup->discharged());
  }

  TEST(DisplayTopologyCoordinator, ExternalRecoveryUsesOnlyStartupAuthorityAndBlocksCurrentRawCleanup) {
    for (const bool managed : { false, true }) {
      fake_clock_t clock;
      coordinator_t coordinator { make_options(clock, managed) };
      const auto startup = coordinator.start([&](const auto &capability, const auto &) {
        EXPECT_NO_THROW(capability.require(operation_e::native_recover));
        EXPECT_THROW(capability.require(operation_e::adapter_restart), capability_error_t);
        auto held = coordinator.hold_external_recovery(capability);
        auto monitor = held;
        for (const auto operation : all_operations) {
          if (kind_allows(capability.kind(), operation, managed)) {
            SCOPED_TRACE(to_string(operation));
            EXPECT_THROW(capability.require(operation), capability_error_t);
          }
        }
        held.reset();
        EXPECT_TRUE(coordinator.status().fence_active);
        EXPECT_THROW(capability.require(operation_e::restore_state), capability_error_t);
        monitor.reset();
        EXPECT_NO_THROW(capability.require(operation_e::native_recover));
        EXPECT_NO_THROW(capability.require(operation_e::restore_state));
        EXPECT_THROW(capability.require(operation_e::adapter_restart), capability_error_t);
      });
      EXPECT_TRUE(startup.get().completed());
      EXPECT_FALSE(coordinator.status().fence_active);
    }
  }

  TEST(DisplayTopologyCoordinator, ExternalRecoveryRemainsFencedAfterStartupInspectionThrows) {
    fake_clock_t clock;
    coordinator_t coordinator { make_options(clock) };
    external_hold_t held;
    const auto recovery = coordinator.start([&](const auto &capability, const auto &) {
      held = coordinator.hold_external_recovery(capability);
      throw std::runtime_error { "External process receipt could not be inspected" };
    }).get();
    EXPECT_EQ(recovery.outcome, outcome_e::failed);
    ASSERT_NE(held, nullptr);
    EXPECT_TRUE(coordinator.status().native.in_conflict);
    coordinator.publish_native_health({});
    EXPECT_EQ(coordinator.admit_launch({ 440, "external recovery" }).reason, reject_reason_e::fence_active);
    for (const auto kind : { command_kind_e::reset_persistence, command_kind_e::vdd_create, command_kind_e::vdd_destroy }) {
      EXPECT_EQ(coordinator.post(kind, {}), reject_reason_e::fence_active);
    }
    std::atomic_size_t mutations {};
    std::vector<posted_command_t> cleanup;
    for (const auto kind : expected_cleanup_kinds) {
      cleanup.push_back(coordinator.post_tracked(kind, [&](const auto &capability, const auto &) {
        capability.require(operation_e::vdd_destroy);
        ++mutations;
      }));
      EXPECT_EQ(cleanup.back().reason, reject_reason_e::none);
    }
    auto monitor = held;
    held.reset();
    for (const auto &command : cleanup) {
      EXPECT_EQ(command.completion.wait_for(50ms), std::future_status::timeout);
    }
    EXPECT_EQ(mutations.load(), 0u);
    monitor.reset();
    for (const auto &command : cleanup) EXPECT_TRUE(command.completion.get().completed());
    EXPECT_EQ(mutations.load(), expected_cleanup_kinds.size());
    EXPECT_TRUE(coordinator.admit_launch({ 441, "settled" }));
  }

  TEST(DisplayTopologyCoordinator, ExternalRecoveryRejectsWrongAuthorityAndDuplicateAcquisition) {
    fake_clock_t clock;
    coordinator_t coordinator { make_options(clock) };
    coordinator_t other { make_options(clock) };
    ASSERT_TRUE(start_ready(other).completed());
    const auto startup = coordinator.start([&](const auto &capability, const auto &) {
      std::thread outsider { [&]() {
        EXPECT_THROW(coordinator.hold_external_recovery(capability), capability_error_t);
      } };
      outsider.join();
      EXPECT_THROW(other.hold_external_recovery(capability), std::logic_error);
      EXPECT_FALSE(coordinator.status().fence_active);
      EXPECT_FALSE(other.status().fence_active);
      auto held = coordinator.hold_external_recovery(capability);
      EXPECT_THROW(coordinator.hold_external_recovery(capability), capability_error_t);
      EXPECT_THROW(capability.require(operation_e::restore_state), capability_error_t);
      EXPECT_TRUE(coordinator.status().fence_active);
      EXPECT_TRUE(other.run(command_kind_e::reconfigure, [](const auto &other_capability, const auto &) {
        EXPECT_NO_THROW(other_capability.require(operation_e::restore_state));
      }).completed());
      held.reset();
      EXPECT_NO_THROW(capability.require(operation_e::native_recover));
      other.stop();
      EXPECT_THROW(other.hold_external_recovery(capability), std::logic_error);
      EXPECT_FALSE(other.status().fence_active);
    });
    EXPECT_TRUE(startup.get().completed());
    for (const auto kind : { command_kind_e::reconfigure, command_kind_e::stream_restore, command_kind_e::rotate }) {
      EXPECT_TRUE(coordinator.run(kind, [&](const auto &capability, const auto &) {
        EXPECT_THROW(coordinator.hold_external_recovery(capability), capability_error_t);
      }).completed());
      EXPECT_FALSE(coordinator.status().fence_active);
    }
  }

  TEST(DisplayTopologyCoordinator, ExternalRecoveryAdoptsAnExistingActorWhileDraining) {
    fake_clock_t clock;
    std::atomic_int mutations {};
    external_hold_t held;
    coordinator_t coordinator { make_options(clock) };
    gate_t worker;
    const auto startup = coordinator.start([&](const auto &capability, const auto &) {
      worker.hook()();
      held = coordinator.hold_external_recovery(capability);
      EXPECT_THROW(capability.require(operation_e::restore_state), capability_error_t);
    });
    const finally_t release { [&]() { worker.release(); } };
    ASSERT_TRUE(worker.wait_started());
    coordinator.begin_draining();
    const auto cleanup = coordinator.post_tracked(command_kind_e::shutdown_restore, counting(mutations));
    ASSERT_EQ(cleanup.reason, reject_reason_e::none);
    worker.release();
    ASSERT_TRUE(startup.get().completed());
    EXPECT_EQ(coordinator.status().phase, coordinator_phase_e::draining);
    EXPECT_EQ(cleanup.completion.wait_for(50ms), std::future_status::timeout);
    EXPECT_EQ(mutations.load(), 0);
    held.reset();
    EXPECT_TRUE(cleanup.completion.get().completed());
    EXPECT_EQ(mutations.load(), 1);
  }

  TEST(DisplayTopologyCoordinator, ExternalRecoverySurvivesStopBeforeOrAfterQueueMovement) {
    for (const bool acquire_before_stop : { false, true }) {
      SCOPED_TRACE(acquire_before_stop);
      fake_clock_t clock;
      std::atomic_int mutations {};
      external_hold_t held;
      auto coordinator = std::make_unique<coordinator_t>(make_options(clock));
      gate_t worker;
      const auto startup = coordinator->start([&](const auto &capability, const auto &) {
        if (acquire_before_stop) held = coordinator->hold_external_recovery(capability);
        worker.hook()();
        if (!acquire_before_stop) held = coordinator->hold_external_recovery(capability);
        EXPECT_THROW(capability.require(operation_e::restore_state), capability_error_t);
      });
      std::future<void> stopping;
      const finally_t release { [&]() { worker.release(); } };
      ASSERT_TRUE(worker.wait_started());
      coordinator->begin_draining();
      const auto cleanup = coordinator->post_tracked(command_kind_e::shutdown_restore, counting(mutations));
      ASSERT_EQ(cleanup.reason, reject_reason_e::none);
      stopping = std::async(std::launch::async, [&]() { coordinator->stop(); });
      ASSERT_TRUE(wait_until([&]() { return coordinator->status().phase == coordinator_phase_e::stopped; }));
      worker.release();
      const auto before_release = stopping.wait_for(real_timeout);
      if (before_release != std::future_status::ready) held.reset();
      stopping.get();
      EXPECT_EQ(before_release, std::future_status::ready);
      const auto result = startup.get();
      EXPECT_TRUE(result.outcome == outcome_e::completed || result.outcome == outcome_e::completed_late);
      ASSERT_NE(held, nullptr);
      const auto cleanup_result = cleanup.completion.get();
      EXPECT_TRUE(cleanup_result.outcome == outcome_e::stopped ||
                  (cleanup_result.outcome == outcome_e::rejected && cleanup_result.reason == reject_reason_e::stopped));
      EXPECT_TRUE(coordinator->status().fence_active);
      EXPECT_EQ(mutations.load(), 0);
      coordinator.reset();
      std::thread late_monitor { [guard = std::move(held)]() mutable { guard.reset(); } };
      late_monitor.join();
    }
  }

  TEST(DisplayTopologyCoordinator, NormalExternalMutationCannotStartAfterStopMovesQueue) {
    fake_clock_t clock;
    coordinator_t coordinator { make_options(clock) };
    ASSERT_TRUE(start_ready(coordinator).completed());
    gate_t worker;
    const auto running = coordinator.post_tracked(command_kind_e::reconfigure, [&](const auto &capability, const auto &) {
      worker.hook()();
      EXPECT_THROW(coordinator.hold_external_mutation(capability), std::logic_error);
      EXPECT_NO_THROW(capability.require(operation_e::restore_state));
    });
    std::future<void> stopping;
    const finally_t release { [&]() { worker.release(); } };
    ASSERT_TRUE(worker.wait_started());
    stopping = std::async(std::launch::async, [&]() { coordinator.stop(); });
    ASSERT_TRUE(wait_until([&]() { return coordinator.status().phase == coordinator_phase_e::stopped; }));
    worker.release();
    stopping.get();
    EXPECT_TRUE(running.completion.get().body_started());
    EXPECT_FALSE(coordinator.status().fence_active);
  }

  TEST(DisplayTopologyCoordinator, SuspendedStartupResumesBeforePrequeuedShutdownCleanup) {
    for (const bool managed : { false, true }) {
      SCOPED_TRACE(managed);
      fake_clock_t clock;
      event_log_t log;
      coordinator_t coordinator { make_options(clock, managed) };
      std::shared_ptr<external_recovery_t> actor;
      gate_t worker;
      const auto startup = coordinator.start([&](const auto &capability, const auto &) {
        actor = coordinator.suspend_startup_for_external_recovery(capability, [&](const auto &resumed, const auto &) {
          EXPECT_EQ(resumed.kind(), command_kind_e::startup_recovery);
          EXPECT_NO_THROW(resumed.require(operation_e::native_recover));
          EXPECT_NO_THROW(resumed.require(operation_e::restore_state));
          log.add("continuation");
        });
        worker.hook()();
        EXPECT_THROW(capability.require(operation_e::restore_state), capability_error_t);
      });
      const finally_t release { [&]() { worker.release(); } };
      ASSERT_TRUE(worker.wait_started());
      coordinator.begin_draining();
      const auto cleanup = coordinator.post_tracked(command_kind_e::shutdown_restore, logging(log, "cleanup"));
      ASSERT_EQ(cleanup.reason, reject_reason_e::none);
      worker.release();
      EXPECT_EQ(startup.wait_for(50ms), std::future_status::timeout);
      EXPECT_EQ(cleanup.completion.wait_for(50ms), std::future_status::timeout);
      EXPECT_TRUE(coordinator.status().external_recovery_pending);
      EXPECT_TRUE(log.events().empty());
      ASSERT_NE(actor, nullptr);
      std::thread monitor { [actor]() { actor->settle(); } };
      monitor.join();
      ASSERT_EQ(startup.wait_for(real_timeout), std::future_status::ready);
      EXPECT_TRUE(startup.get().completed());
      EXPECT_TRUE(cleanup.completion.get().completed());
      EXPECT_EQ(log.events(), (std::vector<std::string> { "continuation", "cleanup" }));
      EXPECT_FALSE(coordinator.status().external_recovery_pending);
      EXPECT_FALSE(coordinator.status().fence_active);
    }
  }

  TEST(DisplayTopologyCoordinator, SuspendedStartupStaysFencedWhenInspectionThrows) {
    fake_clock_t clock;
    event_log_t log;
    std::shared_ptr<external_recovery_t> monitor;
    coordinator_t coordinator { make_options(clock) };
    const auto startup = coordinator.start([&](const auto &capability, const auto &) {
      monitor = coordinator.suspend_startup_for_external_recovery(capability, logging(log, "continuation"));
      throw std::runtime_error { "External process receipt could not be inspected" };
    });
    EXPECT_EQ(startup.wait_for(50ms), std::future_status::timeout);
    auto status = coordinator.status();
    EXPECT_EQ(status.phase, coordinator_phase_e::recovering);
    EXPECT_TRUE(status.fence_active);
    EXPECT_TRUE(status.external_recovery_pending);
    EXPECT_EQ(coordinator.admit_launch({ 450, "unresolved actor" }).reason, reject_reason_e::recovering);
    for (const auto kind : { command_kind_e::reset_persistence, command_kind_e::vdd_create, command_kind_e::vdd_destroy,
           command_kind_e::shutdown_restore }) {
      SCOPED_TRACE(to_string(kind));
      EXPECT_EQ(coordinator.post(kind, logging(log, "rejected")), reject_reason_e::recovering);
    }
    ASSERT_NE(monitor, nullptr);
    monitor->settle();
    const auto result = startup.get();
    EXPECT_EQ(result.outcome, outcome_e::failed);
    ASSERT_TRUE(result.exception);
    EXPECT_THROW(std::rethrow_exception(result.exception), std::runtime_error);
    EXPECT_EQ(log.events(), (std::vector<std::string> { "continuation" }));
    status = coordinator.status();
    EXPECT_EQ(status.phase, coordinator_phase_e::running);
    EXPECT_TRUE(status.native.in_conflict);
    EXPECT_FALSE(status.fence_active);
    EXPECT_FALSE(status.external_recovery_pending);
    // Native-only failure still permits the approved legacy path once the actor settled.
    EXPECT_TRUE(coordinator.admit_launch({ 451, "settled" }));
  }

  TEST(DisplayTopologyCoordinator, SettlementDuringSuspendedBodyWaitsForItsReturn) {
    fake_clock_t clock;
    event_log_t log;
    coordinator_t coordinator { make_options(clock) };
    std::shared_ptr<external_recovery_t> actor;
    const auto startup = coordinator.start([&](const auto &capability, const auto &) {
      actor = coordinator.suspend_startup_for_external_recovery(capability, logging(log, "continuation"));
      std::thread monitor { [settled = actor]() { settled->settle(); } };
      monitor.join();
      EXPECT_THROW(capability.require(operation_e::restore_state), capability_error_t);
      EXPECT_TRUE(coordinator.status().fence_active);
      log.add("body-returning");
    });
    EXPECT_TRUE(startup.get().completed());
    EXPECT_EQ(log.events(), (std::vector<std::string> { "body-returning", "continuation" }));
    actor->settle();
    EXPECT_TRUE(coordinator.run(command_kind_e::reconfigure, logging(log, "later")).completed());
    EXPECT_EQ(log.events(), (std::vector<std::string> { "body-returning", "continuation", "later" }));
    EXPECT_FALSE(coordinator.status().external_recovery_pending);
  }

  TEST(DisplayTopologyCoordinator, StopWhileSuspendedRunsNeitherContinuationNorCleanup) {
    for (const bool body_running_at_stop : { false, true }) {
      SCOPED_TRACE(body_running_at_stop);
      fake_clock_t clock;
      std::atomic_int mutations {};
      std::shared_ptr<external_recovery_t> actor;
      std::promise<void> suspended;
      auto coordinator = std::make_unique<coordinator_t>(make_options(clock));
      gate_t worker;
      const auto startup = coordinator->start([&](const auto &capability, const auto &) {
        actor = coordinator->suspend_startup_for_external_recovery(capability, counting(mutations));
        suspended.set_value();
        if (body_running_at_stop) worker.hook()();
      });
      const finally_t release { [&]() { worker.release(); } };
      ASSERT_EQ(suspended.get_future().wait_for(real_timeout), std::future_status::ready);
      if (body_running_at_stop) {
        ASSERT_TRUE(worker.wait_started());
      }
      coordinator->begin_draining();
      const auto cleanup = coordinator->post_tracked(command_kind_e::shutdown_restore, counting(mutations));
      ASSERT_EQ(cleanup.reason, reject_reason_e::none);
      auto stopping = std::async(std::launch::async, [&]() { coordinator->stop(); });
      ASSERT_TRUE(wait_until([&]() { return coordinator->status().phase == coordinator_phase_e::stopped; }));
      actor->settle();
      worker.release();
      ASSERT_EQ(stopping.wait_for(real_timeout), std::future_status::ready);
      stopping.get();
      EXPECT_EQ(startup.get().outcome, outcome_e::stopped);
      const auto cleanup_result = cleanup.completion.get();
      EXPECT_TRUE(cleanup_result.outcome == outcome_e::stopped ||
                  (cleanup_result.outcome == outcome_e::rejected && cleanup_result.reason == reject_reason_e::stopped));
      EXPECT_EQ(mutations.load(), 0);
      EXPECT_FALSE(coordinator->status().fence_active);
      EXPECT_FALSE(coordinator->status().external_recovery_pending);
      coordinator.reset();
      actor->settle();
      EXPECT_EQ(mutations.load(), 0);
    }
  }

  TEST(DisplayTopologyCoordinator, StartupSuspensionRequiresOwnStartupAuthorityOnce) {
    fake_clock_t clock;
    coordinator_t coordinator { make_options(clock) };
    coordinator_t other { make_options(clock) };
    ASSERT_TRUE(start_ready(other).completed());
    std::atomic_int continuations {};
    const auto startup = coordinator.start([&](const auto &capability, const auto &) {
      std::thread outsider { [&]() {
        EXPECT_THROW(coordinator.suspend_startup_for_external_recovery(capability, counting(continuations)), capability_error_t);
      } };
      outsider.join();
      EXPECT_THROW(other.suspend_startup_for_external_recovery(capability, counting(continuations)), std::logic_error);
      EXPECT_THROW(coordinator.suspend_startup_for_external_recovery(capability, {}), std::invalid_argument);
      EXPECT_FALSE(coordinator.status().fence_active);
      EXPECT_FALSE(coordinator.status().external_recovery_pending);
      auto actor = coordinator.suspend_startup_for_external_recovery(capability, [&](const auto &resumed, const auto &) {
        ++continuations;
        EXPECT_THROW(coordinator.suspend_startup_for_external_recovery(resumed, counting(continuations)), std::logic_error);
      });
      EXPECT_THROW(coordinator.suspend_startup_for_external_recovery(capability, counting(continuations)), capability_error_t);
      actor->settle();
    });
    ASSERT_EQ(startup.wait_for(real_timeout), std::future_status::ready);
    EXPECT_TRUE(startup.get().completed());
    EXPECT_EQ(continuations.load(), 1);
    for (const auto kind : { command_kind_e::reconfigure, command_kind_e::stream_restore }) {
      EXPECT_TRUE(coordinator.run(kind, [&](const auto &capability, const auto &) {
        EXPECT_THROW(coordinator.suspend_startup_for_external_recovery(capability, counting(continuations)), capability_error_t);
      }).completed());
    }
    EXPECT_FALSE(other.status().fence_active);
    EXPECT_FALSE(other.status().external_recovery_pending);
  }

  TEST(DisplayTopologyCoordinator, SuspendedStartupMergesContinuationFailure) {
    for (const bool body_throws : { false, true }) {
      SCOPED_TRACE(body_throws);
      fake_clock_t clock;
      coordinator_t coordinator { make_options(clock) };
      std::shared_ptr<external_recovery_t> actor;
      std::promise<void> suspended;
      const auto startup = coordinator.start([&](const auto &capability, const auto &) {
        actor = coordinator.suspend_startup_for_external_recovery(capability, [](const auto &, const auto &) {
          throw std::logic_error { "continuation" };
        });
        suspended.set_value();
        if (body_throws) {
          throw std::runtime_error { "original" };
        }
      });
      ASSERT_EQ(suspended.get_future().wait_for(real_timeout), std::future_status::ready);
      actor->settle();
      const auto result = startup.get();
      EXPECT_EQ(result.outcome, outcome_e::failed);
      ASSERT_TRUE(result.exception);
      if (body_throws) {
        EXPECT_THROW(std::rethrow_exception(result.exception), std::runtime_error);
      }
      else {
        EXPECT_THROW(std::rethrow_exception(result.exception), std::logic_error);
      }
      const auto status = coordinator.status();
      EXPECT_EQ(status.phase, coordinator_phase_e::running);
      EXPECT_TRUE(status.native.in_conflict);
      EXPECT_FALSE(status.external_recovery_pending);
    }
  }

  TEST(DisplayTopologyCoordinator, ConcurrentSettlementResumesStartupOnce) {
    fake_clock_t clock;
    coordinator_t coordinator { make_options(clock) };
    std::atomic_int continuations {};
    std::shared_ptr<external_recovery_t> actor;
    std::promise<void> suspended;
    const auto startup = coordinator.start([&](const auto &capability, const auto &) {
      actor = coordinator.suspend_startup_for_external_recovery(capability, counting(continuations));
      suspended.set_value();
    });
    ASSERT_EQ(suspended.get_future().wait_for(real_timeout), std::future_status::ready);
    std::vector<std::thread> monitors;
    for (int i = 0; i < 8; ++i) {
      monitors.emplace_back([actor]() { actor->settle(); });
    }
    for (auto &monitor : monitors) {
      monitor.join();
    }
    EXPECT_TRUE(startup.get().completed());
    actor->settle();
    EXPECT_TRUE(coordinator.run(command_kind_e::reconfigure, [](const auto &, const auto &) {}).completed());
    EXPECT_EQ(continuations.load(), 1);
    EXPECT_FALSE(coordinator.status().fence_active);
  }

  TEST(DisplayTopologyCoordinator, SettlementRacingDestructionIsSafe) {
    for (int i = 0; i < 100; ++i) {
      fake_clock_t clock;
      std::atomic_int continuations {};
      auto coordinator = std::make_unique<coordinator_t>(make_options(clock));
      std::shared_ptr<external_recovery_t> actor;
      std::promise<void> suspended;
      const auto startup = coordinator->start([&](const auto &capability, const auto &) {
        actor = coordinator->suspend_startup_for_external_recovery(capability, counting(continuations));
        suspended.set_value();
      });
      ASSERT_EQ(suspended.get_future().wait_for(real_timeout), std::future_status::ready);
      std::atomic_bool go {};
      std::thread monitor { [&go, actor]() {
        while (!go.load()) {}
        for (int n = 0; n < 50; ++n) {
          actor->settle();
        }
      } };
      go.store(true);
      coordinator.reset();
      monitor.join();
      actor->settle();
      EXPECT_LE(continuations.load(), 1);
      EXPECT_EQ(startup.wait_for(0ms), std::future_status::ready);
    }
  }

  TEST(DisplayTopologyCoordinator, StaleRecoveryHandleCannotSettleReplacementCoordinator) {
    fake_clock_t clock;
    std::atomic_int stale_continuations {};
    std::atomic_int continuations {};
    std::optional<coordinator_t> coordinator;
    auto suspend = [&](std::atomic_int &counter) {
      auto actor = std::make_shared<std::shared_ptr<external_recovery_t>>();
      auto suspended = std::make_shared<std::promise<void>>();
      auto ready = suspended->get_future();
      auto *owner = &*coordinator;
      auto startup = owner->start([owner, actor, suspended, &counter](const auto &capability, const auto &) {
        *actor = owner->suspend_startup_for_external_recovery(capability, counting(counter));
        suspended->set_value();
      });
      EXPECT_EQ(ready.wait_for(real_timeout), std::future_status::ready);
      return std::pair { *actor, startup };
    };
    coordinator.emplace(make_options(clock));
    const auto *first = &*coordinator;
    const auto [stale, stale_startup] = suspend(stale_continuations);
    coordinator.reset();
    EXPECT_EQ(stale_startup.get().outcome, outcome_e::stopped);
    // Same storage: a dangling owner pointer would now resume the replacement's startup.
    coordinator.emplace(make_options(clock));
    ASSERT_EQ(&*coordinator, first);
    const auto [actor, startup] = suspend(continuations);
    stale->settle();
    EXPECT_EQ(startup.wait_for(50ms), std::future_status::timeout);
    EXPECT_TRUE(coordinator->status().external_recovery_pending);
    EXPECT_EQ(continuations.load(), 0);
    actor->settle();
    ASSERT_EQ(startup.wait_for(real_timeout), std::future_status::ready);
    EXPECT_TRUE(startup.get().completed());
    EXPECT_EQ(continuations.load(), 1);
    EXPECT_EQ(stale_continuations.load(), 0);
  }

  TEST(DisplayTopologyCoordinator, StopRejectsReleasedButUnstartedContinuationAndCleanup) {
    fake_clock_t clock;
    std::atomic_int mutations {};
    coordinator_t coordinator { make_options(clock) };
    std::shared_ptr<external_recovery_t> actor;
    std::shared_ptr<const probe_fence_t> probe;
    std::promise<void> suspended;
    const auto startup = coordinator.start([&](const auto &capability, const auto &) {
      actor = coordinator.suspend_startup_for_external_recovery(capability, counting(mutations));
      probe = coordinator.hold_probe_fence();
      suspended.set_value();
    });
    ASSERT_EQ(suspended.get_future().wait_for(real_timeout), std::future_status::ready);
    coordinator.begin_draining();
    const auto cleanup = coordinator.post_tracked(command_kind_e::shutdown_restore, counting(mutations));
    ASSERT_EQ(cleanup.reason, reject_reason_e::none);
    const auto before = coordinator.status().submitted_commands;
    // Continuation goes first and the exclusion is released; only the probe holds dispatch.
    actor->settle();
    ASSERT_TRUE(wait_until([&]() { return coordinator.status().submitted_commands == before + 1; }));
    EXPECT_EQ(startup.wait_for(50ms), std::future_status::timeout);
    EXPECT_EQ(mutations.load(), 0);
    auto stopping = std::async(std::launch::async, [&]() { coordinator.stop(); });
    ASSERT_TRUE(wait_until([&]() { return coordinator.status().phase == coordinator_phase_e::stopped; }));
    probe.reset();
    ASSERT_EQ(stopping.wait_for(real_timeout), std::future_status::ready);
    stopping.get();
    EXPECT_EQ(startup.get().outcome, outcome_e::stopped);
    static_cast<void>(cleanup.completion.get());
    EXPECT_EQ(mutations.load(), 0);
  }

  TEST(DisplayTopologyCoordinator, ProbeFenceQueuesEveryCleanupAndRejectsAdmission) {
    fake_clock_t clock;
    coordinator_t coordinator { make_options(clock) };
    ASSERT_TRUE(start_ready(coordinator).completed());
    const auto launch = coordinator.admit_launch({ 420, "probing" });
    ASSERT_TRUE(launch);
    std::shared_ptr<const probe_fence_t> probe;
    ASSERT_TRUE(coordinator.run_launch_prepare(*launch.token, [&](const auto &, const auto &) {
                             probe = coordinator.hold_probe_fence();
                           })
        .completed());
    std::atomic_int mutations {};
    EXPECT_TRUE(coordinator.status().fence_active);
    EXPECT_EQ(coordinator.admit_launch({ 421, "competing" }).reason, reject_reason_e::fence_active);
    EXPECT_EQ(coordinator.post_tracked(command_kind_e::rotate, counting(mutations)).reason, reject_reason_e::fence_active);
    std::vector<posted_command_t> cleanup;
    for (const auto kind : expected_cleanup_kinds) {
      cleanup.push_back(coordinator.post_tracked(kind, counting(mutations)));
      EXPECT_EQ(cleanup.back().reason, reject_reason_e::none);
      EXPECT_EQ(cleanup.back().completion.wait_for(50ms), std::future_status::timeout)
        << to_string(kind) << " mutated during a probe";
    }
    EXPECT_EQ(mutations.load(), 0);
    probe.reset();
    for (const auto &command : cleanup) {
      EXPECT_TRUE(command.completion.get().completed());
    }
    EXPECT_EQ(mutations.load(), expected_cleanup_kinds.size());
    EXPECT_FALSE(coordinator.status().fence_active);
  }

  TEST(DisplayTopologyCoordinator, ProbeFenceOrdersExpiredSetupRestoreBeforeReplacement) {
    for (const bool managed : { false, true }) {
      fake_clock_t clock;
      event_log_t log;
      auto options = make_options(clock, managed);
      if (managed) options.native_restore = logging(log, "restore");
      coordinator_t coordinator { options };
      ASSERT_TRUE(start_ready(coordinator).completed());
      const auto first = coordinator.admit_launch({ 422, "expiring" });
      ASSERT_TRUE(first);
      std::shared_ptr<const probe_fence_t> probe;
      ASSERT_TRUE(coordinator.run_launch_prepare(*first.token, [&](const auto &, const auto &) {
        probe = coordinator.hold_probe_fence();
        log.add("first"); }, logging(log, "restore")).completed());
      clock.advance(1000ms);
      coordinator.tick();
      EXPECT_FALSE(coordinator.status().lease);
      EXPECT_FALSE(coordinator.bind_launch(*first.token));
      EXPECT_EQ(coordinator.admit_launch({ 423, "too early" }).reason, reject_reason_e::fence_active);
      const auto barrier = coordinator.post_tracked(command_kind_e::stream_restore, logging(log, "barrier"));
      EXPECT_EQ(barrier.completion.wait_for(50ms), std::future_status::timeout);
      EXPECT_EQ(log.events(), (std::vector<std::string> { "first" }));
      probe.reset();
      const auto next = coordinator.admit_launch({ 424, "replacement" });
      ASSERT_TRUE(next);
      ASSERT_TRUE(coordinator.run_launch_prepare(*next.token, logging(log, "next")).completed());
      EXPECT_TRUE(barrier.completion.get().completed());
      EXPECT_EQ(log.events(), (std::vector<std::string> { "first", "restore", "barrier", "next" }));
    }
  }

  TEST(DisplayTopologyCoordinator, ProbeFenceReleaseDoesNotClearTeardownFence) {
    fake_clock_t clock;
    coordinator_t coordinator { make_options(clock) };
    ASSERT_TRUE(start_ready(coordinator).completed());
    std::shared_ptr<const probe_fence_t> probe;
    ASSERT_TRUE(coordinator.run(command_kind_e::rotate, [&](const auto &, const auto &) {
                             probe = coordinator.hold_probe_fence();
                           })
        .completed());
    auto teardown = coordinator.hold_fence();
    const auto cleanup = coordinator.post_tracked(command_kind_e::stream_restore, {});
    EXPECT_EQ(cleanup.completion.wait_for(50ms), std::future_status::timeout);
    probe.reset();
    EXPECT_EQ(cleanup.completion.wait_for(real_timeout), std::future_status::ready);
    EXPECT_TRUE(cleanup.completion.get().completed());
    EXPECT_EQ(coordinator.admit_launch({ 425, "still tearing down" }).reason, reject_reason_e::fence_active);
    teardown.reset();
    EXPECT_TRUE(coordinator.admit_launch({ 426, "ready" }));
  }

  TEST(DisplayTopologyCoordinator, ProbeFenceOutlivesStoppedCoordinatorWithoutRunningCleanup) {
    fake_clock_t clock;
    auto coordinator = std::make_unique<coordinator_t>(make_options(clock));
    ASSERT_TRUE(start_ready(*coordinator).completed());
    std::shared_ptr<const probe_fence_t> probe;
    ASSERT_TRUE(coordinator->run(command_kind_e::rotate, [&](const auto &, const auto &) {
                             probe = coordinator->hold_probe_fence();
                           })
        .completed());
    std::atomic_int mutations {};
    const auto cleanup = coordinator->post_tracked(command_kind_e::stream_restore, counting(mutations));
    auto stopped = std::async(std::launch::async, [&]() { coordinator->stop(); });
    const auto before_release = stopped.wait_for(real_timeout);
    if (before_release != std::future_status::ready) probe.reset();
    stopped.get();
    EXPECT_EQ(before_release, std::future_status::ready);
    EXPECT_EQ(cleanup.completion.get().outcome, outcome_e::stopped);
    coordinator.reset();
    probe.reset();
    EXPECT_EQ(mutations.load(), 0);
  }

  TEST(DisplayTopologyCoordinator, ProbeFenceRefusesOtherThreadsAndDrainingWorker) {
    fake_clock_t clock;
    coordinator_t coordinator { make_options(clock) };
    coordinator_t other { make_options(clock) };
    ASSERT_TRUE(start_ready(coordinator).completed());
    ASSERT_TRUE(start_ready(other).completed());
    EXPECT_THROW(coordinator.hold_probe_fence(), std::logic_error);
    gate_t worker;
    auto running = std::async(std::launch::async, [&]() {
      return coordinator.run(command_kind_e::rotate, [&](const auto &, const auto &) {
        EXPECT_THROW(other.hold_probe_fence(), std::logic_error);
        worker.hook()();
        coordinator.hold_probe_fence();
      });
    });
    const finally_t release { [&]() { worker.release(); } };
    ASSERT_TRUE(worker.wait_started());
    coordinator.begin_draining();
    worker.release();
    const auto result = running.get();
    EXPECT_EQ(result.outcome, outcome_e::failed);
    ASSERT_TRUE(result.exception);
    EXPECT_THROW(std::rethrow_exception(result.exception), std::logic_error);
    EXPECT_FALSE(coordinator.status().fence_active);
  }

  TEST(DisplayTopologyCoordinator, StagedPrepareRequiresExplicitCompletionBeforeBinding) {
    fake_clock_t clock;
    coordinator_t coordinator { make_options(clock) };
    ASSERT_TRUE(start_ready(coordinator).completed());
    const auto launch = coordinator.admit_launch({ 430, "staged" });
    ASSERT_TRUE(launch);
    EXPECT_FALSE(coordinator.finish_launch_prepare(*launch.token));
    ASSERT_TRUE(coordinator.run_launch_step(*launch.token, {}).completed());
    EXPECT_FALSE(coordinator.bind_launch(*launch.token))
      << "a mutation step alone must not publish the unprobed launch";
    ASSERT_TRUE(coordinator.finish_launch_prepare(*launch.token));
    EXPECT_FALSE(coordinator.finish_launch_prepare(*launch.token));
    EXPECT_TRUE(coordinator.bind_launch(*launch.token));
  }

  TEST(DisplayTopologyCoordinator, StagedFallbackRunsBetweenSupervisedOffWorkerProbes) {
    fake_clock_t clock;
    event_log_t log;
    coordinator_t coordinator { make_options(clock) };
    ASSERT_TRUE(start_ready(coordinator).completed());
    const auto launch = coordinator.admit_launch({ 431, "fallback" });
    ASSERT_TRUE(launch);
    std::shared_ptr<const probe_fence_t> probe;
    std::thread::id mutation_thread;
    ASSERT_TRUE(coordinator.run_launch_step(*launch.token, [&](const auto &, const auto &) {
      mutation_thread = std::this_thread::get_id();
      log.add("configure");
      probe = coordinator.hold_probe_fence(); }, logging(log, "rollback")).completed());
    gate_t probing;
    auto result = std::async(std::launch::async, [&, guard = std::move(probe)]() mutable {
      EXPECT_NE(std::this_thread::get_id(), mutation_thread);
      log.add("probe");
      probing.hook()();
      log.add("probe teardown");
      guard.reset();
    });
    const finally_t release { [&]() { probing.release(); } };
    ASSERT_TRUE(probing.wait_started());
    EXPECT_FALSE(coordinator.finish_launch_prepare(*launch.token));
    EXPECT_FALSE(coordinator.bind_launch(*launch.token));
    EXPECT_EQ(coordinator.run_launch_step(*launch.token, {}).reason, reject_reason_e::fence_active);
    const auto cleanup = coordinator.post_tracked(command_kind_e::stream_restore, logging(log, "cleanup"));
    EXPECT_EQ(cleanup.completion.wait_for(50ms), std::future_status::timeout);
    probing.release();
    result.get();
    EXPECT_TRUE(cleanup.completion.get().completed());
    ASSERT_TRUE(coordinator.run_launch_step(*launch.token, [&](const auto &, const auto &) {
                             EXPECT_EQ(std::this_thread::get_id(), mutation_thread);
                             log.add("fallback configure");
                             probe = coordinator.hold_probe_fence();
                           })
        .completed());
    EXPECT_FALSE(coordinator.finish_launch_prepare(*launch.token));
    probe.reset();
    ASSERT_TRUE(coordinator.finish_launch_prepare(*launch.token));
    EXPECT_TRUE(coordinator.bind_launch(*launch.token));
    EXPECT_EQ(log.events(), (std::vector<std::string> {
                              "configure", "probe", "probe teardown", "cleanup", "fallback configure" }));
  }

  TEST(DisplayTopologyCoordinator, StagedStepsShareTheOriginalDeadlineAndCleanup) {
    for (const bool managed : { false, true }) {
      fake_clock_t clock;
      std::atomic_int restores {}, mutations {};
      auto options = make_options(clock, managed);
      if (managed) options.native_restore = counting(restores);
      coordinator_t coordinator { options };
      ASSERT_TRUE(start_ready(coordinator).completed());
      const auto launch = coordinator.admit_launch({ 432, "deadline" });
      ASSERT_TRUE(launch);
      ASSERT_TRUE(coordinator.run_launch_step(*launch.token, counting(mutations), counting(restores)).completed());
      clock.advance(999ms);
      auto forged = *launch.token;
      forged.setup_deadline += 1h;
      ASSERT_TRUE(coordinator.run_launch_step(forged, counting(mutations)).completed());
      clock.advance(1ms);
      EXPECT_FALSE(coordinator.finish_launch_prepare(forged));
      EXPECT_FALSE(coordinator.bind_launch(forged));
      EXPECT_FALSE(coordinator.status().lease);
      EXPECT_EQ(coordinator.run_launch_step(forged, counting(mutations)).reason, reject_reason_e::stale_transaction);
      ASSERT_TRUE(flush(coordinator).completed());
      EXPECT_EQ(mutations.load(), 2);
      EXPECT_EQ(restores.load(), 1);
    }
  }

  TEST(DisplayTopologyCoordinator, StagedTerminationRestoresEarlierMutationWhenLatestStepNeverStarted) {
    for (const bool managed : { false, true }) {
      for (const bool expiry : { false, true }) {
        SCOPED_TRACE(::testing::Message() << "managed=" << managed << ", expiry=" << expiry);
        fake_clock_t clock;
        event_log_t log;
        auto options = make_options(clock, managed);
        if (managed) options.native_restore = logging(log, "restore");
        coordinator_t coordinator { options };
        ASSERT_TRUE(start_ready(coordinator).completed());
        const auto launch = coordinator.admit_launch({ 433, "queued continuation" });
        ASSERT_TRUE(launch);
        ASSERT_TRUE(coordinator.run_launch_step(*launch.token, logging(log, "first"), logging(log, "restore")).completed());
        gate_t blocker;
        const auto blocking = coordinator.post_tracked(command_kind_e::stream_restore, blocker.body());
        ASSERT_TRUE(blocker.wait_started());
        const auto before = submitted(coordinator);
        auto step = std::async(std::launch::async, [&]() {
          return coordinator.run_launch_step(*launch.token, logging(log, "never"));
        });
        const finally_t release { [&]() { blocker.release(); } };
        ASSERT_TRUE(wait_until([&]() { return submitted(coordinator) == before + 1; }));
        const auto unrelated = coordinator.post_tracked(command_kind_e::stream_restore, logging(log, "unrelated"));
        if (expiry) {
          clock.advance(1000ms);
          coordinator.tick();
        }
        else {
          EXPECT_TRUE(coordinator.abort_launch(*launch.token, {}));
        }
        EXPECT_FALSE(coordinator.abort_launch(*launch.token, {}));
        EXPECT_FALSE(coordinator.status().lease);
        const auto next = coordinator.admit_launch({ 434, "replacement" });
        ASSERT_TRUE(next);
        blocker.release();
        EXPECT_EQ(step.get().outcome, expiry ? outcome_e::expired_before_start : outcome_e::cancelled);
        EXPECT_TRUE(blocking.completion.get().completed());
        EXPECT_TRUE(unrelated.completion.get().completed());
        ASSERT_TRUE(coordinator.run_launch_step(*next.token, logging(log, "next")).completed());
        EXPECT_EQ(log.events(), (std::vector<std::string> { "first", "unrelated", "restore", "next" }));
      }
    }
  }

  TEST(DisplayTopologyCoordinator, ConcurrentStagedStepCannotRunOrSealAndSealedStepsCannotResume) {
    fake_clock_t clock;
    coordinator_t coordinator { make_options(clock) };
    ASSERT_TRUE(start_ready(coordinator).completed());
    const auto launch = coordinator.admit_launch({ 435, "concurrent" });
    ASSERT_TRUE(launch);
    ASSERT_TRUE(coordinator.run_launch_step(*launch.token, {}).completed());
    gate_t worker;
    auto running = std::async(std::launch::async, [&]() {
      return coordinator.run_launch_step(*launch.token, worker.body());
    });
    const finally_t release { [&]() { worker.release(); } };
    ASSERT_TRUE(worker.wait_started());
    std::atomic_int unexpected {};
    auto competing = std::async(std::launch::async, [&]() {
      return coordinator.run_launch_step(*launch.token, counting(unexpected));
    });
    EXPECT_EQ(competing.wait_for(real_timeout), std::future_status::ready)
      << "a concurrent step must reject without waiting on the in-flight mutation";
    EXPECT_FALSE(coordinator.finish_launch_prepare(*launch.token));
    EXPECT_FALSE(coordinator.bind_launch(*launch.token));
    worker.release();
    EXPECT_TRUE(running.get().completed());
    EXPECT_EQ(competing.get().reason, reject_reason_e::stale_transaction);
    ASSERT_TRUE(coordinator.finish_launch_prepare(*launch.token));
    EXPECT_EQ(coordinator.run_launch_step(*launch.token, counting(unexpected)).reason, reject_reason_e::stale_transaction);
    EXPECT_EQ(unexpected.load(), 0);
    EXPECT_TRUE(coordinator.bind_launch(*launch.token));
    EXPECT_EQ(coordinator.run_launch_step(*launch.token, counting(unexpected)).reason, reject_reason_e::stale_transaction);
  }

  TEST(DisplayTopologyCoordinator, FailedStagedStepCannotContinueOrSealAndStillRestores) {
    fake_clock_t clock;
    std::atomic_int restores {};
    coordinator_t coordinator { make_options(clock) };
    ASSERT_TRUE(start_ready(coordinator).completed());
    const auto launch = coordinator.admit_launch({ 436, "failed continuation" });
    ASSERT_TRUE(launch);
    ASSERT_TRUE(coordinator.run_launch_step(*launch.token, {}, counting(restores)).completed());
    const auto result = coordinator.run_launch_step(*launch.token, [](const auto &, const auto &) {
      throw std::runtime_error { "fallback mutation failed" };
    });
    EXPECT_EQ(result.outcome, outcome_e::failed);
    ASSERT_TRUE(result.exception);
    EXPECT_FALSE(coordinator.finish_launch_prepare(*launch.token));
    EXPECT_FALSE(coordinator.bind_launch(*launch.token));
    EXPECT_EQ(coordinator.run_launch_step(*launch.token, {}).reason, reject_reason_e::stale_transaction);
    EXPECT_TRUE(coordinator.abort_launch(*launch.token, {}));
    ASSERT_TRUE(flush(coordinator).completed());
    EXPECT_EQ(restores.load(), 1);
  }

  TEST(DisplayTopologyCoordinator, StagedAndOneShotPreparationCannotBeMixed) {
    for (const bool staged_first : { false, true }) {
      fake_clock_t clock;
      coordinator_t coordinator { make_options(clock) };
      ASSERT_TRUE(start_ready(coordinator).completed());
      const auto launch = coordinator.admit_launch({ 437, "mode" });
      ASSERT_TRUE(launch);
      if (staged_first) {
        ASSERT_TRUE(coordinator.run_launch_step(*launch.token, {}).completed());
        EXPECT_EQ(coordinator.run_launch_prepare(*launch.token, {}).reason, reject_reason_e::stale_transaction);
        EXPECT_TRUE(coordinator.finish_launch_prepare(*launch.token));
      }
      else {
        ASSERT_TRUE(coordinator.run_launch_prepare(*launch.token, {}).completed());
        EXPECT_EQ(coordinator.run_launch_step(*launch.token, {}).reason, reject_reason_e::stale_transaction);
        EXPECT_FALSE(coordinator.finish_launch_prepare(*launch.token));
        EXPECT_EQ(coordinator.run_launch_prepare(*launch.token, {}).reason, reject_reason_e::stale_transaction);
      }
    }
  }

  TEST(DisplayTopologyCoordinator, StagedStepAndFinishRequireFullOwnerTokenNotJoinToken) {
    fake_clock_t clock;
    coordinator_t coordinator { make_options(clock, true) };
    ASSERT_TRUE(start_ready(coordinator).completed());
    const auto launch = coordinator.admit_launch({ 438, "owner" });
    ASSERT_TRUE(launch);
    ASSERT_TRUE(coordinator.run_launch_step(*launch.token, {}).completed());
    auto wrong_transaction = *launch.token;
    ++wrong_transaction.transaction;
    auto wrong_id = *launch.token;
    ++wrong_id.launch_session_id;
    auto wrong_flavor = *launch.token;
    wrong_flavor.flavor = launch_flavor_e::legacy;
    auto wrong_serial = *launch.token;
    ++wrong_serial.attachment_serial;
    std::atomic_int mutations {};
    for (const auto &token : { wrong_transaction, wrong_id, wrong_flavor, wrong_serial }) {
      EXPECT_EQ(coordinator.run_launch_step(token, counting(mutations)).reason, reject_reason_e::stale_transaction);
      EXPECT_FALSE(coordinator.finish_launch_prepare(token));
    }
    ASSERT_TRUE(coordinator.finish_launch_prepare(*launch.token));
    ASSERT_TRUE(coordinator.bind_launch(*launch.token));
    const auto join = coordinator.admit_managed_join({ 439, "join" });
    ASSERT_TRUE(join);
    EXPECT_EQ(coordinator.run_launch_step(*join.token, counting(mutations)).reason, reject_reason_e::stale_transaction);
    EXPECT_FALSE(coordinator.finish_launch_prepare(*join.token));
    EXPECT_EQ(mutations.load(), 0);
  }

  TEST(DisplayTopologyCoordinator, StagedCleanupCannotBeReplacedByContinuationOrAbort) {
    fake_clock_t clock;
    std::atomic_int original {}, replacement {};
    coordinator_t coordinator { make_options(clock) };
    ASSERT_TRUE(start_ready(coordinator).completed());
    const auto launch = coordinator.admit_launch({ 440, "cleanup" });
    ASSERT_TRUE(launch);
    ASSERT_TRUE(coordinator.run_launch_step(*launch.token, {}, counting(original)).completed());
    EXPECT_THROW(coordinator.run_launch_step(*launch.token, {}, counting(replacement)), std::invalid_argument);
    EXPECT_THROW(coordinator.abort_launch(*launch.token, counting(replacement)), std::invalid_argument);
    ASSERT_TRUE(coordinator.run_launch_step(*launch.token, {}).completed());
    ASSERT_TRUE(coordinator.abort_launch(*launch.token, {}));
    ASSERT_TRUE(flush(coordinator).completed());
    EXPECT_EQ(original.load(), 1);
    EXPECT_EQ(replacement.load(), 0);
  }

  TEST(DisplayTopologyCoordinator, UnstartedStagedTerminationDoesNotRestore) {
    for (const bool managed : { false, true }) {
      for (const bool expiry : { false, true }) {
        fake_clock_t clock;
        std::atomic_int calls {};
        auto options = make_options(clock, managed);
        if (managed) options.native_restore = counting(calls);
        coordinator_t coordinator { options };
        ASSERT_TRUE(start_ready(coordinator).completed());
        gate_t blocker;
        const auto blocking = coordinator.post_tracked(command_kind_e::stream_restore, blocker.body());
        ASSERT_TRUE(blocker.wait_started());
        const auto launch = coordinator.admit_launch({ 441, "unstarted" });
        ASSERT_TRUE(launch);
        const auto before = submitted(coordinator);
        auto step = std::async(std::launch::async, [&]() {
          return coordinator.run_launch_step(*launch.token, counting(calls), counting(calls));
        });
        const finally_t release { [&]() { blocker.release(); } };
        ASSERT_TRUE(wait_until([&]() { return submitted(coordinator) == before + 1; }));
        if (expiry) {
          clock.advance(1000ms);
          coordinator.tick();
        }
        else {
          EXPECT_TRUE(coordinator.abort_launch(*launch.token, {}));
        }
        blocker.release();
        EXPECT_FALSE(step.get().completed());
        EXPECT_TRUE(blocking.completion.get().completed());
        ASSERT_TRUE(flush(coordinator).completed());
        EXPECT_EQ(calls.load(), 0);
      }
    }
  }

  TEST(DisplayTopologyCoordinator, ProbeFencePreventsOneShotTicketPublication) {
    fake_clock_t clock;
    coordinator_t coordinator { make_options(clock) };
    ASSERT_TRUE(start_ready(coordinator).completed());
    const auto launch = coordinator.admit_launch({ 442, "one-shot probe" });
    ASSERT_TRUE(launch);
    std::shared_ptr<const probe_fence_t> probe;
    ASSERT_TRUE(coordinator.run_launch_prepare(*launch.token, [&](const auto &, const auto &) {
                             probe = coordinator.hold_probe_fence();
                           })
        .completed());
    EXPECT_FALSE(coordinator.bind_launch(*launch.token));
    probe.reset();
    EXPECT_TRUE(coordinator.bind_launch(*launch.token));
  }

  TEST(DisplayTopologyCoordinator, StagedPreparationRejectsReentryAndShutdownProgress) {
    for (const bool stop : { false, true }) {
      fake_clock_t clock;
      coordinator_t coordinator { make_options(clock) };
      ASSERT_TRUE(start_ready(coordinator).completed());
      const auto launch = coordinator.admit_launch({ 443, "shutdown" });
      ASSERT_TRUE(launch);
      ASSERT_TRUE(coordinator.run_launch_step(*launch.token, [&](const auto &, const auto &) {
                               EXPECT_EQ(coordinator.run_launch_step(*launch.token, {}).outcome, outcome_e::reentrant);
                               EXPECT_FALSE(coordinator.finish_launch_prepare(*launch.token));
                             })
          .completed());
      if (stop)
        coordinator.stop();
      else
        coordinator.begin_draining();
      EXPECT_FALSE(coordinator.finish_launch_prepare(*launch.token));
      EXPECT_FALSE(coordinator.bind_launch(*launch.token));
      EXPECT_EQ(coordinator.run_launch_step(*launch.token, {}).reason,
        stop ? reject_reason_e::stopped : reject_reason_e::draining);
    }
  }

  TEST(DisplayTopologyCoordinator, FenceRefusesNonCleanupWorkUntilItSettles) {
    fake_clock_t clock;
    std::atomic_bool settled {};
    std::atomic_int calls {};
    coordinator_t coordinator { make_options(clock) };
    ASSERT_TRUE(start_ready(coordinator).completed());

    coordinator.install_fence([&]() {
      return settled.load();
    });
    EXPECT_TRUE(coordinator.status().fence_active);
    EXPECT_EQ(coordinator.post(command_kind_e::rotate, counting(calls)), reject_reason_e::fence_active);
    EXPECT_EQ(coordinator.admit_launch({ 171, "cert:a" }).reason, reject_reason_e::fence_active);
    EXPECT_EQ(coordinator.post(command_kind_e::stream_restore, counting(calls)), reject_reason_e::none);

    settled = true;
    EXPECT_EQ(coordinator.post(command_kind_e::rotate, counting(calls)), reject_reason_e::none);
    EXPECT_FALSE(coordinator.status().fence_active);
    settled = false;
    EXPECT_EQ(coordinator.post(command_kind_e::scale, counting(calls)), reject_reason_e::none) << "a settled fence must stay cleared";
    ASSERT_TRUE(flush(coordinator).completed());
    EXPECT_EQ(calls.load(), 3);
  }

  TEST(DisplayTopologyCoordinator, RunFromTheWorkerIsReentrantWhilePostIsQueued) {
    fake_clock_t clock;
    std::atomic_int scales {};
    std::atomic<outcome_e> inner_run { outcome_e::completed };
    std::atomic<outcome_e> inner_prepare { outcome_e::completed };
    std::atomic<reject_reason_e> inner_post { reject_reason_e::stopped };
    coordinator_t coordinator { make_options(clock) };
    ASSERT_TRUE(start_ready(coordinator).completed());
    const auto launch = coordinator.admit_launch({ 181, "cert:a" });
    ASSERT_TRUE(launch);

    auto outer = std::async(std::launch::async, [&]() {
      return coordinator.run(command_kind_e::rotate, [&](const mutation_capability_t &, const command_context_t &) {
        inner_run = coordinator.run(command_kind_e::scale, counting(scales)).outcome;
        inner_prepare = coordinator.run_launch_prepare(*launch.token, counting(scales)).outcome;
        inner_post = coordinator.post(command_kind_e::scale, counting(scales));
      });
    });
    ASSERT_EQ(outer.wait_for(real_timeout), std::future_status::ready) << "a nested run() deadlocked the worker";
    EXPECT_TRUE(outer.get().completed());
    EXPECT_EQ(inner_run.load(), outcome_e::reentrant);
    EXPECT_EQ(inner_prepare.load(), outcome_e::reentrant);
    EXPECT_EQ(inner_post.load(), reject_reason_e::none);
    ASSERT_TRUE(flush(coordinator).completed());
    EXPECT_EQ(scales.load(), 1);
  }

  TEST(DisplayTopologyCoordinator, PostedShutdownCancellationIsObservedWithoutRunningTheBody) {
    fake_clock_t clock;
    posted_log_t posted;
    auto options = make_options(clock);
    options.posted_observer = posted.observer();
    coordinator_t coordinator { options };
    ASSERT_TRUE(coordinator.start({}).get().completed());
    gate_t worker;
    ASSERT_EQ(coordinator.post(command_kind_e::rotate, worker.body()), reject_reason_e::none);
    ASSERT_TRUE(worker.wait_started());
    std::atomic_int calls {};
    const auto retry = coordinator.post_tracked(command_kind_e::legacy_retry_tick, counting(calls));
    ASSERT_EQ(retry.reason, reject_reason_e::none);
    auto stopping = std::async(std::launch::async, [&]() { coordinator.stop(); });
    EXPECT_TRUE(wait_until([&]() { return coordinator.status().phase == coordinator_phase_e::stopped; }));
    worker.release();
    stopping.get();
    EXPECT_EQ(calls, 0);
    ASSERT_EQ(retry.completion.wait_for(real_timeout), std::future_status::ready);
    EXPECT_EQ(retry.completion.get().outcome, outcome_e::stopped);
    const auto records = posted.records();
    ASSERT_EQ(records.size(), 2u);
    EXPECT_EQ(records[1].kind, command_kind_e::legacy_retry_tick);
    EXPECT_EQ(records[1].outcome, outcome_e::stopped);
    EXPECT_FALSE(records[1].has_exception);
  }

  TEST(DisplayTopologyCoordinator, TrackedPostsReportEagerRejectionWithoutAnObserverOrBodyCall) {
    fake_clock_t clock;
    posted_log_t posted;
    std::atomic_int calls {};
    auto options = make_options(clock);
    options.posted_observer = posted.observer();
    coordinator_t coordinator { options };
    const auto check = [&](reject_reason_e reason) {
      const auto receipt = coordinator.post_tracked(command_kind_e::legacy_retry_tick, counting(calls));
      EXPECT_EQ(receipt.reason, reason);
      EXPECT_EQ(receipt.handle.id, 0u);
      ASSERT_EQ(receipt.completion.wait_for(0ms), std::future_status::ready);
      EXPECT_EQ(receipt.completion.get().outcome, outcome_e::rejected);
      EXPECT_EQ(receipt.completion.get().reason, reason);
    };
    check(reject_reason_e::not_started);
    ASSERT_TRUE(start_ready(coordinator).completed());
    coordinator.install_fence([]() { return false; });
    check(reject_reason_e::fence_active);
    coordinator.begin_draining();
    check(reject_reason_e::draining);
    coordinator.stop();
    check(reject_reason_e::stopped);
    EXPECT_EQ(calls, 0);
    EXPECT_TRUE(posted.records().empty());
  }

  TEST(DisplayTopologyCoordinator, TrackedPostsKeepDistinctReceiptsThroughWorkerRejection) {
    fake_clock_t clock;
    std::atomic_int calls {};
    posted_log_t posted;
    auto options = make_options(clock);
    options.posted_observer = posted.observer();
    coordinator_t coordinator { options };
    ASSERT_TRUE(start_ready(coordinator).completed());
    gate_t worker;
    const auto blocking = coordinator.post_tracked(command_kind_e::rotate, worker.body());
    ASSERT_TRUE(worker.wait_started());
    const auto first = coordinator.post_tracked(command_kind_e::legacy_retry_tick, counting(calls));
    const auto second = coordinator.post_tracked(command_kind_e::legacy_retry_tick, counting(calls));
    ASSERT_EQ(first.reason, reject_reason_e::none);
    ASSERT_EQ(second.reason, reject_reason_e::none);
    EXPECT_NE(first.handle.id, second.handle.id);
    EXPECT_EQ(first.handle.generation, second.handle.generation);
    EXPECT_EQ(first.completion.wait_for(0ms), std::future_status::timeout);
    coordinator.install_fence([]() { return false; });
    worker.release();
    for (const auto &receipt : { first, second }) {
      ASSERT_EQ(receipt.completion.wait_for(real_timeout), std::future_status::ready);
      EXPECT_EQ(receipt.completion.get().outcome, outcome_e::rejected);
      EXPECT_EQ(receipt.completion.get().reason, reject_reason_e::fence_active);
    }
    EXPECT_TRUE(blocking.completion.get().completed());
    EXPECT_EQ(calls, 0);
    EXPECT_EQ(posted.records().size(), 3u);
  }

  TEST(DisplayTopologyCoordinator, TrackedRestoreWaitsForBodyAndObserverWithoutHoldingTheStateLock) {
    fake_clock_t clock;
    gate_t observer;
    coordinator_t *owner {};
    std::atomic<outcome_e> nested { outcome_e::completed };
    auto options = make_options(clock);
    options.posted_observer = [&](command_kind_e, const command_result_t &) {
      EXPECT_EQ(owner->status().phase, coordinator_phase_e::running);
      nested = owner->run(command_kind_e::rotate, {}).outcome;
      observer.hook()();
    };
    coordinator_t coordinator { options };
    owner = &coordinator;
    ASSERT_TRUE(start_ready(coordinator).completed());
    gate_t worker;
    const auto release_observer = finally_t { [&]() { observer.release(); } };
    const auto receipt = coordinator.post_tracked(command_kind_e::stream_restore, worker.body());
    ASSERT_TRUE(worker.wait_started());
    EXPECT_EQ(receipt.completion.wait_for(0ms), std::future_status::timeout);
    worker.release();
    ASSERT_TRUE(observer.wait_started());
    EXPECT_EQ(receipt.completion.wait_for(0ms), std::future_status::timeout);
    observer.release();
    ASSERT_EQ(receipt.completion.wait_for(real_timeout), std::future_status::ready);
    EXPECT_TRUE(receipt.completion.get().completed());
    EXPECT_EQ(nested.load(), outcome_e::reentrant);
  }

  TEST(DisplayTopologyCoordinator, TrackedPostsSurfaceBodyAndObserverFailuresAndKeepProcessing) {
    fake_clock_t clock;
    auto options = make_options(clock);
    options.posted_observer = [](command_kind_e kind, const command_result_t &) {
      if (kind == command_kind_e::scale) {
        throw std::logic_error { "observer failed" };
      }
    };
    coordinator_t coordinator { options };
    ASSERT_TRUE(start_ready(coordinator).completed());
    const auto body = coordinator.post_tracked(command_kind_e::rotate, [](const auto &, const auto &) {
      throw std::runtime_error { "body failed" };
    });
    const auto observer = coordinator.post_tracked(command_kind_e::scale, {});
    for (const auto &receipt : { body, observer }) {
      ASSERT_EQ(receipt.completion.wait_for(real_timeout), std::future_status::ready);
      EXPECT_EQ(receipt.completion.get().outcome, outcome_e::failed);
      ASSERT_TRUE(receipt.completion.get().exception);
    }
    EXPECT_THROW(std::rethrow_exception(body.completion.get().exception), std::runtime_error);
    EXPECT_THROW(std::rethrow_exception(observer.completion.get().exception), std::logic_error);
    EXPECT_TRUE(coordinator.post_tracked(command_kind_e::stream_restore, {}).completion.get().completed());
  }

  TEST(DisplayTopologyCoordinator, PostedFailuresAreReportedAndTheWorkerContinues) {
    fake_clock_t clock;
    posted_log_t posted;
    auto options = make_options(clock);
    options.posted_observer = posted.observer();
    coordinator_t coordinator { std::move(options) };
    ASSERT_TRUE(start_ready(coordinator).completed());

    ASSERT_EQ(coordinator.post(command_kind_e::rotate, [](const mutation_capability_t &, const command_context_t &) {
      throw std::runtime_error { "rotation failed" };
    }),
      reject_reason_e::none);
    EXPECT_TRUE(coordinator.run(command_kind_e::scale, {}).completed());

    const auto records = posted.records();
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].kind, command_kind_e::rotate);
    EXPECT_EQ(records[0].outcome, outcome_e::failed);
    EXPECT_TRUE(records[0].has_exception);
  }

  TEST(DisplayTopologyCoordinator, QueuedCommandsAreRevalidatedAgainstChangesMadeWhileTheyWaited) {
    struct case_t {
      const char *name;
      command_kind_e kind;
      std::function<void(coordinator_t &)> change;
      reject_reason_e expected;
    };
    const std::vector<case_t> cases {
      { "draining", command_kind_e::rotate, [](coordinator_t &c) { c.begin_draining(); }, reject_reason_e::draining },
      { "fence", command_kind_e::rotate, [](coordinator_t &c) { c.install_fence([]() { return false; }); }, reject_reason_e::fence_active },
      { "native journal", command_kind_e::shutdown_vdd_destroy,
        [](coordinator_t &c) { c.publish_native_health({ true, false, "Journal present" }); }, reject_reason_e::recovery_required },
      { "native conflict", command_kind_e::shutdown_vdd_destroy,
        [](coordinator_t &c) { c.publish_native_health({ false, true, "In conflict" }); }, reject_reason_e::recovery_required },
    };
    for (const auto &test_case : cases) {
      SCOPED_TRACE(test_case.name);
      fake_clock_t clock;
      posted_log_t posted;
      std::atomic_int calls {};
      auto options = make_options(clock);
      options.posted_observer = posted.observer();
      coordinator_t coordinator { std::move(options) };
      ASSERT_TRUE(start_ready(coordinator).completed());
      gate_t busy;
      std::future<command_result_t> blocker;
      const finally_t release { [&]() { busy.release(); } };

      blocker = std::async(std::launch::async, [&]() {
        return coordinator.run(command_kind_e::stream_restore, busy.body());
      });
      ASSERT_TRUE(busy.wait_started());
      ASSERT_EQ(coordinator.post(test_case.kind, counting(calls)), reject_reason_e::none);
      test_case.change(coordinator);
      busy.release();
      EXPECT_TRUE(blocker.get().completed());
      ASSERT_TRUE(flush(coordinator).completed());

      EXPECT_EQ(calls.load(), 0);
      const auto records = posted.records();
      ASSERT_EQ(records.size(), 1u);
      EXPECT_EQ(records[0].kind, test_case.kind);
      EXPECT_EQ(records[0].outcome, outcome_e::rejected);
      EXPECT_EQ(records[0].reason, test_case.expected);
    }
  }

  TEST(DisplayTopologyCoordinator, TickThreadSurvivesAThrowingProbe) {
    fake_clock_t clock;
    std::atomic_int probes {};
    auto options = make_options(clock, true);
    options.tick_interval = 1ms;
    options.probes.ticket_present = [&](std::uint32_t) -> bool {
      if (++probes == 1) {
        throw std::runtime_error { "session lookup failed" };
      }
      return false;
    };
    coordinator_t coordinator { std::move(options) };
    ASSERT_TRUE(start_ready(coordinator).completed());

    const auto launch = coordinator.admit_launch({ 281, "cert:a" });
    ASSERT_TRUE(launch);
    ASSERT_TRUE(prepare_and_bind(coordinator, *launch.token));
    EXPECT_TRUE(wait_until([&]() { return coordinator.status().native_restores_posted == 1; })) << "the tick thread stopped after a probe threw";
    EXPECT_GE(probes.load(), 2);
  }
  TEST(DisplayTopologyCoordinator, ManagedJoinProtectsTopologyInEveryReferenceStage) {
    for (const auto stage : { lease_stage_e::preparing, lease_stage_e::bound, lease_stage_e::active }) {
      SCOPED_TRACE(to_string(stage));
      fake_clock_t clock;
      event_log_t log;
      auto options = make_options(clock, true);
      options.probes.active_streams = []() { return 0u; };
      options.probes.ticket_present = [](auto) { return true; };
      options.native_restore = [&](const auto &cap, const auto &) { log.add(std::string { to_string(cap.kind()) }); };
      coordinator_t coordinator { std::move(options) };
      ASSERT_TRUE(start_ready(coordinator).completed());
      const auto owner = coordinator.admit_launch({ 401, "a" });
      ASSERT_TRUE(owner);
      ASSERT_TRUE(prepare_and_bind(coordinator, *owner.token));
      ASSERT_TRUE(coordinator.launch_activated(*owner.token));
      const auto join = coordinator.admit_managed_join({ 402, "b" });
      ASSERT_TRUE(join);
      EXPECT_EQ(join.token->transaction, owner.token->transaction);
      EXPECT_NE(join.token->attachment_serial, 0u);
      if (stage != lease_stage_e::preparing) {
        ASSERT_TRUE(coordinator.bind_launch(*join.token));
      }
      if (stage == lease_stage_e::active) {
        ASSERT_TRUE(coordinator.launch_activated(*join.token));
      }
      ASSERT_TRUE(coordinator.end_managed_launch(*owner.token));
      ASSERT_TRUE(coordinator.status().lease);
      EXPECT_EQ(coordinator.status().lease->launch_session_id, 401u);
      EXPECT_EQ(coordinator.status().lease->attachments, 1u);
      EXPECT_EQ(coordinator.status().native_restores_posted, 0u);
      if (stage == lease_stage_e::preparing) {
        ASSERT_TRUE(coordinator.abort_launch(*join.token, {}));
      }
      else {
        ASSERT_TRUE(coordinator.end_managed_launch(*join.token, command_kind_e::native_restore));
      }
      ASSERT_TRUE(flush(coordinator).completed());
      EXPECT_FALSE(coordinator.status().lease);
      EXPECT_EQ(log.events(), (std::vector<std::string> { "stream_restore" })) << "the first restore obligation is sticky";
    }
  }

  TEST(DisplayTopologyCoordinator, ManagedJoinAdmissionIsBoundedAndNeverPreparesTopology) {
    fake_clock_t clock;
    auto options = make_options(clock, true);
    options.max_managed_attachments = 2;
    coordinator_t coordinator { std::move(options) };
    EXPECT_EQ(coordinator.admit_managed_join({ 402, "b" }).reason, reject_reason_e::not_started);
    ASSERT_TRUE(start_ready(coordinator).completed());
    EXPECT_EQ(coordinator.admit_managed_join({ 402, "b" }).reason, reject_reason_e::stale_transaction);
    const auto owner = coordinator.admit_launch({ 401, "a" });
    ASSERT_TRUE(owner);
    EXPECT_EQ(coordinator.admit_managed_join({ 402, "b" }).reason, reject_reason_e::owner_busy);
    ASSERT_TRUE(prepare_and_bind(coordinator, *owner.token));
    auto fence = coordinator.hold_fence();
    EXPECT_EQ(coordinator.admit_managed_join({ 402, "b" }).reason, reject_reason_e::fence_active);
    fence.reset();
    coordinator.publish_native_health({ true, true, "conflict" });
    EXPECT_EQ(coordinator.admit_managed_join({ 402, "b" }).reason, reject_reason_e::recovery_required);
    coordinator.publish_native_health({ true, false, {} });
    EXPECT_EQ(coordinator.admit_managed_join({ 401, "a" }).reason, reject_reason_e::owner_busy);
    const auto before = submitted(coordinator);
    const auto join = coordinator.admit_managed_join({ 402, "b" });
    ASSERT_TRUE(join);
    EXPECT_EQ(coordinator.admit_managed_join({ 403, "c" }).reason, reject_reason_e::owner_busy);
    EXPECT_EQ(coordinator.run_launch_prepare(*join.token, {}).reason, reject_reason_e::stale_transaction);
    EXPECT_EQ(submitted(coordinator), before);
    coordinator.begin_draining();
    EXPECT_EQ(coordinator.admit_managed_join({ 403, "c" }).reason, reject_reason_e::draining);
    auto invalid = make_options(clock, true);
    invalid.max_managed_attachments = 0;
    EXPECT_THROW(coordinator_t { invalid }, std::invalid_argument);
    auto missing_probe = make_options(clock, true);
    missing_probe.probes.active_streams = {};
    EXPECT_THROW(coordinator_t { missing_probe }, std::invalid_argument);
  }

  TEST(DisplayTopologyCoordinator, ActivationRequiresBoundTokenAndTerminalRollbackCanEndBoundReference) {
    for (const bool managed : { false, true }) {
      fake_clock_t clock;
      coordinator_t coordinator { make_options(clock, managed) };
      ASSERT_TRUE(start_ready(coordinator).completed());
      const auto owner = coordinator.admit_launch({ 401, "a" });
      ASSERT_TRUE(owner);
      ASSERT_TRUE(coordinator.run_launch_prepare(*owner.token, {}).completed());
      EXPECT_FALSE(coordinator.launch_activated(*owner.token));
      ASSERT_TRUE(coordinator.bind_launch(*owner.token));
      if (managed) {
        EXPECT_FALSE(coordinator.abort_launch(*owner.token, {}));
        EXPECT_TRUE(coordinator.end_managed_launch(*owner.token));
      }
      else {
        EXPECT_TRUE(coordinator.launch_activated(*owner.token));
      }
      EXPECT_FALSE(coordinator.status().lease);
      EXPECT_EQ(coordinator.status().native_restores_posted, managed ? 1u : 0u);
    }
  }

  TEST(DisplayTopologyCoordinator, ManagedJoinAbortCannotRemovePublishedOrConsumedReferences) {
    fake_clock_t clock;
    coordinator_t coordinator { make_options(clock, true) };
    ASSERT_TRUE(start_ready(coordinator).completed());
    const auto owner = coordinator.admit_launch({ 401, "a" });
    ASSERT_TRUE(owner);
    ASSERT_TRUE(prepare_and_bind(coordinator, *owner.token));
    const auto join = coordinator.admit_managed_join({ 402, "b" });
    ASSERT_TRUE(join);
    EXPECT_FALSE(coordinator.launch_activated(*join.token));
    EXPECT_TRUE(coordinator.abort_launch(*join.token, {}));
    const auto next = coordinator.admit_managed_join({ 402, "b" });
    ASSERT_TRUE(next);
    ASSERT_TRUE(coordinator.bind_launch(*next.token));
    EXPECT_FALSE(coordinator.abort_launch(*next.token, {}));
    ASSERT_TRUE(coordinator.launch_activated(*next.token));
    EXPECT_FALSE(coordinator.abort_launch(*next.token, {}));
    EXPECT_EQ(coordinator.status().lease->attachments, 2u);
  }

  TEST(DisplayTopologyCoordinator, ManagedReferenceTokensRejectReusedIdsAndWrongIdentity) {
    fake_clock_t clock;
    coordinator_t coordinator { make_options(clock, true) };
    ASSERT_TRUE(start_ready(coordinator).completed());
    const auto owner = coordinator.admit_launch({ 401, "a" });
    ASSERT_TRUE(owner);
    ASSERT_TRUE(prepare_and_bind(coordinator, *owner.token));
    const auto old = coordinator.admit_managed_join({ 402, "b" });
    ASSERT_TRUE(old);
    ASSERT_TRUE(coordinator.abort_launch(*old.token, {}));
    const auto join = coordinator.admit_managed_join({ 402, "b" });
    ASSERT_TRUE(join);
    EXPECT_NE(old.token->attachment_serial, join.token->attachment_serial);
    EXPECT_FALSE(coordinator.bind_launch(*old.token));
    EXPECT_FALSE(coordinator.abort_launch(*old.token, {}));
    ASSERT_TRUE(coordinator.bind_launch(*join.token));
    EXPECT_FALSE(coordinator.launch_activated(*old.token));
    EXPECT_FALSE(coordinator.end_managed_launch(*old.token));
    auto wrong = *join.token;
    wrong.launch_session_id = 999;
    EXPECT_FALSE(coordinator.launch_activated(wrong));
    EXPECT_FALSE(coordinator.end_managed_launch(wrong));
    ASSERT_TRUE(coordinator.end_managed_launch(*join.token));
    ASSERT_TRUE(coordinator.end_managed_launch(*owner.token));
    const auto replacement = coordinator.admit_launch({ 401, "a" });
    ASSERT_TRUE(replacement);
    ASSERT_TRUE(prepare_and_bind(coordinator, *replacement.token));
    EXPECT_FALSE(coordinator.launch_activated(*owner.token));
    EXPECT_FALSE(coordinator.end_managed_launch(*owner.token));
    EXPECT_TRUE(coordinator.status().lease);
  }

  TEST(DisplayTopologyCoordinator, OnlyPreparingManagedJoinsExpireAtTheirOwnSetupDeadline) {
    for (const auto stage : { lease_stage_e::preparing, lease_stage_e::bound, lease_stage_e::active }) {
      SCOPED_TRACE(to_string(stage));
      fake_clock_t clock;
      auto options = make_options(clock, true);
      options.probes.ticket_present = [](auto) { return true; };
      options.probes.active_streams = []() { return 1u; };
      coordinator_t coordinator { std::move(options) };
      ASSERT_TRUE(start_ready(coordinator).completed());
      const auto owner = coordinator.admit_launch({ 401, "a" });
      ASSERT_TRUE(owner);
      ASSERT_TRUE(prepare_and_bind(coordinator, *owner.token));
      const auto join = coordinator.admit_managed_join({ 402, "b" });
      ASSERT_TRUE(join);
      if (stage != lease_stage_e::preparing) {
        ASSERT_TRUE(coordinator.bind_launch(*join.token));
      }
      if (stage == lease_stage_e::active) {
        ASSERT_TRUE(coordinator.launch_activated(*join.token));
      }
      clock.advance(999ms);
      coordinator.tick();
      EXPECT_EQ(coordinator.status().lease->attachments, 2u);
      clock.advance(1ms);
      coordinator.tick();
      EXPECT_EQ(coordinator.status().lease->attachments, stage == lease_stage_e::preparing ? 1u : 2u);
      auto forged = *join.token;
      forged.setup_deadline = monotonic_time_t::max();
      EXPECT_FALSE(coordinator.bind_launch(forged));
      EXPECT_EQ(coordinator.status().native_restores_posted, 0u);
    }
  }

  TEST(DisplayTopologyCoordinator, BoundManagedJoinSurvivesItsStaleTicketProbeAfterActivation) {
    fake_clock_t clock;
    gate_t gate;
    std::atomic_bool block {};
    auto options = make_options(clock, true);
    options.probes.ticket_present = [&](auto id) {
      if (id == 402 && block.exchange(false)) {
        gate.hook()();
        return false;
      }
      return true;
    };
    coordinator_t coordinator { std::move(options) };
    ASSERT_TRUE(start_ready(coordinator).completed());
    const auto owner = coordinator.admit_launch({ 401, "a" });
    ASSERT_TRUE(owner);
    ASSERT_TRUE(prepare_and_bind(coordinator, *owner.token));
    const auto join = coordinator.admit_managed_join({ 402, "b" });
    ASSERT_TRUE(join);
    ASSERT_TRUE(coordinator.bind_launch(*join.token));
    ASSERT_TRUE(coordinator.end_managed_launch(*owner.token));
    std::future<void> probing;
    const finally_t release { [&]() { gate.release(); } };
    block = true;
    probing = std::async(std::launch::async, [&]() { coordinator.tick(); });
    ASSERT_TRUE(gate.wait_started());
    ASSERT_TRUE(coordinator.launch_activated(*join.token));
    gate.release();
    probing.get();
    ASSERT_TRUE(coordinator.status().lease);
    EXPECT_EQ(coordinator.status().lease->attachments, 1u);
    EXPECT_EQ(coordinator.status().native_restores_posted, 0u);
  }

  TEST(DisplayTopologyCoordinator, ManagedJoinBindExpiresOnlyItsOwnUnpublishedReference) {
    fake_clock_t clock;
    coordinator_t coordinator { make_options(clock, true) };
    ASSERT_TRUE(start_ready(coordinator).completed());
    const auto owner = coordinator.admit_launch({ 401, "a" });
    ASSERT_TRUE(owner);
    ASSERT_TRUE(prepare_and_bind(coordinator, *owner.token));
    const auto expired = coordinator.admit_managed_join({ 402, "b" });
    ASSERT_TRUE(expired);
    clock.advance(500ms);
    const auto current = coordinator.admit_managed_join({ 403, "c" });
    ASSERT_TRUE(current);
    clock.advance(500ms);
    auto forged = *expired.token;
    forged.setup_deadline = monotonic_time_t::max();
    EXPECT_FALSE(coordinator.bind_launch(forged));
    EXPECT_TRUE(coordinator.bind_launch(*current.token));
    ASSERT_TRUE(coordinator.status().lease);
    EXPECT_EQ(coordinator.status().lease->attachments, 2u);
    EXPECT_EQ(coordinator.status().native_restores_posted, 0u);
  }

  TEST(DisplayTopologyCoordinator, BoundManagedJoinProbeCannotRemoveReusedLaunchId) {
    fake_clock_t clock;
    gate_t gate;
    std::atomic_bool block {};
    auto options = make_options(clock, true);
    options.probes.ticket_present = [&](auto id) {
      if (id == 402 && block.exchange(false)) {
        gate.hook()();
        return false;
      }
      return true;
    };
    coordinator_t coordinator { std::move(options) };
    ASSERT_TRUE(start_ready(coordinator).completed());
    const auto owner = coordinator.admit_launch({ 401, "a" });
    ASSERT_TRUE(owner);
    ASSERT_TRUE(prepare_and_bind(coordinator, *owner.token));
    const auto old = coordinator.admit_managed_join({ 402, "b" });
    ASSERT_TRUE(old);
    ASSERT_TRUE(coordinator.bind_launch(*old.token));
    std::future<void> probing;
    const finally_t release { [&]() { gate.release(); } };
    block = true;
    probing = std::async(std::launch::async, [&]() { coordinator.tick(); });
    ASSERT_TRUE(gate.wait_started());
    ASSERT_TRUE(coordinator.end_managed_launch(*old.token));
    const auto next = coordinator.admit_managed_join({ 402, "b" });
    ASSERT_TRUE(next);
    ASSERT_TRUE(coordinator.bind_launch(*next.token));
    gate.release();
    probing.get();
    ASSERT_TRUE(coordinator.status().lease);
    EXPECT_EQ(coordinator.status().lease->attachments, 2u);
    EXPECT_TRUE(coordinator.launch_activated(*next.token));
  }

  TEST(DisplayTopologyCoordinator, RetiringManagedLeaseWaitsForQuiescenceAndRefusesNewJoins) {
    fake_clock_t clock;
    std::atomic<std::size_t> streams { 1 };
    auto options = make_options(clock, true);
    options.probes.active_streams = [&]() { return streams.load(); };
    coordinator_t coordinator { std::move(options) };
    ASSERT_TRUE(start_ready(coordinator).completed());
    const auto owner = coordinator.admit_launch({ 401, "a" });
    ASSERT_TRUE(owner);
    ASSERT_TRUE(prepare_and_bind(coordinator, *owner.token));
    ASSERT_TRUE(coordinator.launch_activated(*owner.token));
    ASSERT_TRUE(coordinator.end_managed_launch(*owner.token));
    ASSERT_TRUE(coordinator.status().lease);
    EXPECT_TRUE(coordinator.status().lease->retiring);
    EXPECT_EQ(coordinator.status().lease->attachments, 0u);
    EXPECT_FALSE(coordinator.admit_managed_join({ 402, "b" }));
    coordinator.tick();
    EXPECT_TRUE(coordinator.status().lease);
    streams = 0;
    coordinator.tick();
    EXPECT_FALSE(coordinator.status().lease) << "terminal completion does not wait for idle grace";
    EXPECT_EQ(coordinator.status().native_restores_posted, 1u);
  }

  TEST(DisplayTopologyCoordinator, PreparingAndBoundManagedJoinsBlockIdleGrace) {
    fake_clock_t clock;
    auto options = make_options(clock, true);
    options.probes.active_streams = []() { return 0u; };
    options.probes.ticket_present = [](auto) { return true; };
    coordinator_t coordinator { std::move(options) };
    ASSERT_TRUE(start_ready(coordinator).completed());
    const auto owner = coordinator.admit_launch({ 401, "a" });
    ASSERT_TRUE(owner);
    ASSERT_TRUE(prepare_and_bind(coordinator, *owner.token));
    ASSERT_TRUE(coordinator.launch_activated(*owner.token));
    coordinator.tick();
    clock.advance(500ms);
    const auto join = coordinator.admit_managed_join({ 402, "b" });
    ASSERT_TRUE(join);
    coordinator.tick();
    clock.advance(999ms);
    coordinator.tick();
    ASSERT_TRUE(coordinator.status().lease);
    ASSERT_TRUE(coordinator.bind_launch(*join.token));
    clock.advance(10000ms);
    coordinator.tick();
    EXPECT_TRUE(coordinator.status().lease);
    EXPECT_EQ(coordinator.status().native_restores_posted, 0u);
  }

  TEST(DisplayTopologyCoordinator, ManagedIdleProbeRevalidatesReferenceActivityBeforeStartingGrace) {
    fake_clock_t clock;
    gate_t gate;
    std::atomic_bool block {};
    std::atomic<std::size_t> streams { 1 };
    auto options = make_options(clock, true);
    options.probes.active_streams = [&]() {
      if (block.exchange(false)) {
        gate.hook()();
        return std::size_t { 0 };
      }
      return streams.load();
    };
    coordinator_t coordinator { std::move(options) };
    ASSERT_TRUE(start_ready(coordinator).completed());
    const auto owner = coordinator.admit_launch({ 401, "a" });
    ASSERT_TRUE(owner);
    ASSERT_TRUE(prepare_and_bind(coordinator, *owner.token));
    ASSERT_TRUE(coordinator.launch_activated(*owner.token));
    std::future<void> probing;
    const finally_t release { [&]() { gate.release(); } };
    block = true;
    probing = std::async(std::launch::async, [&]() { coordinator.tick(); });
    ASSERT_TRUE(gate.wait_started());
    const auto join = coordinator.admit_managed_join({ 402, "b" });
    ASSERT_TRUE(join);
    ASSERT_TRUE(coordinator.bind_launch(*join.token));
    ASSERT_TRUE(coordinator.launch_activated(*join.token));
    gate.release();
    probing.get();
    clock.advance(100ms);
    streams = 0;
    coordinator.tick();
    clock.advance(400ms);
    coordinator.tick();
    EXPECT_TRUE(coordinator.status().lease) << "a stale zero answer started the grace period early";
    clock.advance(100ms);
    coordinator.tick();
    EXPECT_FALSE(coordinator.status().lease);
  }

  TEST(DisplayTopologyCoordinator, ManagedTicketAbandonmentRetainsOtherReferences) {
    fake_clock_t clock;
    std::set<std::uint32_t> tickets { 401, 402 };
    auto options = make_options(clock, true);
    options.probes.ticket_present = [&](auto id) { return tickets.contains(id); };
    coordinator_t coordinator { std::move(options) };
    ASSERT_TRUE(start_ready(coordinator).completed());
    const auto owner = coordinator.admit_launch({ 401, "a" });
    ASSERT_TRUE(owner);
    ASSERT_TRUE(prepare_and_bind(coordinator, *owner.token));
    const auto join = coordinator.admit_managed_join({ 402, "b" });
    ASSERT_TRUE(join);
    ASSERT_TRUE(coordinator.bind_launch(*join.token));
    tickets.erase(401);
    coordinator.tick();
    ASSERT_TRUE(coordinator.status().lease);
    EXPECT_EQ(coordinator.status().lease->attachments, 1u);
    EXPECT_EQ(coordinator.status().lease->launch_session_id, 401u);
    EXPECT_EQ(coordinator.admit_launch({ 403, "c" }).reason, reject_reason_e::owner_busy);
    tickets.erase(402);
    coordinator.tick();
    EXPECT_FALSE(coordinator.status().lease);
    EXPECT_EQ(coordinator.status().native_restores_posted, 1u);
  }

  TEST(DisplayTopologyCoordinator, RetiringManagedLeaseSurvivesAThrowingQuiescenceProbe) {
    fake_clock_t clock;
    bool fail = true;
    auto options = make_options(clock, true);
    options.probes.active_streams = [&]() {
      if (fail) throw std::runtime_error { "unavailable stream count" };
      return 0u;
    };
    coordinator_t coordinator { std::move(options) };
    ASSERT_TRUE(start_ready(coordinator).completed());
    const auto owner = coordinator.admit_launch({ 401, "a" });
    ASSERT_TRUE(owner);
    ASSERT_TRUE(prepare_and_bind(coordinator, *owner.token));
    EXPECT_THROW(coordinator.end_managed_launch(*owner.token), std::runtime_error);
    ASSERT_TRUE(coordinator.status().lease);
    EXPECT_TRUE(coordinator.status().lease->retiring);
    fail = false;
    coordinator.tick();
    EXPECT_FALSE(coordinator.status().lease);
    EXPECT_EQ(coordinator.status().native_restores_posted, 1u);
  }
  TEST(DisplayTopologyCoordinator, ManagedEndReceiptFollowsTheDeferredActualRestore) {
    fake_clock_t clock;
    gate_t restore;
    std::atomic<std::size_t> streams { 1 };
    auto options = make_options(clock, true);
    options.probes.active_streams = [&]() { return streams.load(); };
    options.native_restore = restore.body();
    coordinator_t coordinator { std::move(options) };
    const finally_t release { [&]() { restore.release(); } };
    ASSERT_TRUE(start_ready(coordinator).completed());
    const auto owner = coordinator.admit_launch({ 501, "a" });
    ASSERT_TRUE(owner);
    ASSERT_TRUE(prepare_and_bind(coordinator, *owner.token));
    const auto join = coordinator.admit_managed_join({ 502, "b" });
    ASSERT_TRUE(join);
    ASSERT_TRUE(coordinator.bind_launch(*join.token));
    std::promise<command_result_t> unrelated;
    auto receipt = unrelated.get_future().share();
    EXPECT_TRUE(coordinator.end_managed_launch(*owner.token, command_kind_e::stream_restore, &receipt));
    ASSERT_FALSE(receipt.valid()) << "non-final teardown must not wait on other streams";
    ASSERT_TRUE(coordinator.end_managed_launch(*join.token, command_kind_e::stream_restore, &receipt));
    ASSERT_TRUE(receipt.valid());
    EXPECT_EQ(receipt.wait_for(0ms), std::future_status::timeout);
    coordinator.tick();
    EXPECT_EQ(receipt.wait_for(0ms), std::future_status::timeout);
    streams = 0;
    coordinator.tick();
    ASSERT_TRUE(restore.wait_started());
    EXPECT_EQ(receipt.wait_for(0ms), std::future_status::timeout) << "submission is not restoration completion";
    restore.release();
    ASSERT_EQ(receipt.wait_for(real_timeout), std::future_status::ready);
    EXPECT_TRUE(receipt.get().completed());
  }

  TEST(DisplayTopologyCoordinator, ManagedEndProbeFailureStillHandsOffItsRestoreReceipt) {
    fake_clock_t clock;
    bool fail = true;
    auto options = make_options(clock, true);
    options.probes.active_streams = [&]() {
      if (fail) throw std::runtime_error { "unavailable stream count" };
      return 0u;
    };
    coordinator_t coordinator { std::move(options) };
    ASSERT_TRUE(start_ready(coordinator).completed());
    const auto owner = coordinator.admit_launch({ 501, "a" });
    ASSERT_TRUE(owner);
    ASSERT_TRUE(prepare_and_bind(coordinator, *owner.token));
    std::shared_future<command_result_t> receipt;
    EXPECT_THROW(coordinator.end_managed_launch(*owner.token, command_kind_e::stream_restore, &receipt), std::runtime_error);
    ASSERT_TRUE(receipt.valid()) << "the consumed token cannot be retried to recover a lost receipt";
    EXPECT_EQ(receipt.wait_for(0ms), std::future_status::timeout);
    fail = false;
    coordinator.tick();
    ASSERT_EQ(receipt.wait_for(real_timeout), std::future_status::ready);
    EXPECT_TRUE(receipt.get().completed());
  }

  TEST(DisplayTopologyCoordinator, ManagedRestoreReceiptExposesBodyAndObserverFailures) {
    for (const bool body_fails : { false, true }) {
      fake_clock_t clock;
      auto options = make_options(clock, true);
      options.native_restore = [&](const auto &, const auto &) {
        if (body_fails) throw std::runtime_error { "restore failed" };
      };
      options.posted_observer = [&](auto, const auto &) {
        if (!body_fails) throw std::runtime_error { "observer failed" };
      };
      coordinator_t coordinator { std::move(options) };
      ASSERT_TRUE(start_ready(coordinator).completed());
      const auto owner = coordinator.admit_launch({ 501, "a" });
      ASSERT_TRUE(owner);
      ASSERT_TRUE(prepare_and_bind(coordinator, *owner.token));
      std::shared_future<command_result_t> receipt;
      ASSERT_TRUE(coordinator.end_managed_launch(*owner.token, command_kind_e::stream_restore, &receipt));
      ASSERT_TRUE(receipt.valid());
      ASSERT_EQ(receipt.wait_for(real_timeout), std::future_status::ready);
      EXPECT_EQ(receipt.get().outcome, outcome_e::failed);
      ASSERT_TRUE(receipt.get().exception);
      EXPECT_THROW(std::rethrow_exception(receipt.get().exception), std::runtime_error);
      EXPECT_EQ(coordinator.status().native.in_conflict, body_fails);
    }
  }

  TEST(DisplayTopologyCoordinator, StopCompletesAnUnsubmittedManagedRestoreReceipt) {
    fake_clock_t clock;
    auto options = make_options(clock, true);
    options.probes.active_streams = []() { return 1u; };
    coordinator_t coordinator { std::move(options) };
    ASSERT_TRUE(start_ready(coordinator).completed());
    const auto owner = coordinator.admit_launch({ 501, "a" });
    ASSERT_TRUE(owner);
    ASSERT_TRUE(prepare_and_bind(coordinator, *owner.token));
    std::shared_future<command_result_t> receipt;
    ASSERT_TRUE(coordinator.end_managed_launch(*owner.token, command_kind_e::stream_restore, &receipt));
    ASSERT_TRUE(receipt.valid());
    EXPECT_EQ(receipt.wait_for(0ms), std::future_status::timeout);
    coordinator.stop();
    ASSERT_EQ(receipt.wait_for(real_timeout), std::future_status::ready);
    EXPECT_EQ(receipt.get().outcome, outcome_e::stopped);
    EXPECT_EQ(coordinator.status().native_restores_posted, 0u);
  }

  TEST(DisplayTopologyCoordinator, StopSkippedManagedRestoreResolvesTheSameReceipt) {
    fake_clock_t clock;
    gate_t gate;
    std::atomic_int restores {};
    auto options = make_options(clock, true);
    options.native_restore = counting(restores);
    coordinator_t coordinator { std::move(options) };
    std::future<void> stopping;
    const finally_t release { [&]() { gate.release(); } };
    ASSERT_TRUE(start_ready(coordinator).completed());
    const auto owner = coordinator.admit_launch({ 501, "a" });
    ASSERT_TRUE(owner);
    ASSERT_TRUE(prepare_and_bind(coordinator, *owner.token));
    const auto blocker = coordinator.post_tracked(command_kind_e::stream_restore, gate.body());
    ASSERT_TRUE(gate.wait_started());
    std::shared_future<command_result_t> receipt;
    ASSERT_TRUE(coordinator.end_managed_launch(*owner.token, command_kind_e::stream_restore, &receipt));
    ASSERT_TRUE(receipt.valid());
    stopping = std::async(std::launch::async, [&]() { coordinator.stop(); });
    ASSERT_TRUE(wait_until([&]() { return coordinator.status().phase == coordinator_phase_e::stopped; }));
    gate.release();
    stopping.get();
    ASSERT_EQ(receipt.wait_for(real_timeout), std::future_status::ready);
    EXPECT_EQ(receipt.get().outcome, outcome_e::stopped);
    EXPECT_EQ(restores, 0);
  }

  TEST(DisplayTopologyCoordinator, StopCannotCompleteARunningManagedRestoreReceiptEarly) {
    fake_clock_t clock;
    gate_t gate;
    auto options = make_options(clock, true);
    options.native_restore = gate.body();
    coordinator_t coordinator { std::move(options) };
    std::future<void> stopping;
    const finally_t release { [&]() { gate.release(); } };
    ASSERT_TRUE(start_ready(coordinator).completed());
    const auto owner = coordinator.admit_launch({ 501, "a" });
    ASSERT_TRUE(owner);
    ASSERT_TRUE(prepare_and_bind(coordinator, *owner.token));
    std::shared_future<command_result_t> receipt;
    ASSERT_TRUE(coordinator.end_managed_launch(*owner.token, command_kind_e::stream_restore, &receipt));
    ASSERT_TRUE(receipt.valid());
    ASSERT_TRUE(gate.wait_started());
    stopping = std::async(std::launch::async, [&]() { coordinator.stop(); });
    ASSERT_TRUE(wait_until([&]() { return coordinator.status().phase == coordinator_phase_e::stopped; }));
    EXPECT_EQ(receipt.wait_for(0ms), std::future_status::timeout);
    gate.release();
    stopping.get();
    ASSERT_EQ(receipt.wait_for(real_timeout), std::future_status::ready);
    EXPECT_TRUE(receipt.get().body_started());
  }

  TEST(DisplayTopologyCoordinator, ManagedRestoreReceiptWaitsForItsObserver) {
    fake_clock_t clock;
    gate_t gate;
    auto options = make_options(clock, true);
    options.posted_observer = [hold = gate.hook()](auto, const auto &) { hold(); };
    coordinator_t coordinator { std::move(options) };
    const finally_t release { [&]() { gate.release(); } };
    ASSERT_TRUE(start_ready(coordinator).completed());
    const auto owner = coordinator.admit_launch({ 501, "a" });
    ASSERT_TRUE(owner);
    ASSERT_TRUE(prepare_and_bind(coordinator, *owner.token));
    std::shared_future<command_result_t> receipt;
    ASSERT_TRUE(coordinator.end_managed_launch(*owner.token, command_kind_e::stream_restore, &receipt));
    ASSERT_TRUE(receipt.valid());
    ASSERT_TRUE(gate.wait_started());
    EXPECT_EQ(receipt.wait_for(0ms), std::future_status::timeout);
    gate.release();
    ASSERT_EQ(receipt.wait_for(real_timeout), std::future_status::ready);
    EXPECT_TRUE(receipt.get().completed());
  }

  // ---------------------------------------------------------------------------
  // run_for_value: the HTTP rotate/scale handlers' route
  // ---------------------------------------------------------------------------

  TEST(DisplayTopologyCall, ReturnsTheBodyValueFromAScopedCapabilityOnTheWorker) {
    fake_clock_t clock;
    coordinator_t coordinator { make_options(clock) };
    ASSERT_TRUE(start_ready(coordinator).completed());

    const auto value = run_for_value(coordinator, command_kind_e::rotate, [&](const mutation_capability_t &capability) {
      capability.require(operation_e::rotate);
      return coordinator.on_worker_thread() ? 90 : -1;
    });

    ASSERT_TRUE(value.has_value()) << value.error().detail;
    EXPECT_EQ(*value, 90);
  }

  TEST(DisplayTopologyCall, ARefusedCallNeverRunsTheBodyAndNamesWhy) {
    fake_clock_t clock;
    coordinator_t coordinator { make_options(clock) };
    std::atomic_bool ran {};

    const auto value = run_for_value(coordinator, command_kind_e::scale, [&](const mutation_capability_t &) {
      ran = true;
      return true;
    });

    ASSERT_FALSE(value.has_value());
    EXPECT_FALSE(ran);
    EXPECT_EQ(value.error().result.outcome, outcome_e::rejected);
    EXPECT_EQ(value.error().detail, std::string { "rejected / " } + std::string { to_string(reject_reason_e::not_started) });
  }

  TEST(DisplayTopologyCall, ABodyExceptionReachesTheCaller) {
    fake_clock_t clock;
    coordinator_t coordinator { make_options(clock) };
    ASSERT_TRUE(start_ready(coordinator).completed());

    EXPECT_THROW(run_for_value(coordinator, command_kind_e::scale, [](const mutation_capability_t &capability) -> bool {
      capability.require(operation_e::vdd_destroy);
      return true;
    }),
      capability_error_t);
    EXPECT_THROW(run_for_value(coordinator, command_kind_e::rotate, [](const mutation_capability_t &) -> bool {
      throw std::runtime_error { "CCD failed" };
    }),
      std::runtime_error);
  }

  TEST(DisplayTopologyCall, AReentrantCallFromTheWorkerIsRefusedWithoutRunning) {
    fake_clock_t clock;
    coordinator_t coordinator { make_options(clock) };
    ASSERT_TRUE(start_ready(coordinator).completed());
    std::atomic_bool inner_ran {};
    std::optional<call_refused_t> refusal;

    const auto outer = coordinator.run(command_kind_e::rotate, [&](const mutation_capability_t &, const command_context_t &) {
      auto inner = run_for_value(coordinator, command_kind_e::scale, [&](const mutation_capability_t &) {
        inner_ran = true;
        return 0;
      });
      if (!inner) {
        refusal = inner.error();
      }
    });

    EXPECT_TRUE(outer.completed());
    EXPECT_FALSE(inner_ran);
    ASSERT_TRUE(refusal);
    EXPECT_EQ(refusal->result.outcome, outcome_e::reentrant);
    EXPECT_EQ(refusal->detail, "reentrant");
  }

  TEST(DisplayTopologyCall, ARunningBodyOutlivesStopAndItsValueIsStillReturned) {
    fake_clock_t clock;
    coordinator_t coordinator { make_options(clock) };
    ASSERT_TRUE(start_ready(coordinator).completed());
    gate_t gate;
    const finally_t release { [&] { gate.release(); } };
    auto call = std::async(std::launch::async, [&, hold = gate.hook()] {
      return run_for_value(coordinator, command_kind_e::rotate, [&](const mutation_capability_t &) {
        hold();
        return 7;
      });
    });
    ASSERT_TRUE(gate.wait_started());
    auto stopping = std::async(std::launch::async, [&] { coordinator.stop(); });
    ASSERT_TRUE(wait_until([&] { return coordinator.status().phase == coordinator_phase_e::stopped; }));

    EXPECT_EQ(call.wait_for(50ms), std::future_status::timeout) << "returned while its body still held references";
    gate.release();
    stopping.get();
    const auto value = call.get();
    ASSERT_TRUE(value.has_value()) << value.error().detail;
    EXPECT_EQ(*value, 7);
  }

  TEST(DisplayTopologyCall, ACallQueuedWhenStopArrivesIsRefusedWithoutRunning) {
    fake_clock_t clock;
    coordinator_t coordinator { make_options(clock) };
    ASSERT_TRUE(start_ready(coordinator).completed());
    gate_t busy;
    const finally_t release { [&] { busy.release(); } };
    std::atomic_bool ran {};
    ASSERT_EQ(coordinator.post(command_kind_e::rotate, busy.body()), reject_reason_e::none);
    ASSERT_TRUE(busy.wait_started());
    auto call = std::async(std::launch::async, [&] {
      return run_for_value(coordinator, command_kind_e::scale, [&](const mutation_capability_t &) {
        ran = true;
        return 0;
      });
    });
    ASSERT_EQ(call.wait_for(50ms), std::future_status::timeout);
    auto stopping = std::async(std::launch::async, [&] { coordinator.stop(); });
    ASSERT_TRUE(wait_until([&] { return coordinator.status().phase == coordinator_phase_e::stopped; }));
    busy.release();
    stopping.get();

    const auto value = call.get();
    ASSERT_FALSE(value.has_value());
    EXPECT_FALSE(ran);
    EXPECT_EQ(value.error().result.outcome, outcome_e::stopped);
  }

  // ---------------------------------------------------------------------------
  // run_toggle: the hotkey/tray VDD toggle
  // ---------------------------------------------------------------------------

  struct toggle_probe_t {
    std::atomic_int prepared {};
    std::atomic_int released {};
    std::atomic_int applied {};
    std::atomic_bool applied_on_worker {};
    std::atomic_bool rotate_refused {};
    std::atomic_bool topology_write_refused {};
    std::atomic_int last_follow_up { -1 };
    std::atomic_int followed {};
    std::atomic_bool follow_up_on_worker {};
    bool decline {};
    bool throw_in_apply {};
    bool stale {};

    toggle_steps_t<int, int>
    steps(coordinator_t &coordinator) {
      return {
        [this]() -> std::optional<int> {
          ++prepared;
          return decline ? std::nullopt : std::optional<int> { 41 };
        },
        [this](const int &request) {
          EXPECT_EQ(request, 41);
          ++released;
        },
        [this, &coordinator](const mutation_capability_t &capability, const int &request) {
          capability.require(operation_e::vdd_create);
          capability.require(operation_e::vdd_destroy);
          try {
            capability.require(operation_e::rotate);
          }
          catch (const capability_error_t &) {
            rotate_refused = true;
          }
          try {
            capability.require(operation_e::write_topology);
          }
          catch (const capability_error_t &) {
            topology_write_refused = true;
          }
          applied_on_worker = coordinator.on_worker_thread();
          ++applied;
          if (throw_in_apply) {
            throw std::runtime_error { "IOCTL failed" };
          }
          return stale ? request : request + 1;
        },
        [](const int &result) { return result == 42; },
        [this, &coordinator](const int &result) {
          last_follow_up = result;
          follow_up_on_worker = coordinator.on_worker_thread();
          ++followed;
        },
      };
    }
  };

  TEST(DisplayTopologyToggle, AppliesOnTheWorkerThenFollowsUpOnTheCaller) {
    fake_clock_t clock;
    coordinator_t coordinator { make_options(clock) };
    ASSERT_TRUE(start_ready(coordinator).completed());
    toggle_probe_t probe;

    EXPECT_TRUE(run_toggle(coordinator, probe.steps(coordinator)));
    EXPECT_EQ(probe.applied.load(), 1);
    EXPECT_TRUE(probe.applied_on_worker);
    EXPECT_TRUE(probe.rotate_refused) << "a toggle capability must not allow rotation";
    EXPECT_TRUE(probe.topology_write_refused) << "extending the new VDD is the follow-up's job, not the toggle's";
    EXPECT_EQ(probe.last_follow_up.load(), 42);
    EXPECT_EQ(probe.followed.load(), 1);
    EXPECT_FALSE(probe.follow_up_on_worker);
    EXPECT_EQ(probe.released.load(), 0);
  }

  TEST(DisplayTopologyToggle, ADeclinedPromptSubmitsNothing) {
    fake_clock_t clock;
    coordinator_t coordinator { make_options(clock) };
    ASSERT_TRUE(start_ready(coordinator).completed());
    toggle_probe_t probe;
    probe.decline = true;
    const auto submitted = coordinator.status().submitted_commands;

    EXPECT_FALSE(run_toggle(coordinator, probe.steps(coordinator)));
    EXPECT_EQ(probe.applied.load(), 0);
    EXPECT_EQ(probe.followed.load(), 0);
    EXPECT_EQ(probe.released.load(), 0);
    EXPECT_EQ(coordinator.status().submitted_commands, submitted);
  }

  TEST(DisplayTopologyToggle, ARefusedToggleReleasesItsReservationAndNeverApplies) {
    fake_clock_t clock;
    coordinator_t coordinator { make_options(clock) };
    toggle_probe_t probe;

    EXPECT_FALSE(run_toggle(coordinator, probe.steps(coordinator)));
    EXPECT_EQ(probe.prepared.load(), 1);
    EXPECT_EQ(probe.applied.load(), 0);
    EXPECT_EQ(probe.released.load(), 1);
    EXPECT_EQ(probe.followed.load(), 0);
  }

  TEST(DisplayTopologyToggle, AFailedApplyPropagatesAndKeepsTheReservation) {
    fake_clock_t clock;
    coordinator_t coordinator { make_options(clock) };
    ASSERT_TRUE(start_ready(coordinator).completed());
    toggle_probe_t probe;
    probe.throw_in_apply = true;

    EXPECT_THROW(run_toggle(coordinator, probe.steps(coordinator)), std::runtime_error);
    EXPECT_EQ(probe.applied.load(), 1);
    EXPECT_EQ(probe.released.load(), 0);
    EXPECT_EQ(probe.followed.load(), 0);
  }

  TEST(DisplayTopologyToggle, AStaleApplyRanButReportsNotApplied) {
    fake_clock_t clock;
    coordinator_t coordinator { make_options(clock) };
    ASSERT_TRUE(start_ready(coordinator).completed());
    toggle_probe_t probe;
    probe.stale = true;

    EXPECT_FALSE(run_toggle(coordinator, probe.steps(coordinator)));
    EXPECT_EQ(probe.applied.load(), 1);
    EXPECT_EQ(probe.released.load(), 0) << "the command ran, so its reservation stays";
    EXPECT_EQ(probe.last_follow_up.load(), 41) << "the follow-up decides for itself what a stale result needs";
  }
}  // namespace
