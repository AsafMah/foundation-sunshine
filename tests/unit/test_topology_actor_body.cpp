#include <gtest/gtest.h>

#include <algorithm>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include "src/display_device/topology_actor_body.h"

using namespace display_device::topology;

namespace {
  struct journal_t {
    std::vector<std::string> events;
  };

  struct fake_context_t final: actor_body_context_t {
    explicit fake_context_t(journal_t &journal):
        journal(journal) {}

    bool
    report_launched(actor_receipt_t receipt) override {
      journal.events.push_back("report");
      reports.push_back(receipt);
      return report_succeeds;
    }

    void
    retain(std::shared_ptr<void> resource) override {
      journal.events.push_back("retain");
      retained.push_back(std::move(resource));
    }

    journal_t &journal;
    bool report_succeeds { true };
    std::vector<actor_receipt_t> reports;
    std::vector<std::shared_ptr<void>> retained;
  };

  struct fake_process_t final: actor_process_t {
    explicit fake_process_t(journal_t &journal):
        journal(journal) {}

    void
    launch(std::error_code &ec) override {
      journal.events.push_back("launch");
      ++launches;
      if (creates) created = true;
      if (launch_error) ec = launch_error;
      if (launch_throws) throw std::runtime_error("launch threw");
    }

    bool
    started() const override {
      return created;
    }

    std::optional<actor_receipt_t>
    receipt() override {
      journal.events.push_back("receipt");
      return receipt_value;
    }

    actor_wait_t
    wait() override {
      journal.events.push_back("wait");
      if (waits.empty()) return { actor_wait_e::failed, {}, "script exhausted" };
      auto next = waits.front();
      waits.pop_front();
      return next;
    }

    journal_t &journal;
    bool creates { true }, created {}, launch_throws {};
    std::error_code launch_error;
    unsigned launches {};
    std::optional<actor_receipt_t> receipt_value { actor_receipt_t { 4242, 777 } };
    std::deque<actor_wait_t> waits { { actor_wait_e::exited, 0u, {} } };
  };

  class ActorBody: public ::testing::Test {
  protected:
    actor_body_result_t
    run() {
      auto body = make_process_actor_body([this]() -> std::shared_ptr<actor_process_t> {
        journal.events.push_back("make");
        return process;
      });
      return body(context);
    }

    journal_t journal;
    fake_context_t context { journal };
    std::shared_ptr<fake_process_t> process { std::make_shared<fake_process_t>(journal) };
  };
}

TEST_F(ActorBody, ANullProcessIsADefiniteNonLaunchWithoutAnyEffect) {
  auto body = make_process_actor_body([]() { return std::shared_ptr<actor_process_t> {}; });
  const auto result = body(context);
  EXPECT_EQ(result.exit, actor_body_exit_e::not_launched);
  EXPECT_TRUE(context.retained.empty());
  EXPECT_TRUE(context.reports.empty());
}

TEST_F(ActorBody, AMissingFactoryIsADefiniteNonLaunch) {
  auto body = make_process_actor_body({});
  EXPECT_EQ(body(context).exit, actor_body_exit_e::not_launched);
  EXPECT_TRUE(context.retained.empty());
}

TEST_F(ActorBody, TheProcessIsRetainedBeforeLaunchThenReportedBeforeItIsObserved) {
  process->waits = { { actor_wait_e::running, {}, {} }, { actor_wait_e::running, {}, {} }, { actor_wait_e::exited, 3u, {} } };
  const auto result = run();
  EXPECT_EQ(result.exit, actor_body_exit_e::exited);
  EXPECT_NE(result.detail.find('3'), std::string::npos) << result.detail;
  const std::vector<std::string> expected { "make", "retain", "launch", "receipt", "report", "wait", "wait", "wait" };
  EXPECT_EQ(journal.events, expected);
  ASSERT_EQ(context.retained.size(), 1u);
  EXPECT_EQ(context.retained.front().get(), process.get());
  ASSERT_EQ(context.reports.size(), 1u);
  EXPECT_EQ(context.reports.front(), (actor_receipt_t { 4242, 777 }));
  EXPECT_EQ(process->launches, 1u);
}

TEST_F(ActorBody, ACreationRefusalIsANonLaunchThatCarriesTheError) {
  process->creates = false;
  process->launch_error = std::make_error_code(std::errc::permission_denied);
  const auto result = run();
  EXPECT_EQ(result.exit, actor_body_exit_e::not_launched);
  EXPECT_NE(result.detail.find(process->launch_error.message()), std::string::npos) << result.detail;
  EXPECT_TRUE(context.reports.empty());
  const std::vector<std::string> expected { "make", "retain", "launch" };
  EXPECT_EQ(journal.events, expected);
}

TEST_F(ActorBody, AnExceptionBeforeCreationIsANonLaunchNotABlock) {
  process->creates = false;
  process->launch_throws = true;
  const auto result = run();
  EXPECT_EQ(result.exit, actor_body_exit_e::not_launched);
  EXPECT_NE(result.detail.find("launch threw"), std::string::npos) << result.detail;
  EXPECT_TRUE(context.reports.empty());
  ASSERT_EQ(context.retained.size(), 1u);
}

TEST_F(ActorBody, AFactoryExceptionIsANonLaunchWithoutAnyEffect) {
  auto body = make_process_actor_body([]() -> std::shared_ptr<actor_process_t> { throw std::runtime_error("no memory"); });
  const auto result = body(context);
  EXPECT_EQ(result.exit, actor_body_exit_e::not_launched);
  EXPECT_NE(result.detail.find("no memory"), std::string::npos) << result.detail;
  EXPECT_TRUE(context.retained.empty());
  EXPECT_TRUE(context.reports.empty());
}

TEST_F(ActorBody, AnErrorOrExceptionAfterCreationStillReportsAndObservesTheProcess) {
  for (const bool throws : { false, true }) {
    SCOPED_TRACE(throws);
    journal.events.clear();
    context.reports.clear();
    context.retained.clear();
    process = std::make_shared<fake_process_t>(journal);
    process->launch_throws = throws;
    process->launch_error = throws ? std::error_code {} : std::make_error_code(std::errc::io_error);
    process->waits = { { actor_wait_e::exited, 0u, {} } };
    const auto result = run();
    EXPECT_EQ(result.exit, actor_body_exit_e::exited);
    const auto cause = throws ? std::string { "launch threw" } : std::make_error_code(std::errc::io_error).message();
    EXPECT_NE(result.detail.find("after creation: " + cause), std::string::npos) << result.detail;
    ASSERT_EQ(context.reports.size(), 1u);
    ASSERT_EQ(context.retained.size(), 1u);
  }
}

TEST_F(ActorBody, AStartedProcessWithoutAReceiptIsUnobservableAndNeverReported) {
  process->receipt_value.reset();
  const auto result = run();
  EXPECT_EQ(result.exit, actor_body_exit_e::unobservable);
  EXPECT_TRUE(context.reports.empty());
  ASSERT_EQ(context.retained.size(), 1u);
  EXPECT_EQ(std::count(journal.events.begin(), journal.events.end(), "wait"), 0);
}

TEST_F(ActorBody, AReportThatIsNotDurableStillWaitsForTheActorToExit) {
  context.report_succeeds = false;
  process->waits = { { actor_wait_e::running, {}, {} }, { actor_wait_e::exited, 1u, {} } };
  const auto result = run();
  EXPECT_EQ(result.exit, actor_body_exit_e::exited);
  EXPECT_EQ(std::count(journal.events.begin(), journal.events.end(), "wait"), 2);
}

TEST_F(ActorBody, AnObservationFailureIsUnobservableNotAnExit) {
  process->waits = { { actor_wait_e::running, {}, {} }, { actor_wait_e::failed, {}, "wait failed" } };
  const auto result = run();
  EXPECT_EQ(result.exit, actor_body_exit_e::unobservable);
  EXPECT_NE(result.detail.find("wait failed"), std::string::npos) << result.detail;
}

TEST_F(ActorBody, AnExitWithoutACodeIsStillAnExit) {
  process->waits = { { actor_wait_e::exited, {}, "code unavailable" } };
  const auto result = run();
  EXPECT_EQ(result.exit, actor_body_exit_e::exited);
}

TEST_F(ActorBody, TheFactoryRunsOncePerBodyInvocation) {
  unsigned made {};
  auto body = make_process_actor_body([&]() -> std::shared_ptr<actor_process_t> {
    ++made;
    return process;
  });
  EXPECT_EQ(body(context).exit, actor_body_exit_e::exited);
  EXPECT_EQ(made, 1u);
}

namespace {
  class memory_store_t final: public actor_record_store_t {
  public:
    actor_load_t
    load() override {
      std::lock_guard lock { mutex_ };
      return record_ ? actor_load_t { actor_load_status_e::loaded, record_, {} } : actor_load_t {};
    }

    result_t<void>
    create(const actor_record_t &record) override {
      std::lock_guard lock { mutex_ };
      if (record_) return std::unexpected(error_t { error_code_e::resource_unavailable, "exists", {} });
      record_ = record;
      return {};
    }

    result_t<void>
    replace(const actor_record_t &expected, const actor_record_t &next) override {
      std::lock_guard lock { mutex_ };
      if (!record_ || *record_ != expected) return std::unexpected(error_t { error_code_e::resource_unavailable, "stale", {} });
      record_ = next;
      return {};
    }

    result_t<void>
    retire(const actor_record_t &) override {
      return std::unexpected(error_t { error_code_e::resource_unavailable, "not used", {} });
    }

  private:
    std::mutex mutex_;
    std::optional<actor_record_t> record_;
  };

  struct supervised_t {
    std::shared_ptr<memory_store_t> store { std::make_shared<memory_store_t>() };
    std::shared_ptr<actor_supervisor_t> supervisor;

    void
    start(actor_body_t body) {
      auto intent = begin_actor_intent(9, std::string { "boot" }, actor_obligation_e::restore_legacy_display, "detail");
      ASSERT_TRUE(intent);
      auto started = actor_supervisor_t::start(store, *intent, std::make_shared<int>(1),
        [this](const std::shared_ptr<actor_supervisor_t> &published) -> result_t<void> {
          supervisor = published;
          return {};
        },
        std::move(body));
      ASSERT_TRUE(started) << started.error().detail;
    }
  };
}

TEST_F(ActorBody, UnderTheSupervisorAnExitedActorIsReportedThenSettledWithItsHandlesHeld) {
  process->waits = { { actor_wait_e::running, {}, {} }, { actor_wait_e::exited, 0u, {} } };
  std::weak_ptr<fake_process_t> watched = process;
  supervised_t supervised;
  auto made = process;
  process.reset();
  supervised.start(make_process_actor_body([made]() -> std::shared_ptr<actor_process_t> { return made; }));
  made.reset();
  ASSERT_TRUE(supervised.supervisor);
  const auto launch = supervised.supervisor->wait_launch();
  EXPECT_EQ(launch.status, actor_launch_e::launched);
  EXPECT_EQ(launch.receipt, (actor_receipt_t { 4242, 777 }));
  ASSERT_TRUE(supervised.supervisor->wait_finished(std::chrono::seconds { 5 }));
  const auto snapshot = supervised.supervisor->snapshot();
  EXPECT_EQ(snapshot.phase, actor_phase_e::settled);
  EXPECT_EQ(snapshot.record.stage, actor_stage_e::cleanup_pending);
  EXPECT_EQ(snapshot.record.receipt, (actor_receipt_t { 4242, 777 }));
  EXPECT_FALSE(watched.expired());
  supervised.supervisor.reset();
  EXPECT_TRUE(watched.expired());
}

TEST_F(ActorBody, UnderTheSupervisorARefusedCreationIsADefiniteNonLaunch) {
  process->creates = false;
  process->launch_error = std::make_error_code(std::errc::permission_denied);
  supervised_t supervised;
  auto made = process;
  supervised.start(make_process_actor_body([made]() -> std::shared_ptr<actor_process_t> { return made; }));
  ASSERT_TRUE(supervised.supervisor);
  EXPECT_EQ(supervised.supervisor->wait_launch().status, actor_launch_e::not_launched);
  ASSERT_TRUE(supervised.supervisor->wait_finished(std::chrono::seconds { 5 }));
  EXPECT_EQ(supervised.supervisor->snapshot().phase, actor_phase_e::settled);
  EXPECT_FALSE(supervised.supervisor->snapshot().record.receipt);
}

TEST_F(ActorBody, UnderTheSupervisorAnUnreceiptedProcessBlocksAndKeepsItsHandles) {
  process->receipt_value.reset();
  std::weak_ptr<fake_process_t> watched = process;
  supervised_t supervised;
  auto made = process;
  process.reset();
  supervised.start(make_process_actor_body([made]() -> std::shared_ptr<actor_process_t> { return made; }));
  made.reset();
  ASSERT_TRUE(supervised.supervisor);
  EXPECT_NE(supervised.supervisor->wait_launch().status, actor_launch_e::launched);
  ASSERT_TRUE(supervised.supervisor->wait_finished(std::chrono::seconds { 5 }));
  EXPECT_EQ(supervised.supervisor->snapshot().phase, actor_phase_e::blocked);
  EXPECT_EQ(supervised.supervisor->snapshot().record.stage, actor_stage_e::intent);
  EXPECT_FALSE(watched.expired());
}
