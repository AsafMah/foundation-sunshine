/**
 * @file tests/unit/test_topology_actor_startup.cpp
 * @brief Startup recovery of the durable external actor record.
 */
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <future>
#include <mutex>
#include <random>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "src/display_device/topology_actor_startup.h"

namespace display_device::topology {
  namespace {
    using namespace std::chrono_literals;
    using kind = command_kind_e;

    constexpr std::uint64_t op = 5151;
    constexpr actor_receipt_t receipt { 4410, 133'400'000'000'000'000ULL };
    constexpr auto real_timeout = 5s;

    class temp_dir_t {
    public:
      temp_dir_t() {
        std::random_device random;
        path_ = std::filesystem::temp_directory_path() /
                ("sunshine-actor-startup-" + std::to_string(random()) + "-" + std::to_string(random()));
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

    /** Records every durable transition so tests can tell completion from transfer. */
    class store_t final: public actor_record_store_t {
    public:
      explicit store_t(std::filesystem::path path):
          inner_(std::move(path)) {}

      actor_load_t
      load() override {
        if (vanish) return {};
        return inner_.load();
      }

      result_t<void>
      create(const actor_record_t &record) override {
        return inner_.create(record);
      }

      result_t<void>
      replace(const actor_record_t &expected, const actor_record_t &next) override {
        if (fail_replace) return std::unexpected(error_t { error_code_e::resource_unavailable, "injected replace failure", {} });
        auto result = inner_.replace(expected, next);
        if (result) {
          std::lock_guard lock { mutex_ };
          stages_.push_back(next.stage);
          last_ = next;
        }
        return result;
      }

      result_t<void>
      retire(const actor_record_t &expected) override {
        auto result = inner_.retire(expected);
        if (result) {
          std::lock_guard lock { mutex_ };
          ++retired_;
        }
        return result;
      }

      std::vector<actor_stage_e>
      stages() const {
        std::lock_guard lock { mutex_ };
        return stages_;
      }

      int
      retired() const {
        std::lock_guard lock { mutex_ };
        return retired_;
      }

      std::optional<actor_record_t>
      last() const {
        std::lock_guard lock { mutex_ };
        return last_;
      }

      void
      forget_history() {
        std::lock_guard lock { mutex_ };
        stages_.clear();
      }

      std::atomic_bool fail_replace {};
      std::atomic_bool vanish {};

    private:
      file_actor_record_store_t inner_;
      mutable std::mutex mutex_;
      std::vector<actor_stage_e> stages_;
      std::optional<actor_record_t> last_;
      int retired_ {};
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

    private:
      mutable std::mutex mutex_;
      std::vector<std::string> events_;
    };

    actor_record_t
    intent() {
      auto record = begin_actor_intent(op, std::string { "boot-a" }, actor_obligation_e::restore_legacy_display, "adapter restart");
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

    struct fixture_t {
      temp_dir_t dir;
      std::shared_ptr<store_t> store { std::make_shared<store_t>(dir.file()) };
      coordinator_t coordinator;
      event_log_t log;
      std::atomic<process_observation_e> observation { process_observation_e::exited };
      std::atomic_int observed {};
      std::atomic_bool restore_pending {};
      actor_spawn_t spawn;
      std::atomic<actor_recovery_e> classified { actor_recovery_e::none };
      std::shared_future<command_result_t> started;
      std::vector<std::string> reports;
      std::mutex reports_mutex;
      std::vector<std::thread> watchers;
      std::mutex watchers_mutex;
      std::atomic_int watchers_done {};
      std::function<void(actor_startup_options_t &)> adjust;

      fixture_t() {
        spawn = [this](std::function<void()> work) {
          std::lock_guard lock { watchers_mutex };
          watchers.emplace_back([this, work = std::move(work)]() {
            work();
            ++watchers_done;
          });
        };
      }

      ~fixture_t() {
        observation = process_observation_e::exited;
        {
          std::lock_guard lock { watchers_mutex };
          for (auto &watcher : watchers) watcher.join();
        }
        coordinator.stop();
      }

      /** Reach `record` through the store's own transitions, as a dead process would have left it. */
      void
      write(const actor_record_t &record) {
        auto current = intent();
        const auto created = store->create(current);
        ASSERT_TRUE(created.has_value()) << created.error().detail;
        std::vector<actor_record_t> path { launched() };
        if (record.stage != actor_stage_e::launched) path.push_back(cleanup_pending());
        if (actor_record_terminal(record)) path.push_back(record);
        for (const auto &next : path) {
          const auto replaced = store->replace(current, next);
          ASSERT_TRUE(replaced.has_value()) << replaced.error().detail;
          current = next;
        }
        ASSERT_EQ(current, record);
        store->forget_history();
      }

      actor_startup_options_t
      options() {
        actor_startup_options_t result;
        result.store = store;
        result.current_boot = std::string { "boot-a" };
        result.observe = [this](const actor_receipt_t &seen) {
          EXPECT_EQ(seen, receipt);
          ++observed;
          return observation.load();
        };
        result.spawn = spawn;
        result.poll_interval = 1ms;
        result.restore_pending = [this]() { return restore_pending.load(); };
        result.report = [this](const std::string &line) {
          std::lock_guard lock { reports_mutex };
          reports.push_back(line);
        };
        if (adjust) adjust(result);
        return result;
      }

      /** Start the coordinator with recovery as the first statement of its startup body. */
      void
      start(command_body_t startup = {}) {
        if (!startup) {
          startup = [this](const mutation_capability_t &capability, const command_context_t &) {
            EXPECT_EQ(capability.kind(), kind::startup_recovery);
            log.add("startup");
          };
        }
        started = coordinator.start([this, startup = std::move(startup)](const mutation_capability_t &capability, const command_context_t &) {
          classified = recover_startup_actor(coordinator, capability, options(), startup);
        });
      }

      std::optional<actor_record_t>
      durable() {
        auto loaded = store->load();
        return loaded.record;
      }
    };

    TEST(ActorStartupRecovery, WithoutARecordStartupRunsAndNothingIsWritten) {
      fixture_t f;
      f.start();
      ASSERT_EQ(f.started.wait_for(real_timeout), std::future_status::ready);
      EXPECT_TRUE(f.started.get().completed());
      EXPECT_EQ(f.classified.load(), actor_recovery_e::none);
      EXPECT_EQ(f.log.events(), (std::vector<std::string> { "startup" }));
      EXPECT_TRUE(f.store->stages().empty());
      EXPECT_EQ(f.observed.load(), 0);
      EXPECT_FALSE(f.coordinator.status().external_recovery_pending);
    }

    TEST(ActorStartupRecovery, OwedCleanupIsCompletedAndRetiredAfterStartupRestored) {
      fixture_t f;
      f.write(cleanup_pending());
      f.start();
      ASSERT_EQ(f.started.wait_for(real_timeout), std::future_status::ready);
      EXPECT_TRUE(f.started.get().completed());
      EXPECT_EQ(f.classified.load(), actor_recovery_e::cleanup_pending);
      EXPECT_EQ(f.log.events(), (std::vector<std::string> { "startup" }));
      EXPECT_EQ(f.store->stages(), (std::vector<actor_stage_e> { actor_stage_e::completed }));
      EXPECT_EQ(f.store->retired(), 1);
      EXPECT_FALSE(f.durable().has_value());
    }

    TEST(ActorStartupRecovery, ADeferredStartupRestoreTakesOverTheObligation) {
      fixture_t f;
      f.write(cleanup_pending());
      f.restore_pending = true;
      f.start();
      ASSERT_EQ(f.started.wait_for(real_timeout), std::future_status::ready);
      EXPECT_TRUE(f.started.get().completed());
      EXPECT_EQ(f.store->stages(), (std::vector<actor_stage_e> { actor_stage_e::transferred }));
      ASSERT_TRUE(f.store->last().has_value());
      EXPECT_EQ(f.store->last()->transferred_to, legacy_restore_owner);
      EXPECT_EQ(f.store->retired(), 1);
    }

    TEST(ActorStartupRecovery, AnExitedActorIsSettledBeforeStartupRestores) {
      fixture_t f;
      f.write(launched());
      std::vector<actor_stage_e> stages_at_startup;
      f.start([&](const mutation_capability_t &, const command_context_t &) {
        stages_at_startup = f.store->stages();
        f.log.add("startup");
      });
      ASSERT_EQ(f.started.wait_for(real_timeout), std::future_status::ready);
      EXPECT_TRUE(f.started.get().completed());
      EXPECT_EQ(f.classified.load(), actor_recovery_e::actor_settled);
      EXPECT_EQ(stages_at_startup, (std::vector<actor_stage_e> { actor_stage_e::cleanup_pending }));
      EXPECT_EQ(f.store->stages(), (std::vector<actor_stage_e> { actor_stage_e::cleanup_pending, actor_stage_e::completed }));
      EXPECT_FALSE(f.durable().has_value());
    }

    TEST(ActorStartupRecovery, ARunningActorIsAdoptedAndStartupWaitsAheadOfQueuedShutdown) {
      fixture_t f;
      f.write(launched());
      f.observation = process_observation_e::running;
      f.start();
      const auto deadline = std::chrono::steady_clock::now() + real_timeout;
      while (f.observed.load() < 3 && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
      ASSERT_GE(f.observed.load(), 3) << "the watcher keeps observing the adopted actor";
      EXPECT_EQ(f.classified.load(), actor_recovery_e::actor_running);
      EXPECT_TRUE(f.coordinator.status().external_recovery_pending);
      EXPECT_NE(f.coordinator.post(kind::reconfigure, [&](const auto &, const auto &) { f.log.add("reconfigure"); }), reject_reason_e::none)
        << "nothing else may mutate while the adopted actor runs";

      f.coordinator.begin_draining();
      auto shutdown = f.coordinator.post_tracked(kind::shutdown_restore, [&](const auto &, const auto &) { f.log.add("shutdown"); });
      std::this_thread::sleep_for(20ms);
      EXPECT_TRUE(f.log.events().empty()) << "startup and queued cleanup stay suspended";
      EXPECT_NE(f.started.wait_for(0ms), std::future_status::ready);

      f.observation = process_observation_e::exited;
      ASSERT_EQ(f.started.wait_for(real_timeout), std::future_status::ready);
      EXPECT_TRUE(f.started.get().completed());
      ASSERT_EQ(shutdown.completion.wait_for(real_timeout), std::future_status::ready);
      EXPECT_EQ(f.log.events(), (std::vector<std::string> { "startup", "shutdown" }));
      EXPECT_EQ(f.store->stages(), (std::vector<actor_stage_e> { actor_stage_e::cleanup_pending, actor_stage_e::completed }));
    }

    TEST(ActorStartupRecovery, AnUnreadableRecordKeepsTopologyFencedUntilStop) {
      fixture_t f;
      {
        std::ofstream corrupt { f.dir.file(), std::ios::binary };
        corrupt << "{ not json";
      }
      f.start();
      std::this_thread::sleep_for(50ms);
      EXPECT_EQ(f.classified.load(), actor_recovery_e::blocked);
      EXPECT_NE(f.started.wait_for(0ms), std::future_status::ready) << "startup must not fall through to normal operation";
      EXPECT_TRUE(f.coordinator.status().external_recovery_pending);
      EXPECT_TRUE(f.log.events().empty());
      EXPECT_TRUE(std::filesystem::exists(f.dir.file())) << "an unusable record is never discarded";
      {
        std::lock_guard lock { f.reports_mutex };
        EXPECT_FALSE(f.reports.empty()) << "the reason is reported";
      }
      f.coordinator.stop();
      ASSERT_EQ(f.started.wait_for(real_timeout), std::future_status::ready);
      EXPECT_FALSE(f.started.get().completed());
      EXPECT_TRUE(f.log.events().empty());
    }

    TEST(ActorStartupRecovery, AFailedSettleWriteKeepsTopologyFenced) {
      fixture_t f;
      f.write(launched());
      f.store->fail_replace = true;
      f.start();
      std::this_thread::sleep_for(50ms);
      EXPECT_NE(f.started.wait_for(0ms), std::future_status::ready);
      EXPECT_TRUE(f.log.events().empty());
      ASSERT_TRUE(f.durable().has_value());
      EXPECT_EQ(f.durable()->stage, actor_stage_e::launched);
    }

    TEST(ActorStartupRecovery, AWatcherThatCannotStartKeepsTopologyFenced) {
      fixture_t f;
      f.write(launched());
      f.observation = process_observation_e::running;
      f.spawn = [](std::function<void()>) { throw std::system_error { std::make_error_code(std::errc::resource_unavailable_try_again) }; };
      f.start();
      std::this_thread::sleep_for(50ms);
      EXPECT_EQ(f.classified.load(), actor_recovery_e::blocked);
      EXPECT_NE(f.started.wait_for(0ms), std::future_status::ready);
      EXPECT_TRUE(f.log.events().empty());
    }

    TEST(ActorStartupRecovery, AFailedStartupRestoreKeepsTheObligation) {
      fixture_t f;
      f.write(cleanup_pending());
      f.start([](const mutation_capability_t &, const command_context_t &) { throw std::runtime_error { "restore failed" }; });
      ASSERT_EQ(f.started.wait_for(real_timeout), std::future_status::ready);
      EXPECT_FALSE(f.started.get().completed());
      EXPECT_TRUE(f.store->stages().empty());
      ASSERT_TRUE(f.durable().has_value());
      EXPECT_EQ(f.durable()->stage, actor_stage_e::cleanup_pending);
    }

    TEST(ActorStartupRecovery, ADischargedRecordIsRetiredBeforeStartup) {
      fixture_t f;
      auto done = complete_actor_cleanup(cleanup_pending(), op);
      ASSERT_TRUE(done.has_value());
      f.write(*done);
      f.start();
      ASSERT_EQ(f.started.wait_for(real_timeout), std::future_status::ready);
      EXPECT_TRUE(f.started.get().completed());
      EXPECT_EQ(f.classified.load(), actor_recovery_e::finished);
      EXPECT_EQ(f.store->retired(), 1);
      EXPECT_TRUE(f.store->stages().empty());
    }

    void
    wait_observed(fixture_t &f, int count) {
      const auto deadline = std::chrono::steady_clock::now() + real_timeout;
      while (f.observed.load() < count && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
      ASSERT_GE(f.observed.load(), count);
    }

    TEST(ActorStartupRecovery, ARecordVanishingWhileTheActorRunsKeepsTopologyFenced) {
      fixture_t f;
      f.write(launched());
      f.observation = process_observation_e::running;
      f.start();
      wait_observed(f, 2);
      f.store->vanish = true;
      std::this_thread::sleep_for(50ms);
      f.observation = process_observation_e::exited;
      std::this_thread::sleep_for(50ms);
      EXPECT_NE(f.started.wait_for(0ms), std::future_status::ready) << "a lost record never proves the actor stopped";
      EXPECT_TRUE(f.coordinator.status().external_recovery_pending);
      EXPECT_TRUE(f.log.events().empty());
      std::lock_guard lock { f.reports_mutex };
      EXPECT_FALSE(f.reports.empty());
    }

    TEST(ActorStartupRecovery, StoppingTheCoordinatorEndsTheWatcher) {
      fixture_t f;
      f.write(launched());
      f.observation = process_observation_e::running;
      f.start();
      wait_observed(f, 2);
      f.coordinator.stop();
      const auto deadline = std::chrono::steady_clock::now() + real_timeout;
      while (f.watchers_done.load() < 1 && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
      EXPECT_EQ(f.watchers_done.load(), 1) << "the watcher must not poll a running actor forever after stop";
      EXPECT_TRUE(f.log.events().empty());
      ASSERT_TRUE(f.durable().has_value());
      EXPECT_EQ(f.durable()->stage, actor_stage_e::launched);
    }

    TEST(ActorStartupRecovery, AMissingDeferralQueryNeverDischargesTheObligation) {
      fixture_t f;
      f.write(cleanup_pending());
      f.adjust = [](actor_startup_options_t &options) { options.restore_pending = {}; };
      f.start();
      ASSERT_EQ(f.started.wait_for(real_timeout), std::future_status::ready);
      EXPECT_FALSE(f.started.get().completed());
      EXPECT_TRUE(f.store->stages().empty());
      ASSERT_TRUE(f.durable().has_value());
      EXPECT_EQ(f.durable()->stage, actor_stage_e::cleanup_pending);
    }
  }  // namespace
}  // namespace display_device::topology
