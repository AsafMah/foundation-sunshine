#include <gtest/gtest.h>

#include <atomic>
#include <future>
#include <stdexcept>
#include <thread>

#include "src/display_device/topology_routes.h"

namespace {
  using namespace std::chrono_literals;
  using namespace display_device::topology;

  class probe_gate_t {
  public:
    probe_gate_t():
        entered_ { entered_promise_.get_future().share() },
        released_ { released_promise_.get_future().share() } {}

    void
    enter() {
      entered_promise_.set_value();
      released_.wait();
    }

    bool
    started() {
      return entered_.wait_for(2s) == std::future_status::ready;
    }

    void
    release() {
      std::call_once(once_, [&]() { released_promise_.set_value(); });
    }

  private:
    std::promise<void> entered_promise_, released_promise_;
    std::shared_future<void> entered_, released_;
    std::once_flag once_;
  };

  struct release_probe_t {
    probe_gate_t &gate;
    ~release_probe_t() {
      gate.release();
    }
  };

  struct probe_thread_exit_t {
    std::function<void()> cleanup;
    ~probe_thread_exit_t() {
      if (cleanup) cleanup();
    }
  };

  std::shared_ptr<const probe_fence_t>
  make_probe_fence(coordinator_t &coordinator) {
    std::shared_ptr<const probe_fence_t> fence;
    const auto result = coordinator.run(command_kind_e::rotate, [&](const auto &, const auto &) {
      fence = coordinator.hold_probe_fence();
    });
    if (!result.completed()) throw std::runtime_error { "could not acquire fake probe fence" };
    return fence;
  }

  TEST(DisplayTopologyRoutes, AbandonedMutationCannotPublishALateProbeFence) {
    coordinator_t coordinator;
    ASSERT_TRUE(coordinator.start({}).get().completed());
    probe_handoff_t handoff;
    probe_gate_t mutation;
    std::weak_ptr<const probe_fence_t> observed;
    const auto pending = coordinator.post_tracked(command_kind_e::rotate, [&](const auto &, const auto &) {
      mutation.enter();
      auto fence = coordinator.hold_probe_fence();
      observed = fence;
      EXPECT_FALSE(handoff.publish(std::move(fence)));
    });
    const release_probe_t release { mutation };
    ASSERT_TRUE(mutation.started());
    EXPECT_FALSE(handoff.abandon());
    const auto cleanup = coordinator.post_tracked(command_kind_e::stream_restore, {});
    mutation.release();
    ASSERT_TRUE(pending.completion.get().completed());
    EXPECT_EQ(cleanup.completion.wait_for(2s), std::future_status::ready)
      << "late publication stranded cleanup behind an unclaimed probe fence";
    EXPECT_TRUE(observed.expired());
    auto stranded = handoff.abandon();
    stranded.reset();
    EXPECT_TRUE(cleanup.completion.get().completed());
  }

  TEST(DisplayTopologyRoutes, ProbeHandoffTransfersOneOwnerAndPreservesRollbackExclusion) {
    coordinator_t coordinator;
    ASSERT_TRUE(coordinator.start({}).get().completed());
    probe_handoff_t handoff;
    EXPECT_THROW(handoff.take(), std::logic_error);
    EXPECT_THROW(handoff.publish({}), std::invalid_argument);
    auto fence = make_probe_fence(coordinator);
    EXPECT_TRUE(handoff.publish(fence));
    EXPECT_THROW(handoff.publish(fence), std::logic_error);
    fence.reset();
    auto claimed = handoff.take();
    ASSERT_TRUE(claimed);
    EXPECT_THROW(handoff.take(), std::logic_error);
    EXPECT_FALSE(handoff.abandon());
    const auto cleanup = coordinator.post_tracked(command_kind_e::stream_restore, {});
    EXPECT_EQ(cleanup.completion.wait_for(50ms), std::future_status::timeout);
    claimed.reset();
    ASSERT_EQ(cleanup.completion.wait_for(2s), std::future_status::ready)
      << "the handoff retained the claimed fence";
    EXPECT_TRUE(cleanup.completion.get().completed());

    probe_handoff_t abandoned;
    EXPECT_TRUE(abandoned.publish(make_probe_fence(coordinator)));
    auto rollback_guard = abandoned.abandon();
    ASSERT_TRUE(rollback_guard);
    EXPECT_FALSE(abandoned.abandon());
    EXPECT_THROW(abandoned.take(), std::logic_error);
    const auto rollback = coordinator.post_tracked(command_kind_e::stream_restore, {});
    EXPECT_EQ(rollback.completion.wait_for(50ms), std::future_status::timeout);
    rollback_guard.reset();
    ASSERT_EQ(rollback.completion.wait_for(2s), std::future_status::ready)
      << "the handoff retained the abandoned fence";
    EXPECT_TRUE(rollback.completion.get().completed());
  }

  TEST(DisplayTopologyRoutes, ProbeRunsOffWorkerAndKeepsExclusionThroughCallerRollback) {
    coordinator_t coordinator;
    ASSERT_TRUE(coordinator.start({}).get().completed());
    auto fence = make_probe_fence(coordinator);
    probe_gate_t probe;
    std::atomic_int fatal {};
    auto run = std::async(std::launch::async, [&]() {
      const auto supervisor = std::this_thread::get_id();
      run_supervised_probe(std::chrono::steady_clock::now() + 2s, fence, [&]() {
        EXPECT_NE(std::this_thread::get_id(), supervisor);
        EXPECT_FALSE(coordinator.on_worker_thread());
        probe.enter(); }, [&]() { ++fatal; });
    });
    const release_probe_t release { probe };
    ASSERT_TRUE(probe.started());
    const auto cleanup = coordinator.post_tracked(command_kind_e::stream_restore, {});
    EXPECT_EQ(cleanup.completion.wait_for(50ms), std::future_status::timeout);
    probe.release();
    EXPECT_NO_THROW(run.get());
    EXPECT_EQ(cleanup.completion.wait_for(50ms), std::future_status::timeout)
      << "only the caller may drop its final guard after deciding rollback";
    fence.reset();
    EXPECT_TRUE(cleanup.completion.get().completed());
    EXPECT_EQ(fatal.load(), 0);
  }

  TEST(DisplayTopologyRoutes, ProbeDeadlineCoversDriverBodyAndThreadLocalTeardown) {
    for (const int mode : { 0, 1, 2 }) {
      SCOPED_TRACE(::testing::Message() << "teardown mode=" << mode);
      coordinator_t coordinator;
      ASSERT_TRUE(coordinator.start({}).get().completed());
      auto fence = make_probe_fence(coordinator);
      probe_gate_t probe;
      std::promise<void> fatal_called;
      auto fatal_ready = fatal_called.get_future();
      std::atomic_int fatals {};
      auto run = std::async(std::launch::async, [&]() {
        try {
          run_supervised_probe(std::chrono::steady_clock::now() + 200ms, fence, [&]() {
            if (mode != 0) {
              thread_local probe_thread_exit_t at_exit;
              at_exit.cleanup = [&]() { probe.enter(); };
              if (mode == 2) throw std::runtime_error { "probe failed before TLS cleanup" };
            }
            else {
              probe.enter();
            } }, [&]() {
            EXPECT_TRUE(coordinator.status().fence_active);
            ++fatals;
            fatal_called.set_value(); });
          return false;
        }
        catch (const probe_timeout_error_t &error) {
          return error.started();
        }
        catch (const std::runtime_error &error) {
          ADD_FAILURE() << "probe error bypassed its teardown deadline: " << error.what();
          return false;
        }
      });
      const release_probe_t release { probe };
      ASSERT_TRUE(probe.started());
      const auto cleanup = coordinator.post_tracked(command_kind_e::stream_restore, {});
      EXPECT_EQ(fatal_ready.wait_for(2s), std::future_status::ready)
        << "the watchdog must cover cleanup after the probe body returns";
      EXPECT_EQ(run.wait_for(0ms), std::future_status::timeout)
        << "a returning fake fatal hook must not detach a live driver thread";
      EXPECT_EQ(cleanup.completion.wait_for(0ms), std::future_status::timeout);
      probe.release();
      EXPECT_TRUE(run.get());
      EXPECT_EQ(fatals.load(), 1);
      fence.reset();
      EXPECT_TRUE(cleanup.completion.get().completed());
    }
  }

  TEST(DisplayTopologyRoutes, ProbeDeadlineIncludesThreadFactoryAndThrowingFatalHook) {
    coordinator_t coordinator;
    ASSERT_TRUE(coordinator.start({}).get().completed());
    auto fence = make_probe_fence(coordinator);
    probe_gate_t factory;
    std::promise<void> probed, fatal_called;
    auto probe_ready = probed.get_future();
    auto fatal_ready = fatal_called.get_future();
    auto run = std::async(std::launch::async, [&]() {
      try {
        run_supervised_probe(std::chrono::steady_clock::now() + 200ms, fence, [&]() { probed.set_value(); }, [&]() {
            fatal_called.set_value();
            throw std::runtime_error { "fake fatal hook" }; }, [&](std::function<void()> body) {
            std::jthread worker { std::move(body) };
            factory.enter();
            return worker; });
        return std::string { "incorrect success" };
      }
      catch (const std::runtime_error &error) {
        return std::string { error.what() };
      }
    });
    const release_probe_t release { factory };
    ASSERT_TRUE(factory.started());
    ASSERT_EQ(probe_ready.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(fatal_ready.wait_for(2s), std::future_status::ready)
      << "supervision must start before invoking the thread factory";
    EXPECT_EQ(run.wait_for(0ms), std::future_status::timeout);
    EXPECT_TRUE(coordinator.status().fence_active);
    factory.release();
    EXPECT_EQ(run.get(), "fake fatal hook");
  }

  TEST(DisplayTopologyRoutes, MutationWorkerCannotSuperviseAProbe) {
    coordinator_t coordinator;
    ASSERT_TRUE(coordinator.start({}).get().completed());
    int probes {}, fatals {};
    EXPECT_TRUE(coordinator.run(command_kind_e::rotate, [&](const auto &, const auto &) {
                             auto fence = coordinator.hold_probe_fence();
                             EXPECT_THROW(run_supervised_probe(std::chrono::steady_clock::now() + 2s, fence, [&]() { ++probes; }, [&]() { ++fatals; }), std::logic_error);
                           })
        .completed());
    EXPECT_EQ(probes, 0);
    EXPECT_EQ(fatals, 0);
  }

  TEST(DisplayTopologyRoutes, ExpiredOrNotYetStartedProbeNeverInvokesDriverOrFatalHook) {
    coordinator_t coordinator;
    ASSERT_TRUE(coordinator.start({}).get().completed());
    auto fence = make_probe_fence(coordinator);
    std::atomic_int calls {}, fatals {}, spawns {};
    const probe_thread_factory_t spawn = [&](std::function<void()> body) {
      ++spawns;
      return std::jthread { std::move(body) };
    };
    try {
      run_supervised_probe(std::chrono::steady_clock::now() - 1ms, fence, [&]() { ++calls; }, [&]() { ++fatals; }, spawn);
      FAIL() << "an expired probe was accepted";
    }
    catch (const probe_timeout_error_t &error) {
      EXPECT_FALSE(error.started());
    }
    EXPECT_EQ(spawns.load(), 0);
    probe_gate_t launch;
    const auto deadline = std::chrono::steady_clock::now() + 100ms;
    auto run = std::async(std::launch::async, [&]() {
      try {
        run_supervised_probe(deadline, fence, [&]() { ++calls; }, [&]() { ++fatals; }, [&](std::function<void()> body) { return std::jthread { [&, body = std::move(body)]() {
                                                                                                                           launch.enter();
                                                                                                                           body();
                                                                                                                         } }; });
        return true;
      }
      catch (const probe_timeout_error_t &error) {
        EXPECT_FALSE(error.started());
        return false;
      }
    });
    const release_probe_t release { launch };
    ASSERT_TRUE(launch.started());
    std::this_thread::sleep_until(deadline + 20ms);
    launch.release();
    EXPECT_FALSE(run.get());
    EXPECT_EQ(calls.load(), 0);
    EXPECT_EQ(fatals.load(), 0);
  }

  TEST(DisplayTopologyRoutes, ProbeErrorsAndSpawnFailuresPropagateWithoutLosingExclusion) {
    coordinator_t coordinator;
    ASSERT_TRUE(coordinator.start({}).get().completed());
    auto fence = make_probe_fence(coordinator);
    std::atomic_int fatals {}, probes {};
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    EXPECT_THROW(run_supervised_probe(deadline, fence, []() { throw std::runtime_error { "driver failed" }; }, [&]() { ++fatals; }), std::runtime_error);
    EXPECT_THROW(run_supervised_probe(deadline, fence, [&]() { ++probes; }, [&]() { ++fatals; }, [](std::function<void()>) -> std::jthread { throw std::runtime_error { "thread creation failed" }; }), std::runtime_error);
    EXPECT_THROW(run_supervised_probe(deadline, fence, [&]() { ++probes; }, [&]() { ++fatals; }, [](std::function<void()>) { return std::jthread {}; }), std::invalid_argument);
    EXPECT_EQ(probes.load(), 0);
    EXPECT_EQ(fatals.load(), 0);
    const auto cleanup = coordinator.post_tracked(command_kind_e::stream_restore, {});
    EXPECT_EQ(cleanup.completion.wait_for(50ms), std::future_status::timeout);
    fence.reset();
    EXPECT_TRUE(cleanup.completion.get().completed());
  }

  TEST(DisplayTopologyRoutes, ProbeSupervisionRequiresFenceBodyAndFatalHandler) {
    coordinator_t coordinator;
    ASSERT_TRUE(coordinator.start({}).get().completed());
    auto fence = make_probe_fence(coordinator);
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    EXPECT_THROW(run_supervised_probe(deadline, {}, []() {}, []() {}), std::invalid_argument);
    EXPECT_THROW(run_supervised_probe(deadline, fence, {}, []() {}), std::invalid_argument);
    EXPECT_THROW(run_supervised_probe(deadline, fence, []() {}, {}), std::invalid_argument);
  }

  TEST(DisplayTopologyRoutes, SetupDeadlineLookupUsesRecordedOwnerDeadlineAndReapsExpiry) {
    std::atomic_int time {};
    std::atomic_int cleanup {};
    coordinator_options_t options;
    options.clock = [&]() { return monotonic_time_t { time.load() }; };
    options.setup_timeout = 100ms;
    coordinator_t coordinator { options };
    ASSERT_TRUE(coordinator.start({}).get().completed());
    const auto launch = coordinator.admit_launch({ 450, "probe" });
    ASSERT_TRUE(launch);
    ASSERT_TRUE(coordinator.run_launch_step(*launch.token, {}, [&](const auto &, const auto &) { ++cleanup; }).completed());
    auto forged = *launch.token;
    forged.setup_deadline = 1h;
    EXPECT_EQ(coordinator.launch_setup_deadline(forged), launch.token->setup_deadline);
    ++forged.launch_session_id;
    EXPECT_FALSE(coordinator.launch_setup_deadline(forged));
    time = 100;
    EXPECT_FALSE(coordinator.launch_setup_deadline(*launch.token));
    EXPECT_FALSE(coordinator.status().lease);
    EXPECT_TRUE(coordinator.post_tracked(command_kind_e::stream_restore, {}).completion.get().completed());
    EXPECT_EQ(cleanup.load(), 1);
  }

  TEST(DisplayTopologyRoutes, SetupDeadlineLookupRejectsBoundDrainingAndStoppedLaunches) {
    for (const int state : { 0, 1, 2 }) {
      coordinator_t coordinator;
      ASSERT_TRUE(coordinator.start({}).get().completed());
      const auto launch = coordinator.admit_launch({ 451, "not preparing" });
      ASSERT_TRUE(launch);
      ASSERT_TRUE(coordinator.run_launch_prepare(*launch.token, {}).completed());
      if (state == 0) ASSERT_TRUE(coordinator.bind_launch(*launch.token));
      if (state == 1) coordinator.begin_draining();
      if (state == 2) coordinator.stop();
      EXPECT_FALSE(coordinator.launch_setup_deadline(*launch.token));
    }
  }

  TEST(DisplayTopologyRoutes, RetryScheduleWaitsForItsDeadlineAndCompletion) {
    retry_schedule_t schedule { 10ms };
    EXPECT_FALSE(schedule.take_due(100ms));
    schedule.setup(true, 100ms);
    EXPECT_EQ(schedule.next_wake(), 110ms);
    EXPECT_FALSE(schedule.take_due(109ms));
    const auto token = schedule.take_due(110ms);
    ASSERT_TRUE(token);
    EXPECT_TRUE(schedule.is_current(*token));
    EXPECT_FALSE(schedule.take_due(1000ms)) << "a retry is already in flight";
    EXPECT_EQ(schedule.complete(*token, {}, false, 120ms), retry_completion_e::rearmed);
    EXPECT_EQ(schedule.next_wake(), 130ms);
    const auto retry = schedule.take_due(130ms);
    ASSERT_TRUE(retry);
    EXPECT_EQ(schedule.complete(*retry, {}, true, 140ms), retry_completion_e::disarmed);
    EXPECT_FALSE(schedule.next_wake());
    EXPECT_FALSE(schedule.is_current(*retry));
  }

  TEST(DisplayTopologyRoutes, ReplacingOrDisablingARetryInvalidatesItsOldCompletion) {
    retry_schedule_t schedule { 10ms };
    schedule.setup(true, 0ms);
    const auto old = schedule.take_due(10ms);
    ASSERT_TRUE(old);
    schedule.setup(true, 20ms);
    EXPECT_FALSE(schedule.is_current(*old));
    EXPECT_EQ(schedule.complete(*old, {}, true, 25ms), retry_completion_e::stale);
    EXPECT_EQ(schedule.next_wake(), 30ms);
    const auto current = schedule.take_due(30ms);
    ASSERT_TRUE(current);
    schedule.setup(false, 40ms);
    EXPECT_EQ(schedule.complete(*current, {}, false, 50ms), retry_completion_e::stale);
    EXPECT_FALSE(schedule.next_wake()) << "a stale completion must not restart a disabled timer";
  }

  TEST(DisplayTopologyRoutes, RepeatedAttemptsCannotConsumeEachOthersCompletion) {
    retry_schedule_t schedule { 10ms };
    schedule.setup(true, 0ms);
    const auto first = schedule.take_due(10ms);
    ASSERT_TRUE(first);
    ASSERT_EQ(schedule.complete(*first, {}, false, 10ms), retry_completion_e::rearmed);
    const auto second = schedule.take_due(20ms);
    ASSERT_TRUE(second);
    EXPECT_NE(*first, *second);
    EXPECT_EQ(schedule.complete(*first, {}, true, 21ms), retry_completion_e::stale);
    EXPECT_TRUE(schedule.is_current(*second));
    EXPECT_EQ(schedule.complete(*second, {}, true, 22ms), retry_completion_e::disarmed);
  }

  TEST(DisplayTopologyRoutes, RetryFailuresRearmButShutdownOutcomesDisarm) {
    for (const auto outcome : { outcome_e::completed_late, outcome_e::cancelled, outcome_e::failed, outcome_e::expired_before_start }) {
      retry_schedule_t schedule { 10ms };
      schedule.setup(true, 0ms);
      const auto token = schedule.take_due(10ms);
      ASSERT_TRUE(token);
      EXPECT_EQ(schedule.complete(*token, { outcome }, true, 20ms), retry_completion_e::rearmed);
      EXPECT_EQ(schedule.next_wake(), 30ms);
    }
    for (const auto reason : { reject_reason_e::owner_busy, reject_reason_e::managed_session, reject_reason_e::fence_active,
           reject_reason_e::draining, reject_reason_e::stopped }) {
      retry_schedule_t schedule { 10ms };
      schedule.setup(true, 0ms);
      const auto token = schedule.take_due(10ms);
      ASSERT_TRUE(token);
      const bool stopped = reason == reject_reason_e::draining || reason == reject_reason_e::stopped;
      EXPECT_EQ(schedule.complete(*token, { outcome_e::rejected, reason }, false, 20ms),
        stopped ? retry_completion_e::disarmed : retry_completion_e::rearmed);
    }
    retry_schedule_t schedule { 10ms };
    schedule.setup(true, 0ms);
    const auto token = schedule.take_due(10ms);
    ASSERT_TRUE(token);
    EXPECT_EQ(schedule.complete(*token, { outcome_e::stopped }, false, 20ms), retry_completion_e::disarmed);
    EXPECT_FALSE(schedule.next_wake());
  }

  TEST(DisplayTopologyRoutes, WorkerRejectedTickReceiptRearmsItsOwnGeneration) {
    coordinator_t coordinator;
    ASSERT_TRUE(coordinator.start({}).get().completed());
    std::promise<void> entered, released;
    auto entered_future = entered.get_future();
    auto released_future = released.get_future().share();
    const auto blocker = coordinator.post_tracked(command_kind_e::rotate, [&](const auto &, const auto &) {
      entered.set_value();
      released_future.wait();
    });
    const bool started = entered_future.wait_for(2s) == std::future_status::ready;
    if (!started) {
      released.set_value();
      FAIL() << "worker did not start";
    }
    retry_schedule_t schedule { 10ms };
    schedule.setup(true, 0ms);
    const auto token = schedule.take_due(10ms);
    // take_due cannot fail here; release the worker before any fatal assertion.
    std::atomic_int calls {};
    const auto receipt = coordinator.post_tracked(command_kind_e::legacy_retry_tick, [&](const auto &, const auto &) { ++calls; });
    auto fence = coordinator.hold_fence();
    released.set_value();
    ASSERT_TRUE(token);
    ASSERT_EQ(receipt.reason, reject_reason_e::none);
    ASSERT_EQ(receipt.completion.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(receipt.completion.get().reason, reject_reason_e::fence_active);
    EXPECT_EQ(calls, 0);
    EXPECT_EQ(schedule.complete(*token, receipt.completion.get(), false, 20ms), retry_completion_e::rearmed);
    const auto retry = schedule.take_due(30ms);
    ASSERT_TRUE(retry);
    EXPECT_NE(*retry, *token);
    fence.reset();
    const auto retried = coordinator.post_tracked(command_kind_e::legacy_retry_tick, [&](const auto &, const auto &) { ++calls; });
    ASSERT_EQ(retried.completion.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(schedule.complete(*retry, retried.completion.get(), true, 40ms), retry_completion_e::disarmed);
    EXPECT_EQ(calls, 1);
  }

  TEST(DisplayTopologyRoutes, RetryDelayMustBePositive) {
    EXPECT_THROW(retry_schedule_t { 0ms }, std::invalid_argument);
    EXPECT_THROW(retry_schedule_t { -1ms }, std::invalid_argument);
  }
}  // namespace
