/**
 * @file tests/unit/test_topology_adapter_restart.cpp
 * @brief Last-resort adapter restart through the supervised external actor owner.
 */
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <random>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "src/display_device/topology_adapter_restart.h"

namespace display_device::topology {
  namespace {
    using namespace std::chrono_literals;

    constexpr std::uint64_t op = 4242;
    constexpr actor_receipt_t actor_receipt { 7310, 133'300'000'000'000'000ULL };
    constexpr auto real_timeout = 5s;

    class temp_dir_t {
    public:
      temp_dir_t() {
        std::random_device random;
        path_ = std::filesystem::temp_directory_path() /
                ("sunshine-adapter-restart-" + std::to_string(random()) + "-" + std::to_string(random()));
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

    class store_t final: public actor_record_store_t {
    public:
      explicit store_t(std::filesystem::path path):
          inner_(std::move(path)) {}

      actor_load_t
      load() override {
        return inner_.load();
      }

      result_t<void>
      create(const actor_record_t &record) override {
        if (fail_create) return std::unexpected(error_t { error_code_e::resource_unavailable, "injected create failure", {} });
        return inner_.create(record);
      }

      result_t<void>
      replace(const actor_record_t &expected, const actor_record_t &next) override {
        return inner_.replace(expected, next);
      }

      result_t<void>
      retire(const actor_record_t &expected) override {
        return inner_.retire(expected);
      }

      std::atomic_bool fail_create {};

    private:
      file_actor_record_store_t inner_;
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

    /** Shared by the fake process and the test; outlives both. */
    struct actor_t {
      bool create_fails {};
      /** Attempts with a lower index fail creation. */
      int fail_first {};
      actor_wait_e ends { actor_wait_e::exited };
      std::promise<void> exit;
      std::shared_future<void> exited { exit.get_future().share() };
      std::atomic_int made {};
      std::atomic_int launched {};
      event_log_t *log {};
    };

    class process_t final: public actor_process_t {
    public:
      explicit process_t(std::shared_ptr<actor_t> actor):
          actor_ { std::move(actor) }, index_ { actor_->made - 1 } {}

      void
      launch(std::error_code &ec) override {
        if (actor_->create_fails || index_ < actor_->fail_first) {
          ec = std::make_error_code(std::errc::no_such_file_or_directory);
          return;
        }
        ++actor_->launched;
        started_ = true;
        if (actor_->log) actor_->log->add("actor");
      }

      bool
      started() const override {
        return started_;
      }

      std::optional<actor_receipt_t>
      receipt() override {
        return actor_receipt;
      }

      actor_wait_t
      wait() override {
        actor_->exited.wait();
        return { actor_->ends, 0u, "test actor ended" };
      }

    private:
      std::shared_ptr<actor_t> actor_;
      int index_ {};
      bool started_ {};
    };

    command_body_t
    logging(event_log_t &log, std::string event) {
      return [&log, event = std::move(event)](const mutation_capability_t &, const command_context_t &) {
        log.add(event);
      };
    }

    struct fixture_t {
      temp_dir_t dir;
      std::shared_ptr<store_t> store { std::make_shared<store_t>(dir.file()) };
      coordinator_t coordinator;
      event_log_t log;
      std::shared_ptr<actor_t> actor { std::make_shared<actor_t>() };
      std::atomic_int restores {};
      actor_spawn_t spawn;

      fixture_t() {
        actor->log = &log;
        EXPECT_TRUE(coordinator.start({}).get().completed());
      }

      ~fixture_t() {
        release();
        coordinator.stop();
      }

      void
      release() {
        try {
          actor->exit.set_value();
        }
        catch (const std::future_error &) {
        }
      }

      adapter_restart_plan_t
      plan() {
        adapter_restart_plan_t result;
        result.store = store;
        result.operation = op;
        result.boot = std::string { "boot-a" };
        result.process = [actor = actor]() -> std::shared_ptr<actor_process_t> {
          ++actor->made;
          return std::make_shared<process_t>(actor);
        };
        result.options.spawn = spawn;
        result.retry = retry;
        if (deadline.count() > 0) result.options.deadline = deadline;
        return result;
      }

      actor_launch_retry_t retry;
      std::chrono::milliseconds deadline {};

      external_cleanup_body_t
      restore(bool result = true) {
        return [this, result](const mutation_capability_t &capability, const command_context_t &) {
          EXPECT_EQ(capability.kind(), command_kind_e::launch_failed_restore);
          EXPECT_NO_THROW(capability.require(operation_e::restore_state));
          ++restores;
          log.add("restore");
          return result;
        };
      }

      /** Restart from a launch, then queue `prequeued` behind it while the launch still runs. */
      adapter_restart_result_t
      restart(posted_command_t &prequeued, command_kind_e kind = command_kind_e::reconfigure) {
        adapter_restart_result_t result;
        const auto ran = coordinator.run(kind, [&](const auto &capability, const auto &) {
          result = restart_adapter(coordinator, capability, plan(), restore());
          prequeued = coordinator.post_tracked(command_kind_e::shutdown_restore, logging(log, "prequeued"));
          log.add("launch-returned");
        });
        EXPECT_TRUE(ran.completed());
        return result;
      }
    };
  }  // namespace

  TEST(AdapterRestart, ExitedRestartRestoresFirstOnceThenRetiresTheRecord) {
    fixture_t f;
    posted_command_t prequeued;
    const auto result = f.restart(prequeued);
    ASSERT_EQ(result.status, adapter_restart_e::cleanup_owned) << result.detail;
    ASSERT_TRUE(result.launch);
    EXPECT_EQ(result.launch->status, actor_launch_e::launched);
    EXPECT_EQ(result.launch->receipt, actor_receipt);

    // The actor is still running: the restoration and everything queued wait for it.
    EXPECT_EQ(prequeued.completion.wait_for(100ms), std::future_status::timeout);
    EXPECT_EQ(f.restores, 0);
    EXPECT_EQ(f.store->load().status, actor_load_status_e::loaded);

    f.release();
    ASSERT_EQ(prequeued.completion.wait_for(real_timeout), std::future_status::ready);
    EXPECT_TRUE(prequeued.completion.get().completed());
    EXPECT_EQ(f.log.events(), (std::vector<std::string> { "actor", "launch-returned", "restore", "prequeued" }));
    EXPECT_EQ(f.restores, 1);
    EXPECT_EQ(f.actor->made, 1);
    EXPECT_EQ(f.store->load().status, actor_load_status_e::absent);
    // Once settled, nothing retains the supervisor.
    const auto deadline = std::chrono::steady_clock::now() + real_timeout;
    while (!result.supervisor.expired() && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(5ms);
    }
    EXPECT_TRUE(result.supervisor.expired());
  }

  TEST(AdapterRestart, DefiniteCreateFailureIsNotRetriedAndStillRestoresFirst) {
    fixture_t f;
    f.actor->create_fails = true;
    posted_command_t prequeued;
    const auto result = f.restart(prequeued);
    ASSERT_EQ(result.status, adapter_restart_e::cleanup_owned) << result.detail;
    ASSERT_TRUE(result.launch);
    EXPECT_EQ(result.launch->status, actor_launch_e::not_launched);

    ASSERT_EQ(prequeued.completion.wait_for(real_timeout), std::future_status::ready);
    EXPECT_EQ(f.log.events(), (std::vector<std::string> { "launch-returned", "restore", "prequeued" }));
    EXPECT_EQ(f.actor->made, 1);
    EXPECT_EQ(f.actor->launched, 0);
    EXPECT_EQ(f.store->load().status, actor_load_status_e::absent);
  }

  TEST(AdapterRestart, RetriedCreationLaunchesUnderOneIntentAndStillRestoresFirstOnce) {
    fixture_t f;
    f.actor->fail_first = 2;
    std::vector<unsigned> waits;
    f.retry = { 3, [&waits](unsigned failed) { waits.push_back(failed); } };
    posted_command_t prequeued;
    const auto result = f.restart(prequeued);
    ASSERT_EQ(result.status, adapter_restart_e::cleanup_owned) << result.detail;
    ASSERT_TRUE(result.launch);
    EXPECT_EQ(result.launch->status, actor_launch_e::launched);
    EXPECT_EQ(result.launch->receipt, actor_receipt);
    EXPECT_EQ(f.actor->made, 3);
    EXPECT_EQ(f.actor->launched, 1);
    EXPECT_EQ(waits, (std::vector<unsigned> { 0, 1 }));
    const auto running = f.store->load();
    ASSERT_TRUE(running.record);
    EXPECT_EQ(running.record->operation, op);
    EXPECT_EQ(running.record->receipt, actor_receipt);

    f.release();
    ASSERT_EQ(prequeued.completion.wait_for(real_timeout), std::future_status::ready);
    EXPECT_EQ(f.log.events(), (std::vector<std::string> { "actor", "launch-returned", "restore", "prequeued" }));
    EXPECT_EQ(f.restores, 1);
    EXPECT_EQ(f.store->load().status, actor_load_status_e::absent);
  }

  TEST(AdapterRestart, ExhaustedRetriesLaunchNothingAndStillRestoreFirstOnce) {
    fixture_t f;
    f.actor->create_fails = true;
    unsigned waited {};
    f.retry = { 3, [&waited](unsigned) { ++waited; } };
    posted_command_t prequeued;
    const auto result = f.restart(prequeued);
    ASSERT_EQ(result.status, adapter_restart_e::cleanup_owned) << result.detail;
    ASSERT_TRUE(result.launch);
    EXPECT_EQ(result.launch->status, actor_launch_e::not_launched);
    EXPECT_NE(result.launch->detail.find("attempt 3/3"), std::string::npos) << result.launch->detail;

    ASSERT_EQ(prequeued.completion.wait_for(real_timeout), std::future_status::ready);
    EXPECT_EQ(f.log.events(), (std::vector<std::string> { "launch-returned", "restore", "prequeued" }));
    EXPECT_EQ(f.actor->made, 3);
    EXPECT_EQ(waited, 2u);
    EXPECT_EQ(f.restores, 1);
    EXPECT_EQ(f.store->load().status, actor_load_status_e::absent);
  }

  TEST(AdapterRestart, TheBackoffHoldsTheFenceAndALateCreationIsUncertainNotRetriedByTheCaller) {
    fixture_t f;
    f.actor->fail_first = 1;
    f.deadline = 200ms;
    std::promise<void> resume;
    std::shared_future<void> resumed = resume.get_future().share();
    f.retry = { 3, [resumed](unsigned) { resumed.wait(); } };
    posted_command_t prequeued;
    const auto result = f.restart(prequeued);
    ASSERT_EQ(result.status, adapter_restart_e::cleanup_owned) << result.detail;
    ASSERT_TRUE(result.launch);
    EXPECT_EQ(result.launch->status, actor_launch_e::uncertain);

    // Between attempts the intent is durable and everything queued stays fenced.
    EXPECT_EQ(prequeued.completion.wait_for(200ms), std::future_status::timeout);
    const auto between = f.store->load();
    ASSERT_TRUE(between.record);
    EXPECT_EQ(between.record->stage, actor_stage_e::intent);
    EXPECT_EQ(f.restores, 0);

    resume.set_value();
    f.release();
    ASSERT_EQ(prequeued.completion.wait_for(real_timeout), std::future_status::ready);
    EXPECT_EQ(f.log.events(), (std::vector<std::string> { "launch-returned", "actor", "restore", "prequeued" }));
    EXPECT_EQ(f.actor->made, 2);
    EXPECT_EQ(f.actor->launched, 1);
    EXPECT_EQ(f.restores, 1);
    EXPECT_EQ(f.store->load().status, actor_load_status_e::absent);
  }

  TEST(AdapterRestart, SettlementBeforeTheRestorationIsSuppliedStillRestoresFirstOnce) {
    fixture_t f;
    f.actor->create_fails = true;
    // The body runs inside start, so the hand-off reaches the relay before the restoration does.
    f.spawn = [](std::function<void()> work) {
      work();
    };
    posted_command_t prequeued;
    const auto result = f.restart(prequeued);
    ASSERT_EQ(result.status, adapter_restart_e::cleanup_owned) << result.detail;
    ASSERT_TRUE(result.launch);
    EXPECT_EQ(result.launch->status, actor_launch_e::not_launched);

    ASSERT_EQ(prequeued.completion.wait_for(real_timeout), std::future_status::ready);
    EXPECT_EQ(f.log.events(), (std::vector<std::string> { "launch-returned", "restore", "prequeued" }));
    EXPECT_EQ(f.restores, 1);
    EXPECT_EQ(f.actor->made, 1);
    EXPECT_EQ(f.store->load().status, actor_load_status_e::absent);
  }

  TEST(AdapterRestart, StopWhileTheActorRunsNeverRestoresAndKeepsTheRecordOwed) {
    fixture_t f;
    posted_command_t prequeued;
    const auto result = f.restart(prequeued);
    ASSERT_EQ(result.status, adapter_restart_e::cleanup_owned) << result.detail;

    auto stopped = std::async(std::launch::async, [&] {
      f.coordinator.stop();
    });
    ASSERT_EQ(stopped.wait_for(real_timeout), std::future_status::ready);
    f.release();
    std::this_thread::sleep_for(200ms);

    EXPECT_EQ(f.restores, 0);
    EXPECT_EQ(f.actor->made, 1);
    // The relay could not queue the restoration, so the durable record still owes it.
    const auto load = f.store->load();
    ASSERT_EQ(load.status, actor_load_status_e::loaded);
    ASSERT_TRUE(load.record);
    EXPECT_EQ(load.record->stage, actor_stage_e::cleanup_pending);
  }

  TEST(AdapterRestart, UnpersistedIntentLaunchesNothingAndStillRestoresFirst) {
    fixture_t f;
    f.store->fail_create = true;
    posted_command_t prequeued;
    const auto result = f.restart(prequeued);
    ASSERT_EQ(result.status, adapter_restart_e::cleanup_owned) << result.detail;
    EXPECT_FALSE(result.launch);

    ASSERT_EQ(prequeued.completion.wait_for(real_timeout), std::future_status::ready);
    EXPECT_EQ(f.log.events(), (std::vector<std::string> { "launch-returned", "restore", "prequeued" }));
    EXPECT_EQ(f.actor->made, 0);
    EXPECT_EQ(f.store->load().status, actor_load_status_e::absent);
  }

  TEST(AdapterRestart, UnobservableActorKeepsTheRestorationAndEverythingQueuedHeld) {
    fixture_t f;
    f.actor->ends = actor_wait_e::failed;
    posted_command_t prequeued;
    const auto result = f.restart(prequeued);
    ASSERT_EQ(result.status, adapter_restart_e::cleanup_owned) << result.detail;
    f.release();

    EXPECT_EQ(prequeued.completion.wait_for(300ms), std::future_status::timeout);
    EXPECT_EQ(f.restores, 0);
    EXPECT_EQ(f.store->load().status, actor_load_status_e::loaded);
    // Nobody else holds the blocked supervisor, yet it stays alive with its exclusion.
    const auto supervisor = result.supervisor.lock();
    ASSERT_TRUE(supervisor);
    EXPECT_EQ(supervisor->snapshot().phase, actor_phase_e::blocked);
  }

  TEST(AdapterRestart, AnEarlierActorStillHoldingTheExclusionThrowsBeforeAnySideEffect) {
    fixture_t f;
    std::shared_ptr<external_cleanup_t> earlier;
    bool threw = false;
    f.coordinator.run(command_kind_e::reconfigure, [&](const auto &capability, const auto &) {
      earlier = f.coordinator.begin_external_cleanup(capability);
      try {
        restart_adapter(f.coordinator, capability, f.plan(), f.restore());
      }
      catch (const capability_error_t &) {
        threw = true;
      }
    });
    EXPECT_TRUE(threw);
    EXPECT_EQ(f.actor->made, 0);
    EXPECT_EQ(f.restores, 0);
    EXPECT_EQ(f.store->load().status, actor_load_status_e::absent);
  }

  TEST(AdapterRestart, AnUndischargedEarlierObligationThrowsBeforeAnySideEffect) {
    fixture_t f;
    std::shared_ptr<external_cleanup_t> earlier;
    bool threw = false;
    f.coordinator.run(command_kind_e::reconfigure, [&](const auto &capability, const auto &) {
      earlier = f.coordinator.begin_external_cleanup(capability);
      // Queued but not yet run: the exclusion is released, the obligation is not discharged.
      EXPECT_TRUE(earlier->queue_first([](const auto &, const auto &) { return true; }));
      try {
        (void) restart_adapter(f.coordinator, capability, f.plan(), f.restore());
      }
      catch (const std::logic_error &) {
        threw = true;
      }
    });
    EXPECT_TRUE(threw);
    EXPECT_EQ(f.actor->made, 0);
    EXPECT_EQ(f.restores, 0);
    EXPECT_EQ(f.store->load().status, actor_load_status_e::absent);
  }

  TEST(AdapterRestart, AnotherCoordinatorsAuthorityThrowsBeforeAnySideEffect) {
    fixture_t f;
    coordinator_t other;
    ASSERT_TRUE(other.start({}).get().completed());
    bool threw = false;
    EXPECT_TRUE(other.run(command_kind_e::reconfigure, [&](const auto &capability, const auto &) {
      try {
        (void) restart_adapter(f.coordinator, capability, f.plan(), f.restore());
      }
      catch (const std::logic_error &) {
        threw = true;
      }
    }).completed());
    other.stop();
    EXPECT_TRUE(threw);
    EXPECT_EQ(f.actor->made, 0);
    EXPECT_EQ(f.store->load().status, actor_load_status_e::absent);
    posted_command_t later = f.coordinator.post_tracked(command_kind_e::shutdown_restore, logging(f.log, "later"));
    ASSERT_EQ(later.completion.wait_for(real_timeout), std::future_status::ready);
    EXPECT_TRUE(later.completion.get().completed());
  }

  TEST(AdapterRestart, MissingPiecesRefuseBeforeAnySideEffect) {
    for (int missing = 0; missing < 3; ++missing) {
      SCOPED_TRACE(missing);
      fixture_t f;
      auto plan = f.plan();
      auto restore = f.restore();
      if (missing == 0) plan.store.reset();
      if (missing == 1) plan.process = {};
      if (missing == 2) restore = {};
      adapter_restart_result_t result;
      posted_command_t later;
      EXPECT_TRUE(f.coordinator.run(command_kind_e::reconfigure, [&](const auto &capability, const auto &) {
        result = restart_adapter(f.coordinator, capability, std::move(plan), std::move(restore));
      }).completed());
      EXPECT_EQ(result.status, adapter_restart_e::not_started);
      EXPECT_EQ(f.actor->made, 0);
      EXPECT_EQ(f.store->load().status, actor_load_status_e::absent);
      // No obligation was taken, so nothing is fenced.
      later = f.coordinator.post_tracked(command_kind_e::shutdown_restore, logging(f.log, "later"));
      ASSERT_EQ(later.completion.wait_for(real_timeout), std::future_status::ready);
      EXPECT_TRUE(later.completion.get().completed());
    }
  }

  TEST(AdapterRestart, AuthorityWithoutAdapterRestartThrowsBeforeAnySideEffect) {
    for (const bool complete : { true, false }) {
      SCOPED_TRACE(complete);
      fixture_t f;
      auto plan = f.plan();
      if (!complete) plan.process = {};
      bool threw = false;
      EXPECT_TRUE(f.coordinator.run(command_kind_e::rotate, [&](const auto &capability, const auto &) {
        try {
          (void) restart_adapter(f.coordinator, capability, std::move(plan), f.restore());
        }
        catch (const capability_error_t &) {
          threw = true;
        }
      }).completed());
      EXPECT_TRUE(threw);
      EXPECT_EQ(f.actor->made, 0);
      EXPECT_EQ(f.store->load().status, actor_load_status_e::absent);
    }
  }

}  // namespace display_device::topology
