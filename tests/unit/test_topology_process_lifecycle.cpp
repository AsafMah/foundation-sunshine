/**
 * @file tests/unit/test_topology_process_lifecycle.cpp
 * @brief Startup, shutdown restore and final VDD cleanup of the process-lifetime coordinator.
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "src/display_device/topology_process_lifecycle.h"

namespace display_device::topology {
  namespace {
    using namespace std::chrono_literals;
    using kind = command_kind_e;

    constexpr std::uint64_t op = 7171;
    constexpr actor_receipt_t receipt { 5120, 133'500'000'000'000'000ULL };
    constexpr auto real_timeout = 5s;

    class temp_dir_t {
    public:
      temp_dir_t() {
        std::random_device random;
        path_ = std::filesystem::temp_directory_path() /
                ("sunshine-process-lifecycle-" + std::to_string(random()) + "-" + std::to_string(random()));
        std::filesystem::create_directories(path_);
      }

      ~temp_dir_t() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
      }

      std::filesystem::path
      file() const {
        return path_ / "actor.json";
      }

    private:
      std::filesystem::path path_;
    };

    /** Remembers the last durable transition so a transfer is distinguishable from a completion. */
    class recording_store_t final: public actor_record_store_t {
    public:
      explicit recording_store_t(std::filesystem::path path):
          inner_(std::move(path)) {}

      actor_load_t
      load() override { return inner_.load(); }

      result_t<void>
      create(const actor_record_t &record) override { return inner_.create(record); }

      result_t<void>
      replace(const actor_record_t &expected, const actor_record_t &next) override {
        auto result = inner_.replace(expected, next);
        if (result) {
          std::lock_guard lock { mutex_ };
          last_ = next;
        }
        return result;
      }

      result_t<void>
      retire(const actor_record_t &expected) override { return inner_.retire(expected); }

      std::optional<actor_record_t>
      last() const {
        std::lock_guard lock { mutex_ };
        return last_;
      }

    private:
      file_actor_record_store_t inner_;
      mutable std::mutex mutex_;
      std::optional<actor_record_t> last_;
    };

    class event_log_t {    public:
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

    private:
      mutable std::mutex mutex_;
      std::vector<std::string> events_;
    };

    actor_record_t
    intent() {
      auto record = begin_actor_intent(op, std::nullopt, actor_obligation_e::restore_legacy_display, "adapter restart");
      EXPECT_TRUE(record.has_value());
      return *record;
    }

    actor_record_t
    launched() {
      auto record = record_actor_receipt(intent(), op, receipt);
      EXPECT_TRUE(record.has_value());
      return *record;
    }

    actor_record_t
    cleanup_pending() {
      auto record = settle_actor(launched(), op);
      EXPECT_TRUE(record.has_value());
      return *record;
    }

    template<class Predicate>
    bool
    eventually(Predicate predicate) {
      const auto deadline = std::chrono::steady_clock::now() + real_timeout;
      while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(1ms);
      }
      return true;
    }

    struct fixture_t {
      temp_dir_t dir;
      std::shared_ptr<recording_store_t> store { std::make_shared<recording_store_t>(dir.file()) };
      event_log_t log;
      std::atomic<process_observation_e> observation { process_observation_e::exited };
      std::atomic_bool keep_vdd {};
      std::atomic_bool quiescent { true };
      std::atomic_bool defer_startup {};
      std::vector<std::string> reports;
      std::mutex reports_mutex;
      std::vector<std::thread> watchers;
      std::mutex watchers_mutex;
      std::vector<kind> kinds;
      std::mutex kinds_mutex;
      std::function<void()> startup_extra;
      std::unique_ptr<process_lifecycle_t> lifecycle;
      native_health_t native_initial;
      native_health_t native_recovered;
      native_health_t native_restored;
      native_health_t native_reset;
      std::function<void()> native_recover_extra;
      std::function<void()> native_restore_extra;
      native_prepared_t native_prepared { managed_target_t { "{zako-head}", true }, {} };
      std::function<void()> native_prepare_extra;

      ~fixture_t() {
        observation = process_observation_e::exited;
        if (lifecycle) lifecycle->coordinator().stop();
        std::lock_guard lock { watchers_mutex };
        for (auto &watcher : watchers) watcher.join();
      }

      void
      saw(kind value, std::string event) {
        {
          std::lock_guard lock { kinds_mutex };
          kinds.push_back(value);
        }
        log.add(std::move(event));
      }

      std::vector<kind>
      seen_kinds() {
        std::lock_guard lock { kinds_mutex };
        return kinds;
      }

      std::vector<std::string>
      reported() {
        std::lock_guard lock { reports_mutex };
        return reports;
      }

      bool
      reported_containing(const std::string &needle) {
        for (const auto &line : reported()) {
          if (line.find(needle) != std::string::npos) return true;
        }
        return false;
      }

      coordinator_options_t
      options() {
        coordinator_options_t result;
        result.probes.startup_restore_quiescent = [this]() { return quiescent.load(); };
        return result;
      }

      lifecycle_hooks_t
      hooks() {
        lifecycle_hooks_t result;
        result.startup_restore = [this](const mutation_capability_t &capability, const command_context_t &) {
          saw(capability.kind(), "startup");
          if (startup_extra) startup_extra();
          if (defer_startup) {
            lifecycle->coordinator().defer_startup_restore(capability, [this](const auto &, const auto &) { log.add("deferred"); });
          }
        };
        result.shutdown_restore = [this](const mutation_capability_t &capability, const command_context_t &) {
          capability.require(operation_e::restore_state);
          saw(capability.kind(), "shutdown_restore");
        };
        result.quiet_vdd_destroy = [this](const mutation_capability_t &capability, const command_context_t &) {
          capability.require(operation_e::vdd_destroy);
          saw(capability.kind(), "vdd_destroy");
        };
        result.keep_vdd = [this]() { return keep_vdd.load(); };
        result.actor.store = store;
        result.actor.observe = [this](const actor_receipt_t &seen) {
          EXPECT_EQ(seen, receipt);
          return observation.load();
        };
        result.actor.spawn = [this](std::function<void()> work) {
          std::lock_guard lock { watchers_mutex };
          watchers.emplace_back(std::move(work));
        };
        result.actor.poll_interval = 1ms;
        result.report = [this](const std::string &line) {
          std::lock_guard lock { reports_mutex };
          reports.push_back(line);
        };
        result.startup_wait = 2s;
        result.shutdown_restore_wait = 2s;
        result.vdd_destroy_wait = 2s;
        return result;
      }

      process_lifecycle_t &
      make(std::function<void(lifecycle_hooks_t &)> adjust = {}) {
        auto h = hooks();
        if (adjust) adjust(h);
        lifecycle = std::make_unique<process_lifecycle_t>(options(), std::move(h));
        return *lifecycle;
      }

      native_lifecycle_hooks_t
      native_hooks() {
        native_lifecycle_hooks_t result;
        result.health = [this]() { return native_initial; };
        result.recover = [this](const mutation_capability_t &capability) {
          capability.require(operation_e::native_recover);
          saw(capability.kind(), "native_recover");
          if (native_recover_extra) native_recover_extra();
          return native_recovered;
        };
        result.restore = [this](const mutation_capability_t &capability) {
          capability.require(operation_e::native_restore);
          saw(capability.kind(), "native_restore");
          if (native_restore_extra) native_restore_extra();
          return native_restored;
        };
        result.reset = [this](const mutation_capability_t &capability) {
          capability.require(operation_e::reset_persistence);
          saw(capability.kind(), "native_reset");
          return native_reset;
        };
        result.prepare = [this](const mutation_capability_t &capability) {
          capability.require(operation_e::native_apply);
          saw(capability.kind(), "native_prepare");
          if (native_prepare_extra) native_prepare_extra();
          return native_prepared;
        };
        return result;
      }

      process_lifecycle_t &
      make_managed(std::function<void(native_lifecycle_hooks_t &)> adjust = {}) {
        auto h = hooks();
        h.native = native_hooks();
        if (adjust) adjust(*h.native);
        auto o = options();
        o.probes.active_streams = []() { return std::size_t {}; };
        lifecycle = std::make_unique<process_lifecycle_t>(std::move(o), std::move(h));
        return *lifecycle;
      }

      void
      write(const actor_record_t &record) {
        auto current = intent();
        ASSERT_TRUE(store->create(current).has_value());
        std::vector<actor_record_t> path { launched() };
        if (record.stage != actor_stage_e::launched) path.push_back(cleanup_pending());
        for (const auto &next : path) {
          ASSERT_TRUE(store->replace(current, next).has_value());
          current = next;
        }
        ASSERT_EQ(current, record);
      }

      std::optional<actor_record_t>
      durable() {
        return store->load().record;
      }
    };

    TEST(ProcessLifecycle, EveryRequiredHookIsValidated) {
      const std::vector<std::function<void(lifecycle_hooks_t &)>> removals {
        [](auto &h) { h.startup_restore = nullptr; },
        [](auto &h) { h.shutdown_restore = nullptr; },
        [](auto &h) { h.quiet_vdd_destroy = nullptr; },
        [](auto &h) { h.keep_vdd = nullptr; },
        [](auto &h) { h.actor.store = nullptr; },
        [](auto &h) { h.actor.observe = nullptr; },
      };
      for (std::size_t index = 0; index < removals.size(); ++index) {
        fixture_t f;
        EXPECT_THROW(f.make(removals[index]), std::invalid_argument) << "hook " << index;
      }
    }

    TEST(ProcessLifecycle, StartRunsTheStartupRestorationWithStartupAuthority) {
      fixture_t f;
      auto &lifecycle = f.make();
      EXPECT_EQ(lifecycle.start(), lifecycle_start_e::completed);
      EXPECT_EQ(f.log.events(), (std::vector<std::string> { "startup" }));
      EXPECT_EQ(f.seen_kinds(), (std::vector<kind> { kind::startup_recovery }));
      EXPECT_EQ(lifecycle.coordinator().status().phase, coordinator_phase_e::running);
    }

    TEST(ProcessLifecycle, AFailedStartupRestorationIsReportedAndStartReturns) {
      fixture_t f;
      f.startup_extra = []() { throw std::runtime_error { "restore exploded" }; };
      auto &lifecycle = f.make();
      EXPECT_EQ(lifecycle.start(), lifecycle_start_e::failed);
      EXPECT_TRUE(f.reported_containing("restore exploded"));
      EXPECT_TRUE(lifecycle.coordinator().status().native.in_conflict);
    }

    TEST(ProcessLifecycle, OwedActorCleanupIsCompletedAfterStartupRestoration) {
      fixture_t f;
      f.write(cleanup_pending());
      auto &lifecycle = f.make();
      EXPECT_EQ(lifecycle.start(), lifecycle_start_e::completed);
      EXPECT_EQ(f.log.events(), (std::vector<std::string> { "startup" }));
      EXPECT_FALSE(f.durable().has_value()) << "the completed record is retired";
      ASSERT_TRUE(f.store->last().has_value());
      EXPECT_EQ(f.store->last()->stage, actor_stage_e::completed);
      EXPECT_TRUE(f.reported_containing("cleanup_pending")) << "the classification is reported";
    }

    TEST(ProcessLifecycle, ADeferredStartupRestorationTakesOverTheOwedCleanupByDefault) {
      fixture_t f;
      f.write(cleanup_pending());
      f.defer_startup = true;
      f.quiescent = false;
      auto &lifecycle = f.make();
      EXPECT_EQ(lifecycle.start(), lifecycle_start_e::completed);
      EXPECT_TRUE(lifecycle.coordinator().status().startup_restore_pending);
      EXPECT_FALSE(f.durable().has_value());
      ASSERT_TRUE(f.store->last().has_value());
      EXPECT_EQ(f.store->last()->stage, actor_stage_e::transferred) << "the deferred restoration takes over, not a completion";
      EXPECT_EQ(f.store->last()->transferred_to, legacy_restore_owner);
    }

    TEST(ProcessLifecycle, StartIsBoundedWhileAnAdoptedActorStillRuns) {
      fixture_t f;
      f.write(launched());
      f.observation = process_observation_e::running;
      auto &lifecycle = f.make([](auto &h) { h.startup_wait = 50ms; });
      const auto before = std::chrono::steady_clock::now();
      EXPECT_EQ(lifecycle.start(), lifecycle_start_e::suspended);
      EXPECT_LT(std::chrono::steady_clock::now() - before, 2s);
      EXPECT_TRUE(f.log.events().empty()) << "no restoration while the adopted actor runs";
      EXPECT_TRUE(lifecycle.coordinator().status().external_recovery_pending);
      EXPECT_TRUE(f.reported_containing("suspended"));

      f.observation = process_observation_e::exited;
      ASSERT_TRUE(eventually([&]() { return lifecycle.coordinator().status().phase == coordinator_phase_e::running; }));
      EXPECT_EQ(f.log.events(), (std::vector<std::string> { "startup" }));
    }

    TEST(ProcessLifecycle, AResumedStartupRunsBeforeTheAlreadyQueuedShutdownRestore) {
      fixture_t f;
      f.write(launched());
      f.observation = process_observation_e::running;
      auto &lifecycle = f.make([](auto &h) {
        h.startup_wait = 20ms;
        h.shutdown_restore_wait = 50ms;
      });
      EXPECT_EQ(lifecycle.start(), lifecycle_start_e::suspended);
      EXPECT_EQ(lifecycle.shutdown_restore(), lifecycle_cleanup_e::timed_out);
      EXPECT_TRUE(f.log.events().empty());

      f.observation = process_observation_e::exited;
      ASSERT_TRUE(eventually([&]() { return f.log.events().size() == 2; }));
      EXPECT_EQ(f.log.events(), (std::vector<std::string> { "startup", "shutdown_restore" }));
      EXPECT_EQ(f.seen_kinds(), (std::vector<kind> { kind::startup_recovery, kind::shutdown_restore }));
    }

    TEST(ProcessLifecycle, AStartupResumedDuringShutdownMayStillDeferItsRestoration) {
      fixture_t f;
      f.write(launched());
      f.observation = process_observation_e::running;
      f.defer_startup = true;
      auto &lifecycle = f.make([](auto &h) {
        h.startup_wait = 20ms;
        h.shutdown_restore_wait = 50ms;
      });
      EXPECT_EQ(lifecycle.start(), lifecycle_start_e::suspended);
      EXPECT_EQ(lifecycle.shutdown_restore(), lifecycle_cleanup_e::timed_out);

      f.observation = process_observation_e::exited;
      ASSERT_TRUE(eventually([&]() { return f.log.events().size() == 2; }));
      EXPECT_EQ(f.log.events(), (std::vector<std::string> { "startup", "shutdown_restore" }));
      const auto status = lifecycle.coordinator().status();
      EXPECT_FALSE(status.native.in_conflict) << "a lock-screen deferral is not a failed startup because shutdown began";
      EXPECT_TRUE(status.startup_restore_pending);
      EXPECT_EQ(status.phase, coordinator_phase_e::draining);
      EXPECT_EQ(lifecycle.coordinator().retry_startup_restore().reason, reject_reason_e::draining) << "no new restore attempt is admitted while draining";
    }

    TEST(ProcessLifecycle, ShutdownStaysFencedAndStopsPromptlyWhileTheActorNeverExits) {      fixture_t f;
      f.write(launched());
      f.observation = process_observation_e::running;
      auto &lifecycle = f.make([](auto &h) {
        h.startup_wait = 20ms;
        h.shutdown_restore_wait = 30ms;
        h.vdd_destroy_wait = 30ms;
      });
      EXPECT_EQ(lifecycle.start(), lifecycle_start_e::suspended);
      const auto before = std::chrono::steady_clock::now();
      EXPECT_EQ(lifecycle.shutdown_restore(), lifecycle_cleanup_e::timed_out);
      EXPECT_EQ(lifecycle.shutdown_vdd_destroy(), lifecycle_cleanup_e::timed_out);
      EXPECT_LT(std::chrono::steady_clock::now() - before, 2s);
      EXPECT_EQ(lifecycle.coordinator().status().phase, coordinator_phase_e::stopped);
      std::this_thread::sleep_for(20ms);
      EXPECT_TRUE(f.log.events().empty()) << "nothing mutates behind an unsettled actor, even at shutdown";
      ASSERT_TRUE(f.durable().has_value());
      EXPECT_EQ(f.durable()->stage, actor_stage_e::launched) << "the record stays for the next startup";
    }

    TEST(ProcessLifecycle, ShutdownRestoreRunsAfterQueuedWorkAndThenRejectsNewWork) {
      fixture_t f;
      auto &lifecycle = f.make();
      ASSERT_EQ(lifecycle.start(), lifecycle_start_e::completed);
      std::promise<void> release;
      auto gate = release.get_future().share();
      ASSERT_EQ(lifecycle.coordinator().post(kind::reconfigure, [&, gate](const auto &, const auto &) {
        gate.wait();
        f.log.add("reconfigure");
      }),
        reject_reason_e::none);
      std::thread releaser { [&]() {
        std::this_thread::sleep_for(20ms);
        release.set_value();
      } };
      EXPECT_EQ(lifecycle.shutdown_restore(), lifecycle_cleanup_e::completed);
      releaser.join();
      EXPECT_EQ(f.log.events(), (std::vector<std::string> { "startup", "reconfigure", "shutdown_restore" }));
      EXPECT_EQ(lifecycle.coordinator().status().phase, coordinator_phase_e::draining);
      EXPECT_EQ(lifecycle.coordinator().post(kind::reconfigure, [](const auto &, const auto &) {}), reject_reason_e::draining);
    }

    TEST(ProcessLifecycle, AFailedShutdownRestoreIsReported) {
      fixture_t f;
      auto &lifecycle = f.make([](auto &h) {
        h.shutdown_restore = [](const auto &, const auto &) { throw std::runtime_error { "restore refused" }; };
      });
      ASSERT_EQ(lifecycle.start(), lifecycle_start_e::completed);
      EXPECT_EQ(lifecycle.shutdown_restore(), lifecycle_cleanup_e::failed);
      EXPECT_TRUE(f.reported_containing("restore refused"));
    }

    TEST(ProcessLifecycle, FinalVddDestroyRunsQuietlyThenStops) {
      fixture_t f;
      auto &lifecycle = f.make();
      ASSERT_EQ(lifecycle.start(), lifecycle_start_e::completed);
      ASSERT_EQ(lifecycle.shutdown_restore(), lifecycle_cleanup_e::completed);
      EXPECT_EQ(lifecycle.shutdown_vdd_destroy(), lifecycle_cleanup_e::completed);
      EXPECT_EQ(f.seen_kinds(), (std::vector<kind> { kind::startup_recovery, kind::shutdown_restore, kind::shutdown_vdd_destroy }));
      EXPECT_EQ(lifecycle.coordinator().status().phase, coordinator_phase_e::stopped);
      EXPECT_EQ(lifecycle.shutdown_vdd_destroy(), lifecycle_cleanup_e::skipped) << "idempotent";
      EXPECT_EQ(lifecycle.shutdown_restore(), lifecycle_cleanup_e::skipped) << "nothing runs after stop";
      EXPECT_EQ(f.log.events().size(), 3u);
    }

    TEST(ProcessLifecycle, AKeptVddIsNotDestroyedButTheCoordinatorStillStops) {
      fixture_t f;
      f.keep_vdd = true;
      auto &lifecycle = f.make();
      ASSERT_EQ(lifecycle.start(), lifecycle_start_e::completed);
      EXPECT_EQ(lifecycle.shutdown_vdd_destroy(), lifecycle_cleanup_e::skipped);
      EXPECT_EQ(f.log.events(), (std::vector<std::string> { "startup" }));
      EXPECT_EQ(lifecycle.coordinator().status().phase, coordinator_phase_e::stopped);
    }

    TEST(ProcessLifecycle, AHeadANativeJournalMayOwnIsNeverDestroyedAutomatically) {
      fixture_t f;
      auto &lifecycle = f.make();
      ASSERT_EQ(lifecycle.start(), lifecycle_start_e::completed);
      EXPECT_FALSE(lifecycle.suppress_automatic_vdd());
      lifecycle.coordinator().publish_native_health({ true, false, "journal" });
      EXPECT_TRUE(lifecycle.suppress_automatic_vdd());
      EXPECT_EQ(lifecycle.shutdown_vdd_destroy(), lifecycle_cleanup_e::rejected);
      EXPECT_EQ(f.log.events(), (std::vector<std::string> { "startup" }));
      EXPECT_EQ(lifecycle.coordinator().status().phase, coordinator_phase_e::stopped);
      EXPECT_TRUE(f.reported_containing("recovery_required"));
    }

    TEST(ProcessLifecycle, AConflictedNativeHealthAlsoSuppressesAutomaticVdd) {
      fixture_t f;
      auto &lifecycle = f.make();
      lifecycle.coordinator().publish_native_health({ false, true, "conflict" });
      EXPECT_TRUE(lifecycle.suppress_automatic_vdd());
    }

    TEST(ProcessLifecycle, ShutdownWithoutStartStillStopsWithoutRunningBodies) {
      fixture_t f;
      auto &lifecycle = f.make();
      EXPECT_EQ(lifecycle.shutdown_restore(), lifecycle_cleanup_e::skipped);
      EXPECT_EQ(lifecycle.shutdown_vdd_destroy(), lifecycle_cleanup_e::skipped);
      EXPECT_TRUE(f.log.events().empty());
      EXPECT_EQ(lifecycle.coordinator().status().phase, coordinator_phase_e::stopped);
    }

    bool
    same_health(const native_health_t &actual, const native_health_t &expected) {
      return actual.journal_present == expected.journal_present && actual.in_conflict == expected.in_conflict &&
             actual.reason == expected.reason;
    }

    TEST(ProcessLifecycleManaged, EveryNativeHookIsRequired) {
      const std::vector<std::function<void(native_lifecycle_hooks_t &)>> removals {
        [](auto &n) { n.health = nullptr; },
        [](auto &n) { n.recover = nullptr; },
        [](auto &n) { n.restore = nullptr; },
        [](auto &n) { n.reset = nullptr; },
        [](auto &n) { n.prepare = nullptr; },
      };
      for (std::size_t index = 0; index < removals.size(); ++index) {
        fixture_t f;
        EXPECT_THROW(f.make_managed(removals[index]), std::invalid_argument) << "hook " << index;
      }
    }

    TEST(ProcessLifecycleManaged, ManagedCoordinatorOptionsWithoutNativeHooksAreRefused) {
      fixture_t f;
      auto o = f.options();
      o.managed_mode = true;
      o.probes.active_streams = []() { return std::size_t {}; };
      o.native_restore = [](const auto &, const auto &) {};
      EXPECT_THROW((process_lifecycle_t { std::move(o), f.hooks() }), std::invalid_argument);
    }

    TEST(ProcessLifecycleManaged, LegacyHooksLeaveTheCoordinatorLegacyAndResetNativeIsANoOp) {
      fixture_t f;
      auto &lifecycle = f.make();
      ASSERT_EQ(lifecycle.start(), lifecycle_start_e::completed);
      EXPECT_FALSE(lifecycle.managed());
      EXPECT_FALSE(lifecycle.coordinator().status().managed_mode);
      const auto ran = lifecycle.coordinator().run(kind::reset_persistence,
        [&](const mutation_capability_t &capability, const command_context_t &) { EXPECT_FALSE(lifecycle.reset_native(capability)); });
      EXPECT_TRUE(ran.completed());
      EXPECT_EQ(f.log.events(), (std::vector<std::string> { "startup" }));
    }

    TEST(ProcessLifecycleManaged, ManagedModeIsLatchedAndTheInitialHealthIsPublishedAtConstruction) {
      fixture_t f;
      f.native_initial = { false, true, "Invalid display_topology_profile" };
      auto &lifecycle = f.make_managed();
      EXPECT_TRUE(lifecycle.managed());
      const auto status = lifecycle.coordinator().status();
      EXPECT_TRUE(status.managed_mode);
      EXPECT_TRUE(same_health(status.native, f.native_initial)) << status.native.reason;
      EXPECT_TRUE(f.log.events().empty()) << "nothing runs before start";
    }

    TEST(ProcessLifecycleManaged, NativeRecoveryRunsBeforeTheLegacyStartupRestorationAndIsPublished) {
      fixture_t f;
      f.native_recovered = { true, false, "journal kept" };
      auto &lifecycle = f.make_managed();
      ASSERT_EQ(lifecycle.start(), lifecycle_start_e::completed);
      EXPECT_EQ(f.log.events(), (std::vector<std::string> { "native_recover", "startup" }));
      EXPECT_EQ(f.seen_kinds(), (std::vector<kind> { kind::startup_recovery, kind::startup_recovery }));
      EXPECT_TRUE(same_health(lifecycle.coordinator().status().native, f.native_recovered));
    }

    TEST(ProcessLifecycleManaged, AThrowingNativeRecoveryIsInConflictButLegacyStartupStillRuns) {
      fixture_t f;
      f.native_recover_extra = []() { throw std::runtime_error { "recover exploded" }; };
      auto &lifecycle = f.make_managed();
      lifecycle.start();
      EXPECT_EQ(f.log.events(), (std::vector<std::string> { "native_recover", "startup" }));
      const auto native = lifecycle.coordinator().status().native;
      EXPECT_TRUE(native.in_conflict);
      EXPECT_NE(native.reason.find("recover exploded"), std::string::npos) << native.reason;
    }

    TEST(ProcessLifecycleManaged, AConflictedRecoveryRefusesManagedLaunches) {
      fixture_t f;
      f.native_recovered = { true, true, "head of unknown origin" };
      auto &lifecycle = f.make_managed();
      ASSERT_EQ(lifecycle.start(), lifecycle_start_e::completed);
      const auto admission = lifecycle.coordinator().admit_launch({ 1, "cert:a" });
      EXPECT_FALSE(admission);
      EXPECT_EQ(admission.reason, reject_reason_e::recovery_required);
    }

    TEST(ProcessLifecycleManaged, AnEndedManagedLeaseRunsTheNativeRestoreAndPublishesItsHealth) {
      fixture_t f;
      f.native_restored = { true, true, "restore could not prove the head" };
      auto &lifecycle = f.make_managed();
      ASSERT_EQ(lifecycle.start(), lifecycle_start_e::completed);
      auto &coordinator = lifecycle.coordinator();
      const auto admission = coordinator.admit_launch({ 1, "cert:a" });
      ASSERT_TRUE(admission);
      EXPECT_EQ(admission.token->flavor, launch_flavor_e::managed);
      ASSERT_TRUE(coordinator.run_launch_prepare(*admission.token, [](const auto &, const auto &) {}).completed());
      ASSERT_TRUE(coordinator.abort_launch(*admission.token, {}));
      EXPECT_TRUE(eventually([&]() { return same_health(coordinator.status().native, f.native_restored); }));
      const auto events = f.log.events();
      EXPECT_EQ(std::count(events.begin(), events.end(), "native_restore"), 1);
    }

    TEST(ProcessLifecycleManaged, ShutdownRestoresNativelyAndNeverRunsTheLegacyShutdownRestoration) {
      fixture_t f;
      auto &lifecycle = f.make_managed();
      ASSERT_EQ(lifecycle.start(), lifecycle_start_e::completed);
      EXPECT_EQ(lifecycle.shutdown_restore(), lifecycle_cleanup_e::completed);
      EXPECT_EQ(f.log.events(), (std::vector<std::string> { "native_recover", "startup", "native_restore" }));
      EXPECT_EQ(f.seen_kinds().back(), kind::shutdown_restore);
    }

    TEST(ProcessLifecycleManaged, AFailedShutdownNativeRestoreIsReportedAndInConflict) {
      fixture_t f;
      f.native_restore_extra = []() { throw std::runtime_error { "restore exploded" }; };
      auto &lifecycle = f.make_managed();
      ASSERT_EQ(lifecycle.start(), lifecycle_start_e::completed);
      EXPECT_EQ(lifecycle.shutdown_restore(), lifecycle_cleanup_e::failed);
      EXPECT_TRUE(f.reported_containing("restore exploded"));
      EXPECT_TRUE(lifecycle.coordinator().status().native.in_conflict);
    }

    TEST(ProcessLifecycleManaged, ManagedShutdownNeverDestroysAVddButStillStops) {
      fixture_t f;
      auto &lifecycle = f.make_managed();
      ASSERT_EQ(lifecycle.start(), lifecycle_start_e::completed);
      EXPECT_EQ(lifecycle.shutdown_vdd_destroy(), lifecycle_cleanup_e::skipped);
      EXPECT_EQ(f.log.events(), (std::vector<std::string> { "native_recover", "startup" }));
      EXPECT_EQ(lifecycle.coordinator().status().phase, coordinator_phase_e::stopped);
      EXPECT_FALSE(f.reported_containing("rejected"));
    }

    TEST(ProcessLifecycleManaged, ManagedModeSuppressesAutomaticVddEvenWhenHealthy) {
      fixture_t f;
      auto &lifecycle = f.make_managed();
      ASSERT_EQ(lifecycle.start(), lifecycle_start_e::completed);
      EXPECT_FALSE(lifecycle.coordinator().status().native.in_conflict);
      EXPECT_TRUE(lifecycle.suppress_automatic_vdd());
    }

    TEST(ProcessLifecycleManaged, ResetNativeRunsTheNativeResetAndPublishesItsHealth) {
      fixture_t f;
      f.native_recovered = { true, true, "unresolved" };
      f.native_reset = { false, false, "" };
      auto &lifecycle = f.make_managed();
      ASSERT_EQ(lifecycle.start(), lifecycle_start_e::completed);
      const auto ran = lifecycle.coordinator().run(kind::reset_persistence,
        [&](const mutation_capability_t &capability, const command_context_t &) { EXPECT_FALSE(lifecycle.reset_native(capability)); });
      EXPECT_TRUE(ran.completed());
      EXPECT_TRUE(same_health(lifecycle.coordinator().status().native, f.native_reset));
      EXPECT_EQ(f.log.events().back(), "native_reset");
    }

    TEST(ProcessLifecycleManaged, AResetThatKeepsTheJournalIsReportedAsFailed) {
      fixture_t f;
      f.native_recovered = { true, true, "unresolved" };
      f.native_reset = { true, true, "Could not remove the journal: access denied" };
      auto &lifecycle = f.make_managed();
      ASSERT_EQ(lifecycle.start(), lifecycle_start_e::completed);
      std::optional<std::string> failure;
      const auto ran = lifecycle.coordinator().run(kind::reset_persistence,
        [&](const mutation_capability_t &capability, const command_context_t &) { failure = lifecycle.reset_native(capability); });
      EXPECT_TRUE(ran.completed());
      ASSERT_TRUE(failure);
      EXPECT_NE(failure->find("access denied"), std::string::npos) << *failure;
    }

    TEST(ProcessLifecycleManaged, AFailedResetWithoutAReasonStillExplainsItself) {
      fixture_t f;
      f.native_recovered = { true, true, "unresolved" };
      f.native_reset = { true, true, "" };
      auto &lifecycle = f.make_managed();
      ASSERT_EQ(lifecycle.start(), lifecycle_start_e::completed);
      std::optional<std::string> failure;
      const auto ran = lifecycle.coordinator().run(kind::reset_persistence,
        [&](const mutation_capability_t &capability, const command_context_t &) { failure = lifecycle.reset_native(capability); });
      EXPECT_TRUE(ran.completed());
      ASSERT_TRUE(failure);
      EXPECT_NE(failure->find("journal"), std::string::npos) << *failure;
    }

    TEST(ProcessLifecycleManaged, AnInvalidProfileThatStaysInConflictAfterAClearedResetIsNotAFailedReset) {
      fixture_t f;
      f.native_initial = { false, true, "Invalid display_topology_profile" };
      f.native_reset = { false, true, "Invalid display_topology_profile" };
      auto &lifecycle = f.make_managed();
      ASSERT_EQ(lifecycle.start(), lifecycle_start_e::completed);
      std::optional<std::string> failure { "not run" };
      const auto ran = lifecycle.coordinator().run(kind::reset_persistence,
        [&](const mutation_capability_t &capability, const command_context_t &) { failure = lifecycle.reset_native(capability); });
      EXPECT_TRUE(ran.completed());
      EXPECT_FALSE(failure) << *failure;
    }

    namespace {
      struct prepared_in_launch_t {
        native_prepared_t prepared { std::nullopt, "not run" };
        native_health_t published;
      };

      prepared_in_launch_t
      prepare_in_launch(process_lifecycle_t &lifecycle) {
        auto &coordinator = lifecycle.coordinator();
        const auto admission = coordinator.admit_launch({ 7, "cert:a" });
        EXPECT_TRUE(admission) << to_string(admission.reason);
        prepared_in_launch_t result;
        if (admission) {
          EXPECT_TRUE(coordinator.run_launch_prepare(*admission.token, [&](const mutation_capability_t &capability, const command_context_t &) {
            result.prepared = lifecycle.prepare_native(capability);
            result.published = coordinator.status().native;
          }).completed());
          coordinator.abort_launch(*admission.token, {});
        }
        return result;
      }
    }  // namespace

    TEST(ProcessLifecycleManaged, PrepareNativeReturnsTheTargetAndPublishesTheNewHealth) {
      fixture_t f;
      auto &lifecycle = f.make_managed();
      ASSERT_EQ(lifecycle.start(), lifecycle_start_e::completed);
      f.native_initial = { true, false, "journaled" };
      const auto [prepared, published] = prepare_in_launch(lifecycle);
      EXPECT_EQ(prepared.display, (managed_target_t { "{zako-head}", true })) << prepared.reason;
      EXPECT_EQ(f.seen_kinds().back(), kind::launch_prepare);
      const auto events = f.log.events();
      EXPECT_EQ(std::count(events.begin(), events.end(), "native_prepare"), 1);
      EXPECT_TRUE(same_health(published, f.native_initial)) << "the prepare's journal is published before the launch proceeds";
    }

    TEST(ProcessLifecycleManaged, ARefusedNativePrepareHasNoTargetAndKeepsItsReason) {
      fixture_t f;
      f.native_prepared = { std::nullopt, "no virtual display driver" };
      auto &lifecycle = f.make_managed();
      ASSERT_EQ(lifecycle.start(), lifecycle_start_e::completed);
      const auto prepared = prepare_in_launch(lifecycle).prepared;
      EXPECT_FALSE(prepared.display);
      EXPECT_EQ(prepared.reason, "no virtual display driver");
    }

    TEST(ProcessLifecycleManaged, AThrowingNativePrepareIsInConflictAndHasNoTarget) {
      fixture_t f;
      f.native_prepare_extra = []() { throw std::runtime_error { "prepare exploded" }; };
      auto &lifecycle = f.make_managed();
      ASSERT_EQ(lifecycle.start(), lifecycle_start_e::completed);
      const auto [prepared, published] = prepare_in_launch(lifecycle);
      EXPECT_FALSE(prepared.display);
      EXPECT_NE(prepared.reason.find("prepare exploded"), std::string::npos) << prepared.reason;
      EXPECT_TRUE(published.in_conflict);
    }

    TEST(ProcessLifecycleManaged, LegacyPrepareNativeRefusesWithoutRunningAnything) {
      fixture_t f;
      auto &lifecycle = f.make();
      ASSERT_EQ(lifecycle.start(), lifecycle_start_e::completed);
      native_prepared_t prepared;
      const auto ran = lifecycle.coordinator().run(kind::reset_persistence,
        [&](const mutation_capability_t &capability, const command_context_t &) { prepared = lifecycle.prepare_native(capability); });
      EXPECT_TRUE(ran.completed());
      EXPECT_FALSE(prepared.display);
      EXPECT_FALSE(prepared.reason.empty());
      EXPECT_EQ(f.log.events(), (std::vector<std::string> { "startup" }));
    }

    namespace {
      struct mode_change_run_t {
        bool completed {};
        native_mode_changed_t changed { native_mode_verdict_e::applied, "not run", "not run" };
        native_health_t published;
      };

      mode_change_run_t
      change_mode(process_lifecycle_t &lifecycle, std::uint32_t width, std::uint32_t height) {
        mode_change_run_t result;
        result.completed = lifecycle.coordinator().run(kind::reconfigure, [&](const mutation_capability_t &capability, const command_context_t &) {
          result.changed = lifecycle.change_native_mode(capability, width, height);
          result.published = lifecycle.coordinator().status().native;
        }).completed();
        return result;
      }
    }  // namespace

    TEST(ProcessLifecycleManaged, ChangeNativeModeRunsTheHookAndPublishesTheNewHealth) {
      fixture_t f;
      std::vector<std::string> seen;
      auto &lifecycle = f.make_managed([&](native_lifecycle_hooks_t &hooks) {
        hooks.change_mode = [&](const mutation_capability_t &capability, std::uint32_t width, std::uint32_t height) {
          capability.require(operation_e::native_mode);
          seen.push_back(std::to_string(width) + "x" + std::to_string(height));
          return native_mode_changed_t { native_mode_verdict_e::applied, "{zako-head}", {} };
        };
      });
      ASSERT_EQ(lifecycle.start(), lifecycle_start_e::completed);
      f.native_initial = { true, false, "journaled at 2560x1440" };
      const auto run = change_mode(lifecycle, 2560, 1440);
      ASSERT_TRUE(run.completed);
      EXPECT_EQ(run.changed.verdict, native_mode_verdict_e::applied);
      EXPECT_EQ(run.changed.display, "{zako-head}");
      EXPECT_EQ(seen, (std::vector<std::string> { "2560x1440" }));
      EXPECT_TRUE(same_health(run.published, f.native_initial)) << "the head's new journal is published";
    }

    TEST(ProcessLifecycleManaged, ARefusedNativeModeChangeKeepsItsReason) {
      fixture_t f;
      auto &lifecycle = f.make_managed([&](native_lifecycle_hooks_t &hooks) {
        hooks.change_mode = [](const mutation_capability_t &, std::uint32_t, std::uint32_t) {
          return native_mode_changed_t { native_mode_verdict_e::refused, "{zako-head}", "mode not offered" };
        };
      });
      ASSERT_EQ(lifecycle.start(), lifecycle_start_e::completed);
      const auto run = change_mode(lifecycle, 2560, 1440);
      ASSERT_TRUE(run.completed);
      EXPECT_EQ(run.changed.verdict, native_mode_verdict_e::refused);
      EXPECT_EQ(run.changed.reason, "mode not offered");
    }

    TEST(ProcessLifecycleManaged, AThrowingNativeModeChangeIsInConflict) {
      fixture_t f;
      auto &lifecycle = f.make_managed([&](native_lifecycle_hooks_t &hooks) {
        hooks.change_mode = [](const mutation_capability_t &, std::uint32_t, std::uint32_t) -> native_mode_changed_t {
          throw std::runtime_error { "ioctl exploded" };
        };
      });
      ASSERT_EQ(lifecycle.start(), lifecycle_start_e::completed);
      const auto run = change_mode(lifecycle, 2560, 1440);
      ASSERT_TRUE(run.completed);
      EXPECT_EQ(run.changed.verdict, native_mode_verdict_e::conflict);
      EXPECT_NE(run.changed.reason.find("ioctl exploded"), std::string::npos) << run.changed.reason;
      EXPECT_TRUE(run.published.in_conflict);
    }

    TEST(ProcessLifecycleManaged, AnUnknownHealthAfterAModeChangeIsInConflict) {
      fixture_t f;
      bool changed = false;
      auto &lifecycle = f.make_managed([&](native_lifecycle_hooks_t &hooks) {
        auto health = hooks.health;
        hooks.health = [&changed, health]() {
          if (changed) throw std::runtime_error { "journal unreadable" };
          return health();
        };
        hooks.change_mode = [&changed](const mutation_capability_t &, std::uint32_t, std::uint32_t) {
          changed = true;
          return native_mode_changed_t { native_mode_verdict_e::applied, "{zako-head}", {} };
        };
      });
      ASSERT_EQ(lifecycle.start(), lifecycle_start_e::completed);
      const auto run = change_mode(lifecycle, 2560, 1440);
      ASSERT_TRUE(run.completed);
      EXPECT_EQ(run.changed.verdict, native_mode_verdict_e::conflict);
      EXPECT_TRUE(run.published.in_conflict);
    }

    TEST(ProcessLifecycleManaged, WithoutAChangeHookTheModeChangeIsRefused) {
      fixture_t f;
      auto &lifecycle = f.make_managed();
      ASSERT_EQ(lifecycle.start(), lifecycle_start_e::completed);
      const auto run = change_mode(lifecycle, 2560, 1440);
      ASSERT_TRUE(run.completed);
      EXPECT_EQ(run.changed.verdict, native_mode_verdict_e::refused);
      EXPECT_FALSE(run.changed.reason.empty());
    }

    TEST(ProcessLifecycleManaged, LegacyChangeNativeModeRefusesWithoutRunningAnything) {
      fixture_t f;
      auto &lifecycle = f.make();
      ASSERT_EQ(lifecycle.start(), lifecycle_start_e::completed);
      native_mode_changed_t changed { native_mode_verdict_e::applied, {}, {} };
      const auto ran = lifecycle.coordinator().run(kind::reset_persistence,
        [&](const mutation_capability_t &capability, const command_context_t &) { changed = lifecycle.change_native_mode(capability, 2560, 1440); });
      EXPECT_TRUE(ran.completed());
      EXPECT_EQ(changed.verdict, native_mode_verdict_e::refused);
      EXPECT_FALSE(changed.reason.empty());
      EXPECT_EQ(f.log.events(), (std::vector<std::string> { "startup" }));
    }

  }  // namespace
}  // namespace display_device::topology
