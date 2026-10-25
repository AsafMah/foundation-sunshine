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
    const std::map<kind, std::set<op>> legacy {
      { kind::startup_recovery, { op::native_recover, op::restore_state } },
      { kind::launch_prepare, { op::configure_display, op::restore_state, op::native_apply } },
      { kind::launch_failed_restore, { op::restore_state, op::native_restore } },
      { kind::reconfigure, { op::configure_display } },
      { kind::stream_restore, { op::restore_state, op::native_restore } },
      { kind::shutdown_restore, { op::restore_state, op::native_restore } },
      { kind::shutdown_vdd_destroy, { op::vdd_destroy } },
      { kind::rotate, { op::rotate } },
      { kind::scale, { op::scale } },
      { kind::vdd_toggle, { op::vdd_create, op::vdd_destroy } },
      { kind::vdd_toggle_followup, { op::vdd_extended_mode, op::vdd_destroy } },
      { kind::vdd_create, { op::vdd_create } },
      { kind::vdd_destroy, { op::vdd_destroy } },
      { kind::reset_persistence, { op::reset_persistence } },
      { kind::legacy_retry_tick, { op::legacy_deferred } },
      { kind::legacy_unlock_restore, { op::legacy_deferred } },
      { kind::native_restore, { op::native_restore } },
    };
    // Managed mode: the legacy whole-state operations survive only in startup recovery.
    const std::map<kind, std::set<op>> managed {
      { kind::startup_recovery, { op::native_recover, op::restore_state } },
      { kind::launch_prepare, { op::native_apply } },
      { kind::launch_failed_restore, { op::native_restore } },
      { kind::reconfigure, {} },
      { kind::stream_restore, { op::native_restore } },
      { kind::shutdown_restore, { op::native_restore } },
      { kind::shutdown_vdd_destroy, { op::vdd_destroy } },
      { kind::rotate, { op::rotate } },
      { kind::scale, { op::scale } },
      { kind::vdd_toggle, { op::vdd_create, op::vdd_destroy } },
      { kind::vdd_toggle_followup, { op::vdd_extended_mode, op::vdd_destroy } },
      { kind::vdd_create, { op::vdd_create } },
      { kind::vdd_destroy, { op::vdd_destroy } },
      { kind::reset_persistence, { op::reset_persistence } },
      { kind::legacy_retry_tick, { op::legacy_deferred } },
      { kind::legacy_unlock_restore, { op::legacy_deferred } },
      { kind::native_restore, { op::native_restore } },
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
    coordinator.launch_activated(14);
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
      legacy.launch_activated(31);
      ASSERT_TRUE(legacy.status().lease);
      EXPECT_EQ(legacy.status().lease->stage, lease_stage_e::preparing);
      ASSERT_TRUE(legacy.bind_launch(*before_bind.token));
      EXPECT_FALSE(legacy.status().lease) << "activation before bind must end the legacy lease at bind";

      const auto after_bind = legacy.admit_launch({ 32, "cert:a" });
      ASSERT_TRUE(after_bind);
      ASSERT_TRUE(prepare_and_bind(legacy, *after_bind.token));
      legacy.launch_activated(999);
      ASSERT_TRUE(legacy.status().lease);
      EXPECT_EQ(legacy.status().lease->stage, lease_stage_e::bound);
      legacy.launch_activated(32);
      EXPECT_FALSE(legacy.status().lease);
    }
    {
      coordinator_t managed { make_options(clock, true) };
      ASSERT_TRUE(start_ready(managed).completed());

      const auto before_bind = managed.admit_launch({ 33, "cert:a" });
      ASSERT_TRUE(before_bind);
      ASSERT_TRUE(managed.run_launch_prepare(*before_bind.token, {}).completed());
      managed.launch_activated(33);
      ASSERT_TRUE(managed.bind_launch(*before_bind.token));
      ASSERT_TRUE(managed.status().lease);
      EXPECT_EQ(managed.status().lease->stage, lease_stage_e::active);

      ASSERT_TRUE(managed.end_managed_lease(command_kind_e::stream_restore));
      const auto after_bind = managed.admit_launch({ 34, "cert:a" });
      ASSERT_TRUE(after_bind);
      ASSERT_TRUE(prepare_and_bind(managed, *after_bind.token));
      ASSERT_TRUE(managed.status().lease);
      EXPECT_EQ(managed.status().lease->stage, lease_stage_e::bound);
      managed.launch_activated(34);
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
    coordinator.launch_activated(241);
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
    coordinator.launch_activated(291);
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
      if (command == command_kind_e::startup_recovery || command == command_kind_e::launch_prepare) {
        continue;
      }
      const auto expected = expected_cleanup_kinds.contains(command) ? reject_reason_e::none : reject_reason_e::managed_session;
      EXPECT_EQ(coordinator.post(command, counting(calls[command])), expected) << to_string(command);
    }
    ASSERT_TRUE(flush(coordinator).completed());
    for (const auto command : all_kinds) {
      if (command == command_kind_e::startup_recovery || command == command_kind_e::launch_prepare) {
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
      if (command == command_kind_e::startup_recovery || command == command_kind_e::launch_prepare) {
        continue;
      }
      EXPECT_EQ(coordinator.post(command, counting(calls)), reject_reason_e::none) << to_string(command);
    }
    ASSERT_TRUE(flush(coordinator).completed());
    EXPECT_EQ(calls.load(), static_cast<int>(all_kinds.size()) - 2);
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

      coordinator.launch_activated(311);
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
    coordinator.launch_activated(111);
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
      EXPECT_FALSE(managed.end_managed_lease(command_kind_e::stream_restore)) << "a preparing lease ends through abort_launch";
      ASSERT_TRUE(prepare_and_bind(managed, *launch.token));
      managed.launch_activated(121);
      EXPECT_THROW(static_cast<void>(managed.end_managed_lease(command_kind_e::rotate)), std::invalid_argument);
      EXPECT_TRUE(managed.end_managed_lease(command_kind_e::stream_restore));
      EXPECT_FALSE(managed.end_managed_lease(command_kind_e::stream_restore));
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
    EXPECT_FALSE(legacy.end_managed_lease(command_kind_e::stream_restore));
    EXPECT_TRUE(legacy.status().lease);
  }

  TEST(DisplayTopologyCoordinator, AStreamEndLeavesABoundManagedLeaseAlone) {
    fake_clock_t clock;
    std::atomic_int restores {};
    auto options = make_options(clock, true);
    options.native_restore = counting(restores);
    coordinator_t coordinator { std::move(options) };
    ASSERT_TRUE(start_ready(coordinator).completed());

    // The previous stream ended after this launch bound its ticket but before it connected.
    const auto launch = coordinator.admit_launch({ 211, "cert:a" });
    ASSERT_TRUE(launch);
    ASSERT_TRUE(prepare_and_bind(coordinator, *launch.token));
    EXPECT_FALSE(coordinator.end_managed_lease(command_kind_e::stream_restore)) << "a stream end released a bound lease";
    EXPECT_FALSE(coordinator.end_managed_lease(command_kind_e::stream_restore, 211)) << "a stream end released a bound lease";
    auto status = coordinator.status();
    ASSERT_TRUE(status.lease);
    EXPECT_EQ(status.lease->launch_session_id, 211u);
    EXPECT_EQ(status.lease->stage, lease_stage_e::bound);
    EXPECT_EQ(status.native_restores_posted, 0u);

    coordinator.launch_activated(211);
    EXPECT_TRUE(coordinator.end_managed_lease(command_kind_e::stream_restore));
    ASSERT_TRUE(flush(coordinator).completed());
    EXPECT_EQ(restores.load(), 1);
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
    coordinator.launch_activated(201);
    ASSERT_TRUE(coordinator.end_managed_lease(command_kind_e::stream_restore, 201));

    const auto second = coordinator.admit_launch({ 202, "cert:b" });
    ASSERT_TRUE(second);
    ASSERT_TRUE(prepare_and_bind(coordinator, *second.token));
    coordinator.launch_activated(202);
    EXPECT_FALSE(coordinator.end_managed_lease(command_kind_e::stream_restore, 201)) << "launch 201's stream end released launch 202";
    const auto status = coordinator.status();
    ASSERT_TRUE(status.lease);
    EXPECT_EQ(status.lease->launch_session_id, 202u);
    EXPECT_EQ(status.lease->stage, lease_stage_e::active);
    EXPECT_EQ(status.native_restores_posted, 1u);

    EXPECT_TRUE(coordinator.end_managed_lease(command_kind_e::stream_restore, 202));
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
    coordinator.launch_activated(271);
    blocker = std::async(std::launch::async, [&]() {
      return coordinator.run(command_kind_e::stream_restore, busy.body());
    });
    ASSERT_TRUE(busy.wait_started());
    ASSERT_TRUE(coordinator.end_managed_lease(command_kind_e::stream_restore, 271));

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
}  // namespace
