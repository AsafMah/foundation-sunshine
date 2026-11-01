/**
 * @file tests/unit/test_topology_actor_supervisor.cpp
 * @brief Bounded strong supervision of an external topology actor.
 */
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <random>
#include <thread>

#include "src/display_device/topology_actor_supervisor.h"

namespace display_device::topology {
  namespace {
    using namespace std::chrono_literals;

    constexpr std::uint64_t op = 77;
    constexpr actor_receipt_t receipt { 5150, 133'100'000'000'000'000ULL };

    actor_record_t
    intent() {
      return *begin_actor_intent(op, std::string { "boot-a" }, actor_obligation_e::restore_legacy_display, "adapter restart");
    }

    class temp_dir_t {
    public:
      temp_dir_t() {
        std::random_device random;
        path_ = std::filesystem::temp_directory_path() /
                ("sunshine-actor-supervisor-" + std::to_string(random()) + "-" + std::to_string(random()));
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

    /** File store whose writes can be made to fail by target stage. */
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
        ++creates;
        std::this_thread::sleep_for(create_delay);
        if (fail_create) return std::unexpected(error_t { error_code_e::resource_unavailable, "injected create failure", {} });
        return inner_.create(record);
      }

      result_t<void>
      replace(const actor_record_t &expected, const actor_record_t &next) override {
        if (on_replace) on_replace();
        if (fail_replace_to && *fail_replace_to == next.stage) {
          return std::unexpected(error_t { error_code_e::resource_unavailable, "injected replace failure", {} });
        }
        if (throw_replace) throw std::runtime_error("injected replace exception");
        auto written = inner_.replace(expected, next);
        if (written && commit_then_fail_to && *commit_then_fail_to == next.stage) {
          return std::unexpected(error_t { error_code_e::resource_unavailable, "injected failure after commit", {} });
        }
        return written;
      }

      result_t<void>
      retire(const actor_record_t &expected) override {
        return inner_.retire(expected);
      }

      std::atomic<int> creates {};
      std::chrono::milliseconds create_delay {};
      std::atomic<bool> fail_create {};
      std::atomic<bool> throw_replace {};
      std::optional<actor_stage_e> fail_replace_to;
      std::optional<actor_stage_e> commit_then_fail_to;
      std::function<void()> on_replace;

    private:
      file_actor_record_store_t inner_;
    };

    /** One-shot gate a blocked body waits on. */
    class gate_t {
    public:
      void
      open() {
        {
          std::lock_guard lock { mutex_ };
          open_ = true;
        }
        changed_.notify_all();
      }

      bool
      wait(std::chrono::milliseconds timeout = 5s) {
        std::unique_lock lock { mutex_ };
        return changed_.wait_for(lock, timeout, [&] { return open_; });
      }

    private:
      std::mutex mutex_;
      std::condition_variable changed_;
      bool open_ {};
    };

    struct fixture_t {
      temp_dir_t dir;
      std::shared_ptr<faulty_store_t> store { std::make_shared<faulty_store_t>(dir.file()) };
      std::shared_ptr<int> exclusion { std::make_shared<int>(1) };
      std::shared_ptr<actor_supervisor_t> published;
      std::atomic<int> body_runs {};

      // The actor thread writes into dir; removing it mid-write can wedge remove_all on Windows.
      ~fixture_t() {
        if (published) EXPECT_TRUE(published->wait_finished(5s)) << "a test returned while its actor was still running";
      }

      actor_supervisor_t::publish_t
      publish() {
        return [this](const std::shared_ptr<actor_supervisor_t> &supervisor) -> result_t<void> {
          published = supervisor;
          return {};
        };
      }

      std::optional<actor_record_t>
      durable() {
        return store->load().record;
      }
    };

    actor_supervisor_options_t
    within(std::chrono::milliseconds deadline) {
      return actor_supervisor_options_t { deadline, {} };
    }

    template <class Predicate>
    bool
    eventually(Predicate predicate, std::chrono::milliseconds timeout = 5s) {
      const auto until = std::chrono::steady_clock::now() + timeout;
      while (std::chrono::steady_clock::now() < until) {
        if (predicate()) return true;
        std::this_thread::sleep_for(2ms);
      }
      return predicate();
    }
  }  // namespace

  TEST(ActorSupervisor, IntentIsDurableAndSupervisorPublishedBeforeTheBodyRuns) {
    fixture_t f;
    bool intent_before_publish = false;
    bool body_after_publish = false;
    auto started = actor_supervisor_t::start(
      f.store, intent(), f.exclusion,
      [&](const std::shared_ptr<actor_supervisor_t> &supervisor) -> result_t<void> {
        auto durable = f.durable();
        intent_before_publish = durable && durable->stage == actor_stage_e::intent && f.body_runs == 0;
        f.published = supervisor;
        return {};
      },
      [&](actor_body_context_t &) {
        body_after_publish = f.published != nullptr;
        ++f.body_runs;
        return actor_body_result_t { actor_body_exit_e::not_launched, "no process" };
      });
    ASSERT_TRUE(started) << started.error().detail;
    EXPECT_EQ(*started, f.published);
    ASSERT_TRUE((*started)->wait_finished(5s));
    EXPECT_TRUE(intent_before_publish);
    EXPECT_TRUE(body_after_publish);
    EXPECT_EQ(f.body_runs, 1);
  }

  TEST(ActorSupervisor, LaunchedResultCarriesTheDurableReceiptAndExitSettlesCleanup) {
    fixture_t f;
    gate_t exit_gate;
    auto started = actor_supervisor_t::start(f.store, intent(), f.exclusion, f.publish(), [&](actor_body_context_t &context) {
      ++f.body_runs;
      EXPECT_TRUE(context.report_launched(receipt));
      exit_gate.wait();
      return actor_body_result_t { actor_body_exit_e::exited, "exit 0" };
    });
    ASSERT_TRUE(started) << started.error().detail;
    const auto launch = (*started)->wait_launch();
    EXPECT_EQ(launch.status, actor_launch_e::launched);
    EXPECT_EQ(launch.receipt, receipt);
    auto durable = f.durable();
    ASSERT_TRUE(durable);
    EXPECT_EQ(durable->stage, actor_stage_e::launched);
    EXPECT_EQ(durable->receipt, receipt);
    EXPECT_EQ((*started)->snapshot().phase, actor_phase_e::running);

    exit_gate.open();
    const auto before_exit = std::chrono::steady_clock::now();
    ASSERT_TRUE((*started)->wait_finished(5s));
    // Waiters are woken by the settlement itself, not by their own timeout.
    EXPECT_LT(std::chrono::steady_clock::now() - before_exit, 2s);
    EXPECT_EQ((*started)->snapshot().phase, actor_phase_e::settled);
    durable = f.durable();
    ASSERT_TRUE(durable);
    EXPECT_EQ(durable->stage, actor_stage_e::cleanup_pending);
    EXPECT_EQ(durable->receipt, receipt);
    EXPECT_EQ((*started)->snapshot().record, *durable);
    EXPECT_EQ((*started)->wait_launch(), launch);
  }

  TEST(ActorSupervisor, DefiniteCreationFailureIsNotLaunchedAndStillOwesCleanup) {
    fixture_t f;
    auto started = actor_supervisor_t::start(f.store, intent(), f.exclusion, f.publish(), [&](actor_body_context_t &) {
      return actor_body_result_t { actor_body_exit_e::not_launched, "CreateProcess failed" };
    });
    ASSERT_TRUE(started);
    const auto launch = (*started)->wait_launch();
    EXPECT_EQ(launch.status, actor_launch_e::not_launched);
    EXPECT_FALSE(launch.receipt);
    ASSERT_TRUE((*started)->wait_finished(5s));
    EXPECT_EQ((*started)->snapshot().phase, actor_phase_e::settled);
    ASSERT_TRUE(f.durable());
    EXPECT_EQ(f.durable()->stage, actor_stage_e::cleanup_pending);
    EXPECT_FALSE(f.durable()->receipt);
  }

  TEST(ActorSupervisor, BlockedCreationTimesOutUncertainWithoutJoiningAndNeverBecomesALateSuccess) {
    fixture_t f;
    gate_t create_gate;
    std::weak_ptr<int> exclusion = f.exclusion;
    auto started = actor_supervisor_t::start(f.store, intent(), std::move(f.exclusion), f.publish(), [&](actor_body_context_t &context) {
      ++f.body_runs;
      create_gate.wait();
      context.report_launched(receipt);
      return actor_body_result_t { actor_body_exit_e::exited, "late" };
    },
      within(60ms));
    ASSERT_TRUE(started);
    const auto before = std::chrono::steady_clock::now();
    const auto launch = (*started)->wait_launch();
    EXPECT_LT(std::chrono::steady_clock::now() - before, 2s);
    EXPECT_EQ(launch.status, actor_launch_e::uncertain);
    EXPECT_FALSE(launch.receipt);
    EXPECT_FALSE(exclusion.expired());
    EXPECT_EQ(f.durable()->stage, actor_stage_e::intent);

    create_gate.open();
    ASSERT_TRUE((*started)->wait_finished(5s));
    EXPECT_EQ((*started)->wait_launch(), launch);
    EXPECT_EQ((*started)->snapshot().launch, launch);
    EXPECT_EQ((*started)->snapshot().phase, actor_phase_e::settled);
    EXPECT_EQ(f.durable()->stage, actor_stage_e::cleanup_pending);
    EXPECT_EQ(f.durable()->receipt, receipt);
    EXPECT_EQ(f.body_runs, 1);
  }

  TEST(ActorSupervisor, ADecisionPastTheDeadlineIsUncertainEvenWithoutAWaiter) {
    fixture_t f;
    auto started = actor_supervisor_t::start(f.store, intent(), f.exclusion, f.publish(), [&](actor_body_context_t &context) {
      std::this_thread::sleep_for(120ms);
      context.report_launched(receipt);
      return actor_body_result_t { actor_body_exit_e::exited, "" };
    },
      within(20ms));
    ASSERT_TRUE(started);
    ASSERT_TRUE((*started)->wait_finished(5s));
    EXPECT_EQ((*started)->wait_launch().status, actor_launch_e::uncertain);
  }

  TEST(ActorSupervisor, LateDefiniteFailureIsAlsoUncertain) {
    fixture_t f;
    auto started = actor_supervisor_t::start(f.store, intent(), f.exclusion, f.publish(), [&](actor_body_context_t &) {
      std::this_thread::sleep_for(120ms);
      return actor_body_result_t { actor_body_exit_e::not_launched, "" };
    },
      within(20ms));
    ASSERT_TRUE(started);
    ASSERT_TRUE((*started)->wait_finished(5s));
    EXPECT_EQ((*started)->wait_launch().status, actor_launch_e::uncertain);
    EXPECT_EQ(f.durable()->stage, actor_stage_e::cleanup_pending);
  }

  TEST(ActorSupervisor, DeadlineIsClampedToTheAbsoluteLimit) {
    fixture_t f;
    gate_t gate;
    const auto before = std::chrono::steady_clock::now();
    auto started = actor_supervisor_t::start(f.store, intent(), f.exclusion, f.publish(), [&](actor_body_context_t &) {
      gate.wait();
      return actor_body_result_t { actor_body_exit_e::not_launched, "" };
    },
      within(std::chrono::hours { 1 }));
    const auto after = std::chrono::steady_clock::now();
    ASSERT_TRUE(started);
    // The deadline is anchored inside the call, so it includes the intent write.
    EXPECT_GE((*started)->deadline() - actor_launch_deadline_limit, before);
    EXPECT_LE((*started)->deadline() - actor_launch_deadline_limit, after);
    gate.open();
    ASSERT_TRUE((*started)->wait_finished(5s));
  }

  TEST(ActorSupervisor, NonPositiveDeadlineIsRejectedBeforeAnyEffect) {
    for (const auto deadline : { 0ms, -5ms }) {
      fixture_t f;
      bool published = false;
      auto started = actor_supervisor_t::start(f.store, intent(), f.exclusion, [&](const auto &) -> result_t<void> {
        published = true;
        return {};
      },
        [&](actor_body_context_t &) {
          ++f.body_runs;
          return actor_body_result_t {};
        },
        within(deadline));
      ASSERT_FALSE(started);
      EXPECT_EQ(started.error().code, error_code_e::invalid_deadline);
      EXPECT_FALSE(published);
      EXPECT_EQ(f.body_runs, 0);
      EXPECT_EQ(f.store->load().status, actor_load_status_e::absent);
    }
  }

  TEST(ActorSupervisor, OnlyAFreshIntentForThisOperationCanStart) {
    fixture_t f;
    auto launched = *record_actor_receipt(intent(), op, receipt);
    bool published = false;
    auto started = actor_supervisor_t::start(f.store, launched, f.exclusion, [&](const auto &) -> result_t<void> {
      published = true;
      return {};
    },
      [&](actor_body_context_t &) {
        ++f.body_runs;
        return actor_body_result_t {};
      });
    ASSERT_FALSE(started);
    EXPECT_EQ(started.error().code, error_code_e::invalid_transition);
    EXPECT_FALSE(published);
    EXPECT_EQ(f.body_runs, 0);
    // Rejected by the supervisor itself, not left to the store's own validation.
    EXPECT_EQ(f.store->creates, 0);
    EXPECT_EQ(f.store->load().status, actor_load_status_e::absent);
  }

  TEST(ActorSupervisor, AMissingPublisherOrBodyIsRejectedBeforeAnyEffect) {
    for (const bool missing_body : { false, true }) {
      fixture_t f;
      actor_supervisor_t::publish_t publish = missing_body ? f.publish() : actor_supervisor_t::publish_t {};
      actor_body_t body = missing_body ? actor_body_t {} : actor_body_t { [](actor_body_context_t &) {
        return actor_body_result_t {};
      } };
      auto started = actor_supervisor_t::start(f.store, intent(), f.exclusion, publish, std::move(body));
      ASSERT_FALSE(started);
      EXPECT_EQ(started.error().code, error_code_e::resource_unavailable);
      EXPECT_EQ(f.published, nullptr);
      EXPECT_EQ(f.store->creates, 0);
      EXPECT_EQ(f.store->load().status, actor_load_status_e::absent);
    }
  }

  TEST(ActorSupervisor, ASlowIntentWriteConsumesTheCallersDeadline) {
    fixture_t f;
    f.store->create_delay = 80ms;
    auto started = actor_supervisor_t::start(f.store, intent(), f.exclusion, f.publish(), [&](actor_body_context_t &context) {
      context.report_launched(receipt);
      return actor_body_result_t { actor_body_exit_e::exited, "" };
    },
      within(40ms));
    ASSERT_TRUE(started);
    EXPECT_EQ((*started)->wait_launch().status, actor_launch_e::uncertain);
    ASSERT_TRUE((*started)->wait_finished(5s));
  }

  TEST(ActorSupervisor, IntentWriteFailureNeverPublishesOrLaunches) {
    fixture_t f;
    f.store->fail_create = true;
    bool published = false;
    auto started = actor_supervisor_t::start(f.store, intent(), f.exclusion, [&](const auto &) -> result_t<void> {
      published = true;
      return {};
    },
      [&](actor_body_context_t &) {
        ++f.body_runs;
        return actor_body_result_t {};
      });
    ASSERT_FALSE(started);
    EXPECT_EQ(started.error().code, error_code_e::resource_unavailable);
    EXPECT_FALSE(published);
    EXPECT_EQ(f.body_runs, 0);
  }

  TEST(ActorSupervisor, AnExistingRecordIsNeverOverwritten) {
    fixture_t f;
    auto other = *begin_actor_intent(op + 1, std::string { "boot-a" }, actor_obligation_e::restore_legacy_display, "older");
    ASSERT_TRUE(f.store->create(other));
    auto started = actor_supervisor_t::start(f.store, intent(), f.exclusion, f.publish(), [&](actor_body_context_t &) {
      ++f.body_runs;
      return actor_body_result_t {};
    });
    ASSERT_FALSE(started);
    EXPECT_EQ(f.published, nullptr);
    EXPECT_EQ(f.body_runs, 0);
    EXPECT_EQ(f.durable(), other);
  }

  TEST(ActorSupervisor, PublishFailureNeverLaunchesAndSettlesTheIntent) {
    for (const bool throws : { false, true }) {
      fixture_t f;
      auto started = actor_supervisor_t::start(f.store, intent(), f.exclusion, [&](const auto &) -> result_t<void> {
        if (throws) throw std::runtime_error("publish threw");
        return std::unexpected(error_t { error_code_e::owner_busy, "busy", {} });
      },
        [&](actor_body_context_t &) {
          ++f.body_runs;
          return actor_body_result_t {};
        });
      ASSERT_FALSE(started);
      EXPECT_EQ(started.error().code, throws ? error_code_e::resource_unavailable : error_code_e::owner_busy);
      EXPECT_EQ(f.body_runs, 0);
      ASSERT_TRUE(f.durable());
      EXPECT_EQ(f.durable()->stage, actor_stage_e::cleanup_pending);
    }
  }

  TEST(ActorSupervisor, SpawnFailureIsADefiniteNonLaunch) {
    fixture_t f;
    actor_supervisor_options_t options;
    options.spawn = [](std::function<void()>) {
      throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
    };
    auto started = actor_supervisor_t::start(f.store, intent(), f.exclusion, f.publish(), [&](actor_body_context_t &) {
      ++f.body_runs;
      return actor_body_result_t {};
    },
      options);
    ASSERT_TRUE(started);
    EXPECT_EQ(*started, f.published);
    EXPECT_EQ((*started)->wait_launch().status, actor_launch_e::not_launched);
    ASSERT_TRUE((*started)->wait_finished(1s));
    EXPECT_EQ((*started)->snapshot().phase, actor_phase_e::settled);
    EXPECT_EQ(f.body_runs, 0);
    EXPECT_EQ(f.durable()->stage, actor_stage_e::cleanup_pending);
  }

  TEST(ActorSupervisor, APartiallyPublishedFailureLeavesAFinishedNeverLaunchedSupervisor) {
    for (const bool settle_fails : { false, true }) {
      fixture_t f;
      if (settle_fails) f.store->fail_replace_to = actor_stage_e::cleanup_pending;
      std::weak_ptr<int> exclusion = f.exclusion;
      auto started = actor_supervisor_t::start(f.store, intent(), std::move(f.exclusion),
        [&](const std::shared_ptr<actor_supervisor_t> &supervisor) -> result_t<void> {
          f.published = supervisor;
          return std::unexpected(error_t { error_code_e::owner_busy, "stored, then refused", {} });
        },
        [&](actor_body_context_t &) {
          ++f.body_runs;
          return actor_body_result_t {};
        });
      ASSERT_FALSE(started);
      ASSERT_TRUE(f.published);
      ASSERT_TRUE(f.published->wait_finished(0ms));
      const auto snapshot = f.published->snapshot();
      EXPECT_EQ(snapshot.phase, settle_fails ? actor_phase_e::blocked : actor_phase_e::settled);
      ASSERT_TRUE(snapshot.launch);
      EXPECT_EQ(snapshot.launch->status, actor_launch_e::not_launched);
      EXPECT_EQ(f.body_runs, 0);
      EXPECT_FALSE(exclusion.expired());
      EXPECT_EQ(f.durable()->stage, settle_fails ? actor_stage_e::intent : actor_stage_e::cleanup_pending);
    }
  }

  TEST(ActorSupervisor, AHostileSpawnerThatKeepsTheWorkCannotLaunchItLater) {
    fixture_t f;
    std::function<void()> kept;
    actor_supervisor_options_t options;
    options.spawn = [&](std::function<void()> work) {
      kept = std::move(work);
      throw std::runtime_error("queued, then threw");
    };
    auto started = actor_supervisor_t::start(f.store, intent(), f.exclusion, f.publish(), [&](actor_body_context_t &) {
      ++f.body_runs;
      return actor_body_result_t { actor_body_exit_e::not_launched, "" };
    },
      options);
    ASSERT_TRUE(started);
    ASSERT_TRUE(kept);
    kept();
    EXPECT_EQ(f.body_runs, 0);
    EXPECT_EQ((*started)->wait_launch().status, actor_launch_e::not_launched);
    EXPECT_EQ((*started)->snapshot().phase, actor_phase_e::settled);
  }

  TEST(ActorSupervisor, ASpawnerThatRanTheWorkBeforeThrowingIsNotANonLaunch) {
    fixture_t f;
    actor_supervisor_options_t options;
    options.spawn = [](std::function<void()> work) {
      work();
      throw std::runtime_error("ran, then threw");
    };
    auto started = actor_supervisor_t::start(f.store, intent(), f.exclusion, f.publish(), [&](actor_body_context_t &context) {
      ++f.body_runs;
      context.report_launched(receipt);
      return actor_body_result_t { actor_body_exit_e::exited, "" };
    },
      options);
    ASSERT_TRUE(started);
    EXPECT_EQ(f.body_runs, 1);
    EXPECT_EQ((*started)->wait_launch().status, actor_launch_e::launched);
    EXPECT_EQ((*started)->snapshot().phase, actor_phase_e::settled);
    EXPECT_EQ(f.durable()->receipt, receipt);
  }

  TEST(ActorSupervisor, ALeakedContextIsRejectedFromOtherThreadsAndAfterTheBodyReturns) {
    fixture_t f;
    actor_body_context_t *leaked = nullptr;
    bool foreign = true;
    auto started = actor_supervisor_t::start(f.store, intent(), f.exclusion, f.publish(), [&](actor_body_context_t &context) {
      leaked = &context;
      std::thread { [&] { foreign = context.report_launched(actor_receipt_t { 1, 1 }); } }.join();
      context.report_launched(receipt);
      return actor_body_result_t { actor_body_exit_e::exited, "" };
    });
    ASSERT_TRUE(started);
    ASSERT_TRUE((*started)->wait_finished(5s));
    EXPECT_FALSE(foreign);
    ASSERT_TRUE(leaked);
    EXPECT_FALSE(leaked->report_launched(actor_receipt_t { 2, 2 }));
    auto late = std::make_shared<int>(3);
    std::weak_ptr<int> late_weak = late;
    leaked->retain(std::move(late));
    EXPECT_TRUE(late_weak.expired());
    EXPECT_EQ((*started)->snapshot().phase, actor_phase_e::settled);
    EXPECT_EQ(f.durable()->stage, actor_stage_e::cleanup_pending);
    EXPECT_EQ(f.durable()->receipt, receipt);
  }

  TEST(ActorSupervisor, ALeakedContextIsRejectedOnTheActorThreadItselfAfterTheBodyReturns) {
    fixture_t f;
    actor_body_context_t *leaked = nullptr;
    bool late_report = true;
    std::weak_ptr<int> late_weak;
    actor_supervisor_options_t options;
    // Runs the work on this thread, then reuses the context on the very same thread.
    options.spawn = [&](std::function<void()> work) {
      work();
      late_report = leaked->report_launched(actor_receipt_t { 2, 2 });
      auto late = std::make_shared<int>(3);
      late_weak = late;
      leaked->retain(std::move(late));
    };
    auto started = actor_supervisor_t::start(f.store, intent(), f.exclusion, f.publish(), [&](actor_body_context_t &context) {
      leaked = &context;
      return actor_body_result_t { actor_body_exit_e::not_launched, "" };
    },
      options);
    ASSERT_TRUE(started);
    EXPECT_FALSE(late_report);
    EXPECT_TRUE(late_weak.expired());
    EXPECT_EQ((*started)->wait_launch().status, actor_launch_e::not_launched);
    EXPECT_EQ((*started)->snapshot().phase, actor_phase_e::settled);
    EXPECT_FALSE(f.durable()->receipt);
  }

  TEST(ActorSupervisor, TheBodyAndItsCapturesAreDestroyedOnTheActorThread) {
    struct sentinel_t {
      std::atomic<std::thread::id> *destroyed_on;
      ~sentinel_t() {
        destroyed_on->store(std::this_thread::get_id());
      }
    };
    fixture_t f;
    std::atomic<std::thread::id> destroyed_on {};
    std::atomic<std::thread::id> body_thread {};
    auto started = actor_supervisor_t::start(f.store, intent(), f.exclusion, f.publish(),
      [sentinel = std::make_shared<sentinel_t>(&destroyed_on), &body_thread](actor_body_context_t &context) {
        body_thread = std::this_thread::get_id();
        context.report_launched(receipt);
        return actor_body_result_t { actor_body_exit_e::exited, "" };
      });
    ASSERT_TRUE(started);
    ASSERT_TRUE(eventually([&] { return destroyed_on.load() != std::thread::id {}; }));
    EXPECT_EQ(destroyed_on.load(), body_thread.load());
    EXPECT_NE(destroyed_on.load(), std::this_thread::get_id());
    EXPECT_EQ(f.published, *started);
    ASSERT_TRUE((*started)->wait_finished(5s));
  }

  TEST(ActorSupervisor, APublishedSettledSupervisorKeepsTheExclusionUntilCleanupIsHandedOff) {
    fixture_t f;
    std::weak_ptr<int> exclusion = f.exclusion;
    auto started = actor_supervisor_t::start(f.store, intent(), std::move(f.exclusion), f.publish(), [&](actor_body_context_t &context) {
      context.report_launched(receipt);
      return actor_body_result_t { actor_body_exit_e::exited, "" };
    });
    ASSERT_TRUE(started);
    ASSERT_TRUE((*started)->wait_finished(5s));
    std::this_thread::sleep_for(20ms);
    EXPECT_FALSE(exclusion.expired());

    auto handoff = (*started)->hand_off_cleanup();
    ASSERT_TRUE(handoff) << handoff.error().detail;
    EXPECT_EQ(handoff->record, *f.durable());
    EXPECT_EQ(handoff->record.stage, actor_stage_e::cleanup_pending);
    EXPECT_EQ(handoff->exclusion, exclusion.lock());

    auto again = (*started)->hand_off_cleanup();
    ASSERT_FALSE(again);
    EXPECT_EQ(again.error().code, error_code_e::already_finalized);

    handoff->exclusion.reset();
    EXPECT_TRUE(exclusion.expired());
  }

  TEST(ActorSupervisor, CleanupCannotBeHandedOffBeforeSettlementOrWhenBlocked) {
    fixture_t f;
    gate_t gate;
    std::weak_ptr<int> exclusion = f.exclusion;
    auto started = actor_supervisor_t::start(f.store, intent(), std::move(f.exclusion), f.publish(), [&](actor_body_context_t &context) {
      context.report_launched(receipt);
      gate.wait();
      return actor_body_result_t { actor_body_exit_e::unobservable, "" };
    });
    ASSERT_TRUE(started);
    EXPECT_EQ((*started)->wait_launch().status, actor_launch_e::launched);
    auto running = (*started)->hand_off_cleanup();
    ASSERT_FALSE(running);
    EXPECT_EQ(running.error().code, error_code_e::invalid_transition);
    gate.open();
    ASSERT_TRUE((*started)->wait_finished(5s));
    auto blocked = (*started)->hand_off_cleanup();
    ASSERT_FALSE(blocked);
    EXPECT_EQ(blocked.error().code, error_code_e::invalid_transition);
    EXPECT_FALSE(exclusion.expired());
  }

  TEST(ActorSupervisor, AReceiptWriteThatCommittedDespiteAnErrorIsReconciled) {
    fixture_t f;
    f.store->commit_then_fail_to = actor_stage_e::launched;
    bool reported = false;
    auto started = actor_supervisor_t::start(f.store, intent(), f.exclusion, f.publish(), [&](actor_body_context_t &context) {
      reported = context.report_launched(receipt);
      return actor_body_result_t { actor_body_exit_e::exited, "" };
    });
    ASSERT_TRUE(started);
    ASSERT_TRUE((*started)->wait_finished(5s));
    EXPECT_TRUE(reported);
    EXPECT_EQ((*started)->wait_launch().status, actor_launch_e::launched);
    EXPECT_EQ((*started)->snapshot().phase, actor_phase_e::settled);
    EXPECT_EQ(f.durable()->stage, actor_stage_e::cleanup_pending);
    EXPECT_EQ(f.durable()->receipt, receipt);
  }

  TEST(ActorSupervisor, ASettleWriteThatCommittedDespiteAnErrorIsReconciled) {
    fixture_t f;
    f.store->commit_then_fail_to = actor_stage_e::cleanup_pending;
    auto started = actor_supervisor_t::start(f.store, intent(), f.exclusion, f.publish(), [&](actor_body_context_t &context) {
      context.report_launched(receipt);
      return actor_body_result_t { actor_body_exit_e::exited, "" };
    });
    ASSERT_TRUE(started);
    ASSERT_TRUE((*started)->wait_finished(5s));
    EXPECT_EQ((*started)->snapshot().phase, actor_phase_e::settled);
    EXPECT_EQ((*started)->snapshot().record, *f.durable());
  }

  TEST(ActorSupervisor, AnUnexpectedDurableRecordAfterAFailedWriteBlocks) {
    fixture_t f;
    f.store->fail_replace_to = actor_stage_e::launched;
    std::shared_ptr<actor_record_store_t> intruder = std::make_shared<file_actor_record_store_t>(f.dir.file());
    f.store->on_replace = [&] {
      auto current = intruder->load().record;
      if (current && current->stage == actor_stage_e::intent) {
        (void) intruder->replace(*current, *settle_actor(*current, op));
      }
    };
    auto started = actor_supervisor_t::start(f.store, intent(), f.exclusion, f.publish(), [&](actor_body_context_t &context) {
      context.report_launched(receipt);
      return actor_body_result_t { actor_body_exit_e::exited, "" };
    });
    ASSERT_TRUE(started);
    ASSERT_TRUE((*started)->wait_finished(5s));
    EXPECT_EQ((*started)->snapshot().phase, actor_phase_e::blocked);
  }

  TEST(ActorSupervisor, StoreCallsRunOutsideTheSupervisorLock) {
    fixture_t f;
    std::atomic<int> reentered {};
    f.store->on_replace = [&] {
      if (f.published) {
        (void) f.published->snapshot();
        ++reentered;
      }
    };
    auto started = actor_supervisor_t::start(f.store, intent(), f.exclusion, f.publish(), [&](actor_body_context_t &context) {
      context.report_launched(receipt);
      return actor_body_result_t { actor_body_exit_e::exited, "" };
    });
    ASSERT_TRUE(started);
    ASSERT_TRUE((*started)->wait_finished(5s));
    EXPECT_EQ(reentered, 2);
    EXPECT_EQ((*started)->snapshot().phase, actor_phase_e::settled);
  }

  TEST(ActorSupervisor, SlowSynchronousStartWorkConsumesTheCallersDeadline) {
    fixture_t f;
    const auto before = std::chrono::steady_clock::now();
    auto started = actor_supervisor_t::start(f.store, intent(), f.exclusion, [&](const std::shared_ptr<actor_supervisor_t> &supervisor) -> result_t<void> {
      std::this_thread::sleep_for(80ms);
      f.published = supervisor;
      return {};
    },
      [&](actor_body_context_t &context) {
        context.report_launched(receipt);
        return actor_body_result_t { actor_body_exit_e::exited, "" };
      },
      within(40ms));
    ASSERT_TRUE(started);
    EXPECT_EQ((*started)->wait_launch().status, actor_launch_e::uncertain);
    EXPECT_LT(std::chrono::steady_clock::now() - before, 2s);
    ASSERT_TRUE((*started)->wait_finished(5s));
  }

  TEST(ActorSupervisor, BodyExceptionBeforeTheReceiptBlocksAndRetainsEverything) {
    fixture_t f;
    auto handles = std::make_shared<int>(9);
    std::weak_ptr<int> retained = handles;
    std::weak_ptr<int> exclusion = f.exclusion;
    auto started = actor_supervisor_t::start(f.store, intent(), std::move(f.exclusion), f.publish(),
      [handles = std::move(handles)](actor_body_context_t &context) mutable -> actor_body_result_t {
        context.retain(std::move(handles));
        throw std::runtime_error("after CreateProcess");
      });
    ASSERT_TRUE(started);
    EXPECT_EQ((*started)->wait_launch().status, actor_launch_e::uncertain);
    ASSERT_TRUE((*started)->wait_finished(5s));
    EXPECT_EQ((*started)->snapshot().phase, actor_phase_e::blocked);
    EXPECT_EQ(f.durable()->stage, actor_stage_e::intent);
    EXPECT_FALSE(retained.expired());
    EXPECT_FALSE(exclusion.expired());
  }

  TEST(ActorSupervisor, BodyExceptionAfterTheReceiptKeepsTheLaunchAndBlocks) {
    fixture_t f;
    auto started = actor_supervisor_t::start(f.store, intent(), f.exclusion, f.publish(),
      [&](actor_body_context_t &context) -> actor_body_result_t {
        context.report_launched(receipt);
        throw std::runtime_error("observe failed");
      });
    ASSERT_TRUE(started);
    ASSERT_TRUE((*started)->wait_finished(5s));
    EXPECT_EQ((*started)->wait_launch().status, actor_launch_e::launched);
    EXPECT_EQ((*started)->snapshot().phase, actor_phase_e::blocked);
    EXPECT_EQ(f.durable()->stage, actor_stage_e::launched);
  }

  TEST(ActorSupervisor, UnobservableActorBlocksWithoutSettling) {
    fixture_t f;
    auto started = actor_supervisor_t::start(f.store, intent(), f.exclusion, f.publish(), [&](actor_body_context_t &context) {
      context.report_launched(receipt);
      return actor_body_result_t { actor_body_exit_e::unobservable, "wait failed" };
    });
    ASSERT_TRUE(started);
    ASSERT_TRUE((*started)->wait_finished(5s));
    EXPECT_EQ((*started)->snapshot().phase, actor_phase_e::blocked);
    EXPECT_EQ(f.durable()->stage, actor_stage_e::launched);
  }

  TEST(ActorSupervisor, ExitWithoutAReceiptIsAContractViolation) {
    fixture_t f;
    auto started = actor_supervisor_t::start(f.store, intent(), f.exclusion, f.publish(), [&](actor_body_context_t &) {
      return actor_body_result_t { actor_body_exit_e::exited, "" };
    });
    ASSERT_TRUE(started);
    ASSERT_TRUE((*started)->wait_finished(5s));
    EXPECT_EQ((*started)->wait_launch().status, actor_launch_e::uncertain);
    EXPECT_EQ((*started)->snapshot().phase, actor_phase_e::blocked);
    EXPECT_EQ(f.durable()->stage, actor_stage_e::intent);
  }

  TEST(ActorSupervisor, NotLaunchedAfterAReceiptIsAContractViolation) {
    fixture_t f;
    auto started = actor_supervisor_t::start(f.store, intent(), f.exclusion, f.publish(), [&](actor_body_context_t &context) {
      context.report_launched(receipt);
      return actor_body_result_t { actor_body_exit_e::not_launched, "" };
    });
    ASSERT_TRUE(started);
    ASSERT_TRUE((*started)->wait_finished(5s));
    EXPECT_EQ((*started)->snapshot().phase, actor_phase_e::blocked);
    EXPECT_EQ(f.durable()->stage, actor_stage_e::launched);
  }

  TEST(ActorSupervisor, ASecondReceiptIsRejectedAndBlocks) {
    fixture_t f;
    bool second = true;
    auto started = actor_supervisor_t::start(f.store, intent(), f.exclusion, f.publish(), [&](actor_body_context_t &context) {
      context.report_launched(receipt);
      second = context.report_launched(actor_receipt_t { 6000, 1 });
      return actor_body_result_t { actor_body_exit_e::exited, "" };
    });
    ASSERT_TRUE(started);
    ASSERT_TRUE((*started)->wait_finished(5s));
    EXPECT_FALSE(second);
    EXPECT_EQ((*started)->snapshot().phase, actor_phase_e::blocked);
    EXPECT_EQ(f.durable()->receipt, receipt);
  }

  TEST(ActorSupervisor, ReceiptPersistFailureIsUncertainButObservedExitStillSettles) {
    fixture_t f;
    f.store->fail_replace_to = actor_stage_e::launched;
    bool reported = true;
    gate_t exit_gate;
    auto started = actor_supervisor_t::start(f.store, intent(), f.exclusion, f.publish(), [&](actor_body_context_t &context) {
      reported = context.report_launched(receipt);
      exit_gate.wait();
      return actor_body_result_t { actor_body_exit_e::exited, "" };
    });
    ASSERT_TRUE(started);
    EXPECT_EQ((*started)->wait_launch().status, actor_launch_e::uncertain);
    exit_gate.open();
    ASSERT_TRUE((*started)->wait_finished(5s));
    EXPECT_FALSE(reported);
    EXPECT_EQ((*started)->snapshot().phase, actor_phase_e::settled);
    EXPECT_EQ(f.durable()->stage, actor_stage_e::cleanup_pending);
    EXPECT_FALSE(f.durable()->receipt);
  }

  TEST(ActorSupervisor, SettleWriteFailureBlocks) {
    for (const bool throws : { false, true }) {
      fixture_t f;
      gate_t exit_gate;
      auto started = actor_supervisor_t::start(f.store, intent(), f.exclusion, f.publish(), [&](actor_body_context_t &context) {
        context.report_launched(receipt);
        exit_gate.wait();
        return actor_body_result_t { actor_body_exit_e::exited, "" };
      });
      ASSERT_TRUE(started);
      EXPECT_EQ((*started)->wait_launch().status, actor_launch_e::launched);
      if (throws) f.store->throw_replace = true;
      else f.store->fail_replace_to = actor_stage_e::cleanup_pending;
      exit_gate.open();
      ASSERT_TRUE((*started)->wait_finished(5s));
      EXPECT_EQ((*started)->snapshot().phase, actor_phase_e::blocked);
      EXPECT_EQ(f.durable()->stage, actor_stage_e::launched);
    }
  }

  TEST(ActorSupervisor, TheThreadKeepsTheSupervisorAndExclusionAliveAfterEveryCallerLetsGo) {
    fixture_t f;
    gate_t exit_gate;
    std::atomic<bool> finished {};
    std::weak_ptr<int> exclusion = f.exclusion;
    std::weak_ptr<actor_supervisor_t> weak;
    {
      auto started = actor_supervisor_t::start(f.store, intent(), std::move(f.exclusion), [](const auto &) -> result_t<void> {
        return {};
      },
        [&exit_gate, &finished](actor_body_context_t &context) {
          context.report_launched(receipt);
          exit_gate.wait();
          finished = true;
          return actor_body_result_t { actor_body_exit_e::exited, "" };
        });
      ASSERT_TRUE(started);
      weak = *started;
      const auto before = std::chrono::steady_clock::now();
      started->reset();
      EXPECT_LT(std::chrono::steady_clock::now() - before, 1s);
    }
    EXPECT_FALSE(weak.expired());
    EXPECT_FALSE(exclusion.expired());
    exit_gate.open();
    EXPECT_TRUE(eventually([&] { return weak.expired() && exclusion.expired(); }));
    EXPECT_TRUE(finished);
    EXPECT_EQ(f.durable()->stage, actor_stage_e::cleanup_pending);
  }

  TEST(ActorSupervisor, ConcurrentWaitersAllSeeTheSameSingleDecision) {
    fixture_t f;
    gate_t create_gate;
    auto started = actor_supervisor_t::start(f.store, intent(), f.exclusion, f.publish(), [&](actor_body_context_t &context) {
      create_gate.wait();
      context.report_launched(receipt);
      return actor_body_result_t { actor_body_exit_e::exited, "" };
    },
      within(500ms));
    ASSERT_TRUE(started);
    std::vector<actor_launch_result_t> results(4);
    std::vector<std::thread> waiters;
    for (auto &result : results) {
      waiters.emplace_back([&, supervisor = *started] { result = supervisor->wait_launch(); });
    }
    std::this_thread::sleep_for(30ms);
    create_gate.open();
    for (auto &waiter : waiters) waiter.join();
    for (const auto &result : results) EXPECT_EQ(result, results.front());
    EXPECT_EQ(results.front().status, actor_launch_e::launched);
    ASSERT_TRUE((*started)->wait_finished(5s));
  }

}  // namespace display_device::topology
