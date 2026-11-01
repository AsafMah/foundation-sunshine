/**
 * @file tests/unit/test_topology_process_lifecycle.cpp
 * @brief Startup, shutdown restore and final VDD cleanup of the process-lifetime coordinator.
 */
#include <gtest/gtest.h>

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

  }  // namespace
}  // namespace display_device::topology
