/**
 * @file tests/unit/test_topology_actor_cleanup.cpp
 * @brief Relay of a settled external actor's durable cleanup into its coordinator.
 */
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "src/display_device/topology_actor_cleanup.h"

namespace display_device::topology {
  namespace {
    using namespace std::chrono_literals;

    constexpr std::uint64_t op = 91;
    constexpr actor_receipt_t receipt { 6160, 133'200'000'000'000'000ULL };
    constexpr auto real_timeout = 5s;

    actor_record_t
    intent(std::uint64_t operation = op) {
      return *begin_actor_intent(operation, std::string { "boot-a" }, actor_obligation_e::restore_legacy_display, "adapter restart");
    }

    actor_record_t
    pending(std::uint64_t operation = op) {
      return *settle_actor(*record_actor_receipt(intent(operation), operation, receipt), operation);
    }

    class temp_dir_t {
    public:
      temp_dir_t() {
        std::random_device random;
        path_ = std::filesystem::temp_directory_path() /
                ("sunshine-actor-cleanup-" + std::to_string(random()) + "-" + std::to_string(random()));
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

    class faulty_store_t final: public actor_record_store_t {
    public:
      explicit faulty_store_t(std::filesystem::path path):
          inner_(std::move(path)) {}

      actor_load_t
      load() override {
        return inner_.load();
      }

      result_t<void>
      create(const actor_record_t &record) override {
        return inner_.create(record);
      }

      result_t<void>
      replace(const actor_record_t &expected, const actor_record_t &next) override {
        if (fail_replace) return std::unexpected(error_t { error_code_e::resource_unavailable, "injected replace failure", {} });
        auto written = inner_.replace(expected, next);
        if (written && commit_then_throw) throw std::runtime_error { "injected exception after commit" };
        if (written && commit_then_fail) {
          return std::unexpected(error_t { error_code_e::resource_unavailable, "injected failure after commit", {} });
        }
        return written;
      }

      result_t<void>
      retire(const actor_record_t &expected) override {
        if (throw_retire) throw std::runtime_error { "injected retire exception" };
        return inner_.retire(expected);
      }

      std::atomic_bool fail_replace {};
      std::atomic_bool commit_then_fail {};
      std::atomic_bool commit_then_throw {};
      std::atomic_bool throw_retire {};

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

    command_body_t
    logging(event_log_t &log, std::string event) {
      return [&log, event = std::move(event)](const mutation_capability_t &, const command_context_t &) {
        log.add(event);
      };
    }

    external_cleanup_body_t
    cleaning(event_log_t &log, std::string event, bool result = true) {
      return [&log, event = std::move(event), result](const mutation_capability_t &capability, const command_context_t &) {
        EXPECT_EQ(capability.kind(), command_kind_e::launch_failed_restore);
        EXPECT_NO_THROW(capability.require(operation_e::restore_state));
        log.add(event);
        return result;
      };
    }

    struct fixture_t {
      temp_dir_t dir;
      std::shared_ptr<faulty_store_t> store { std::make_shared<faulty_store_t>(dir.file()) };
      coordinator_t coordinator;
      std::shared_ptr<external_cleanup_t> cleanup;
      event_log_t log;

      fixture_t() {
        EXPECT_TRUE(coordinator.start({}).get().completed());
        EXPECT_TRUE(coordinator.run(command_kind_e::reconfigure, [&](const auto &capability, const auto &) {
          cleanup = coordinator.begin_external_cleanup(capability);
        }).completed());
      }

      std::shared_ptr<actor_cleanup_relay_t>
      relay(std::uint64_t operation = op) {
        return std::make_shared<actor_cleanup_relay_t>(cleanup, store, operation);
      }

      void
      persist(const actor_record_t &record) {
        // The store only creates intents; walk the legal chain up to `record`.
        auto start = intent(record.operation);
        start.obligation_detail = record.obligation_detail;
        ASSERT_TRUE(store->create(start));
        if (record == start) return;
        const auto launched = *record_actor_receipt(start, record.operation, receipt);
        ASSERT_TRUE(store->replace(start, launched));
        if (record == launched) return;
        ASSERT_EQ(record, *settle_actor(launched, record.operation));
        ASSERT_TRUE(store->replace(launched, record));
      }

      bool
      flush() {
        const auto flushed = coordinator.post_tracked(command_kind_e::stream_restore, {});
        return flushed.completion.wait_for(real_timeout) == std::future_status::ready && flushed.completion.get().completed();
      }

      std::optional<actor_record_t>
      durable() {
        return store->load().record;
      }
    };
  }  // namespace

  TEST(ActorCleanupRelay, HandoffThenBodyQueuesAheadOfPendingCleanupAndCompletesDurably) {
    fixture_t f;
    f.persist(pending());
    auto relay = f.relay();
    auto supervisor = relay->exclusion();
    ASSERT_NE(supervisor, nullptr);
    const auto prequeued = f.coordinator.post_tracked(command_kind_e::shutdown_restore, logging(f.log, "prequeued"));
    ASSERT_EQ(prequeued.reason, reject_reason_e::none);
    relay->accept({ pending(), std::move(supervisor) });
    EXPECT_EQ(relay->state(), actor_cleanup_state_e::awaiting);
    EXPECT_EQ(prequeued.completion.wait_for(50ms), std::future_status::timeout);

    EXPECT_TRUE(relay->submit(cleaning(f.log, "actor-cleanup")));
    ASSERT_EQ(prequeued.completion.wait_for(real_timeout), std::future_status::ready);
    EXPECT_TRUE(prequeued.completion.get().completed());
    EXPECT_EQ(f.log.events(), (std::vector<std::string> { "actor-cleanup", "prequeued" }));
    EXPECT_EQ(relay->state(), actor_cleanup_state_e::completed);
    EXPECT_TRUE(f.cleanup->discharged());
    EXPECT_EQ(f.store->load().status, actor_load_status_e::absent);
    EXPECT_FALSE(f.coordinator.status().fence_active);
  }

  TEST(ActorCleanupRelay, BodyBeforeHandoffWaitsAndTheSettlingThreadQueuesIt) {
    fixture_t f;
    f.persist(pending());
    auto relay = f.relay();
    auto supervisor = relay->exclusion();
    EXPECT_TRUE(relay->submit(cleaning(f.log, "actor-cleanup")));
    EXPECT_EQ(relay->state(), actor_cleanup_state_e::awaiting);
    const auto prequeued = f.coordinator.post_tracked(command_kind_e::stream_restore, logging(f.log, "prequeued"));
    EXPECT_EQ(prequeued.completion.wait_for(50ms), std::future_status::timeout);
    EXPECT_TRUE(f.log.events().empty());
    std::thread settling { [&relay, supervisor = std::move(supervisor)]() mutable {
      relay->accept({ pending(), std::move(supervisor) });
    } };
    settling.join();
    ASSERT_EQ(prequeued.completion.wait_for(real_timeout), std::future_status::ready);
    EXPECT_EQ(f.log.events(), (std::vector<std::string> { "actor-cleanup", "prequeued" }));
    EXPECT_EQ(relay->state(), actor_cleanup_state_e::completed);
    EXPECT_EQ(f.store->load().status, actor_load_status_e::absent);
    EXPECT_FALSE(f.coordinator.status().fence_active);
  }

  TEST(ActorCleanupRelay, FailedCleanupLeavesTheRecordPendingAndAdmissionFenced) {
    for (const bool throws : { false, true }) {
      SCOPED_TRACE(throws);
      fixture_t f;
      f.persist(pending());
      auto relay = f.relay();
      relay->accept({ pending(), relay->exclusion() });
      EXPECT_TRUE(relay->submit([&](const auto &, const auto &) -> bool {
        f.log.add("actor-cleanup");
        if (throws) throw std::runtime_error { "restore failed" };
        return false;
      }));
      ASSERT_TRUE(f.flush());
      EXPECT_EQ(f.log.events(), (std::vector<std::string> { "actor-cleanup" }));
      EXPECT_EQ(relay->state(), actor_cleanup_state_e::failed);
      EXPECT_EQ(f.durable(), pending());
      EXPECT_FALSE(f.cleanup->discharged());
      EXPECT_TRUE(f.coordinator.status().fence_active);
      EXPECT_EQ(f.coordinator.admit_launch({ 470, "cleanup owed" }).reason, reject_reason_e::fence_active);
    }
  }

  TEST(ActorCleanupRelay, MismatchedHandoffIsRefusedAndItsExclusionRetained) {
    for (const auto &wrong : { pending(op + 1), *record_actor_receipt(intent(), op, receipt) }) {
      SCOPED_TRACE(to_string(wrong.stage));
      fixture_t f;
      f.persist(pending());
      auto relay = f.relay();
      // The obligation's own exclusion: only the record is wrong.
      auto exclusion = relay->exclusion();
      std::weak_ptr<void> watched = exclusion;
      relay->accept({ wrong, std::move(exclusion) });
      EXPECT_EQ(relay->state(), actor_cleanup_state_e::refused);
      EXPECT_FALSE(watched.expired());
      EXPECT_FALSE(relay->submit(cleaning(f.log, "actor-cleanup")));
      relay->accept({ pending(), relay->exclusion() });
      EXPECT_EQ(relay->state(), actor_cleanup_state_e::refused);
      EXPECT_TRUE(f.log.events().empty());
      EXPECT_FALSE(watched.expired());
      EXPECT_TRUE(f.coordinator.status().fence_active);
      EXPECT_EQ(f.durable(), pending());
    }
  }

  TEST(ActorCleanupRelay, NothingIsQueuedOrKeptOnceRefused) {
    {
      SCOPED_TRACE("late valid hand-off");
      fixture_t f;
      f.persist(pending());
      auto relay = f.relay();
      EXPECT_TRUE(relay->submit(cleaning(f.log, "supplied-early")));
      // Refused for a foreign exclusion, so the obligation's own token is still unquarantined.
      relay->accept({ pending(), std::make_shared<int>(6) });
      ASSERT_EQ(relay->state(), actor_cleanup_state_e::refused);
      relay->accept({ pending(), relay->exclusion() });
      EXPECT_EQ(relay->state(), actor_cleanup_state_e::refused);
      const auto later = f.coordinator.post_tracked(command_kind_e::stream_restore, logging(f.log, "later"));
      EXPECT_EQ(later.completion.wait_for(100ms), std::future_status::timeout);
      EXPECT_TRUE(f.log.events().empty());
      EXPECT_FALSE(f.cleanup->discharged());
      EXPECT_EQ(f.durable(), pending());
    }
    {
      SCOPED_TRACE("body after refusal");
      fixture_t f;
      f.persist(pending());
      auto relay = f.relay();
      relay->accept({ pending(), std::make_shared<int>(7) });
      ASSERT_EQ(relay->state(), actor_cleanup_state_e::refused);
      auto token = std::make_shared<int>(8);
      std::weak_ptr<int> watched = token;
      EXPECT_FALSE(relay->submit([&f, token = std::move(token)](const auto &, const auto &) {
        f.log.add("late");
        return true;
      }));
      // A rejected body's captures are released at once, not kept by the relay.
      EXPECT_TRUE(watched.expired());
      EXPECT_TRUE(f.log.events().empty());
    }
  }

  TEST(ActorCleanupRelay, AHandoffWithoutTheObligationsOwnExclusionIsRefused) {
    for (const bool foreign : { false, true }) {
      SCOPED_TRACE(foreign);
      fixture_t f;
      f.persist(pending());
      auto relay = f.relay();
      std::shared_ptr<void> exclusion;
      if (foreign) exclusion = std::make_shared<int>(3);
      std::weak_ptr<void> watched = exclusion;
      relay->accept({ pending(), std::move(exclusion) });
      EXPECT_EQ(relay->state(), actor_cleanup_state_e::refused);
      EXPECT_FALSE(relay->submit(cleaning(f.log, "actor-cleanup")));
      const auto later = f.coordinator.post_tracked(command_kind_e::stream_restore, logging(f.log, "later"));
      // The unqueued obligation still holds dispatch: nothing runs, fail closed.
      EXPECT_EQ(later.completion.wait_for(50ms), std::future_status::timeout);
      EXPECT_TRUE(f.log.events().empty());
      EXPECT_EQ(relay->state(), actor_cleanup_state_e::refused);
      EXPECT_FALSE(f.cleanup->discharged());
      EXPECT_TRUE(f.coordinator.status().fence_active);
      EXPECT_EQ(f.durable(), pending());
      if (foreign) EXPECT_FALSE(watched.expired());
    }
  }

  TEST(ActorCleanupRelay, RefusedExclusionsOutliveTheRelay) {
    std::weak_ptr<void> mismatched;
    std::weak_ptr<void> duplicate;
    std::weak_ptr<void> after_stop;
    {
      fixture_t f;
      f.persist(pending());
      {
        auto relay = f.relay();
        auto wrong = std::make_shared<int>(4);
        mismatched = wrong;
        relay->accept({ pending(op + 1), std::move(wrong) });
      }
      {
        auto relay = f.relay();
        relay->accept({ pending(), relay->exclusion() });
        auto second = std::make_shared<int>(5);
        duplicate = second;
        relay->accept({ pending(), std::move(second) });
      }
      {
        auto relay = f.relay();
        auto exclusion = relay->exclusion();
        after_stop = exclusion;
        f.coordinator.stop();
        relay->accept({ pending(), std::move(exclusion) });
        EXPECT_FALSE(relay->submit(cleaning(f.log, "actor-cleanup")));
      }
    }
    EXPECT_FALSE(mismatched.expired());
    EXPECT_FALSE(duplicate.expired());
    EXPECT_FALSE(after_stop.expired());
  }

  TEST(ActorCleanupRelay, ASecondHandoffAfterTheFirstIsRetainedWithoutDisturbingIt) {
    fixture_t f;
    f.persist(pending());
    auto relay = f.relay();
    relay->accept({ pending(), relay->exclusion() });
    auto duplicate = std::make_shared<int>(2);
    std::weak_ptr<int> watched = duplicate;
    relay->accept({ pending(), std::move(duplicate) });
    EXPECT_TRUE(relay->submit(cleaning(f.log, "actor-cleanup")));
    ASSERT_TRUE(f.flush());
    EXPECT_EQ(relay->state(), actor_cleanup_state_e::completed);
    EXPECT_FALSE(watched.expired());
  }

  TEST(ActorCleanupRelay, CompletionNeedsTheFullRecordCompareAndReplace) {
    struct case_t {
      const char *name;
      bool fail_replace;
      bool commit_then_fail;
      bool foreign_record;
      actor_cleanup_state_e expected;
      bool commit_then_throw = false;
    };
    for (const auto &c : {
           case_t { "replace fails", true, false, false, actor_cleanup_state_e::failed },
           case_t { "committed but reported failure", false, true, false, actor_cleanup_state_e::completed },
           case_t { "committed then threw", false, false, false, actor_cleanup_state_e::completed, true },
           case_t { "record changed underneath", false, false, true, actor_cleanup_state_e::failed },
         }) {
      SCOPED_TRACE(c.name);
      fixture_t f;
      auto durable = pending();
      if (c.foreign_record) durable.obligation_detail = "another writer";
      f.persist(durable);
      auto relay = f.relay();
      relay->accept({ pending(), relay->exclusion() });
      f.store->fail_replace = c.fail_replace;
      f.store->commit_then_fail = c.commit_then_fail;
      f.store->commit_then_throw = c.commit_then_throw;
      EXPECT_TRUE(relay->submit(cleaning(f.log, "actor-cleanup")));
      ASSERT_TRUE(f.flush());
      EXPECT_EQ(relay->state(), c.expected);
      const bool completed = c.expected == actor_cleanup_state_e::completed;
      EXPECT_EQ(f.cleanup->discharged(), completed);
      EXPECT_EQ(f.coordinator.status().fence_active, !completed);
      if (!completed) EXPECT_EQ(f.durable(), durable);
    }
  }

  TEST(ActorCleanupRelay, RetireFailureStillDischargesACompletedCleanup) {
    fixture_t f;
    f.persist(pending());
    f.store->throw_retire = true;
    auto relay = f.relay();
    relay->accept({ pending(), relay->exclusion() });
    EXPECT_TRUE(relay->submit(cleaning(f.log, "actor-cleanup")));
    ASSERT_TRUE(f.flush());
    EXPECT_EQ(relay->state(), actor_cleanup_state_e::completed);
    ASSERT_TRUE(f.durable());
    EXPECT_EQ(f.durable()->stage, actor_stage_e::completed);
    EXPECT_FALSE(f.coordinator.status().fence_active);
  }

  TEST(ActorCleanupRelay, BodyIsAcceptedOnceAndNeverEmpty) {
    fixture_t f;
    f.persist(pending());
    auto relay = f.relay();
    EXPECT_FALSE(relay->submit({}));
    EXPECT_TRUE(relay->submit(cleaning(f.log, "first")));
    EXPECT_FALSE(relay->submit(cleaning(f.log, "second")));
    relay->accept({ pending(), relay->exclusion() });
    EXPECT_FALSE(relay->submit(cleaning(f.log, "third")));
    ASSERT_TRUE(f.flush());
    EXPECT_EQ(f.log.events(), (std::vector<std::string> { "first" }));
    EXPECT_EQ(relay->state(), actor_cleanup_state_e::completed);
  }

  TEST(ActorCleanupRelay, CoordinatorRefusalAfterStopRetainsTheHandoff) {
    fixture_t f;
    f.persist(pending());
    auto relay = f.relay();
    auto exclusion = relay->exclusion();
    std::weak_ptr<void> watched = exclusion;
    f.coordinator.stop();
    relay->accept({ pending(), std::move(exclusion) });
    EXPECT_FALSE(relay->submit(cleaning(f.log, "actor-cleanup")));
    EXPECT_EQ(relay->state(), actor_cleanup_state_e::refused);
    EXPECT_FALSE(watched.expired());
    EXPECT_TRUE(f.log.events().empty());
    EXPECT_EQ(f.durable(), pending());
  }

  TEST(ActorCleanupRelay, ASupervisedActorHandsItsCleanupToTheCoordinatorOnSettlement) {
    fixture_t f;
    auto relay = f.relay();
    std::shared_ptr<actor_supervisor_t> published;
    std::mutex exit_mutex;
    std::condition_variable exit_changed;
    bool may_exit = false;
    actor_supervisor_options_t options;
    options.cleanup_sink = relay;
    auto started = actor_supervisor_t::start(
      f.store, intent(), relay->exclusion(),
      [&](const std::shared_ptr<actor_supervisor_t> &supervisor) -> result_t<void> {
        published = supervisor;
        return {};
      },
      [&](actor_body_context_t &context) {
        EXPECT_TRUE(context.report_launched(receipt));
        std::unique_lock lock { exit_mutex };
        exit_changed.wait_for(lock, real_timeout, [&] { return may_exit; });
        return actor_body_result_t { actor_body_exit_e::exited, "exit 0" };
      },
      options);
    ASSERT_TRUE(started) << started.error().detail;
    EXPECT_EQ((*started)->wait_launch().status, actor_launch_e::launched);
    EXPECT_TRUE(relay->submit(cleaning(f.log, "actor-cleanup")));
    f.coordinator.begin_draining();
    const auto shutdown = f.coordinator.post_tracked(command_kind_e::shutdown_restore, logging(f.log, "shutdown"));
    ASSERT_EQ(shutdown.reason, reject_reason_e::none);
    EXPECT_EQ(shutdown.completion.wait_for(50ms), std::future_status::timeout);
    {
      std::lock_guard lock { exit_mutex };
      may_exit = true;
    }
    exit_changed.notify_all();
    ASSERT_TRUE((*started)->wait_finished(real_timeout));
    ASSERT_EQ(shutdown.completion.wait_for(real_timeout), std::future_status::ready);
    EXPECT_EQ(f.log.events(), (std::vector<std::string> { "actor-cleanup", "shutdown" }));
    EXPECT_EQ(relay->state(), actor_cleanup_state_e::completed);
    EXPECT_EQ(f.store->load().status, actor_load_status_e::absent);
    EXPECT_FALSE(f.coordinator.status().fence_active);
  }

}  // namespace display_device::topology
