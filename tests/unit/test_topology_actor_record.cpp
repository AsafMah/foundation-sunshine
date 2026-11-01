/**
 * @file tests/unit/test_topology_actor_record.cpp
 * @brief Durable external-actor record transitions, storage and restart classification.
 */
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <random>
#include <thread>
#include <vector>

#include "src/display_device/topology_actor_record.h"
#include "src/display_device/topology_durable_file.h"

namespace display_device::topology {
  namespace {
    constexpr std::uint64_t op = 41;
    constexpr actor_receipt_t receipt { 4242, 133'000'000'000'000'000ULL };

    actor_record_t
    intent(std::optional<std::string> boot = std::string { "boot-a" }) {
      auto record = begin_actor_intent(op, std::move(boot), actor_obligation_e::restore_legacy_display, "adapter restart");
      EXPECT_TRUE(record) << (record ? "" : record.error().detail);
      return *record;
    }

    actor_record_t
    launched(std::optional<std::string> boot = std::string { "boot-a" }) {
      return *record_actor_receipt(intent(std::move(boot)), op, receipt);
    }

    actor_record_t
    pending() {
      return *settle_actor(launched(), op);
    }

    class temp_dir_t {
    public:
      temp_dir_t() {
        std::random_device random;
        path_ = std::filesystem::temp_directory_path() /
                ("sunshine-actor-record-" + std::to_string(random()) + "-" + std::to_string(random()));
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

    std::string
    read_all(const std::filesystem::path &path) {
      std::ifstream file(path, std::ios::binary);
      return { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
    }

    void
    write_all(const std::filesystem::path &path, const std::string &bytes) {
      std::ofstream file(path, std::ios::binary | std::ios::trunc);
      file << bytes;
    }

    actor_load_t
    loaded(const actor_record_t &record) {
      return { actor_load_status_e::loaded, record, {} };
    }

    process_observer_t
    observing(process_observation_e observation, int *calls = nullptr) {
      return [observation, calls](const actor_receipt_t &seen) {
        EXPECT_EQ(seen, receipt);
        if (calls) ++*calls;
        return observation;
      };
    }

    process_observer_t
    never_observed() {
      return [](const actor_receipt_t &) -> process_observation_e {
        ADD_FAILURE() << "process observed";
        return process_observation_e::running;
      };
    }
  }  // namespace

  TEST(TopologyActorRecord, LifecycleKeepsImmutableIdentityAndObligation) {
    const auto first = intent();
    EXPECT_EQ(first.stage, actor_stage_e::intent);
    EXPECT_EQ(first.operation, op);
    EXPECT_FALSE(first.receipt);
    EXPECT_FALSE(actor_record_terminal(first));

    const auto second = record_actor_receipt(first, op, receipt);
    ASSERT_TRUE(second);
    EXPECT_EQ(second->stage, actor_stage_e::launched);
    EXPECT_EQ(second->receipt, receipt);

    const auto third = settle_actor(*second, op);
    ASSERT_TRUE(third);
    EXPECT_EQ(third->stage, actor_stage_e::cleanup_pending);
    EXPECT_EQ(third->receipt, receipt);
    EXPECT_FALSE(actor_record_terminal(*third));

    const auto done = complete_actor_cleanup(*third, op);
    ASSERT_TRUE(done);
    EXPECT_EQ(done->stage, actor_stage_e::completed);
    EXPECT_TRUE(actor_record_terminal(*done));

    const auto moved = transfer_actor_cleanup(*third, op, "session pending restore");
    ASSERT_TRUE(moved);
    EXPECT_EQ(moved->stage, actor_stage_e::transferred);
    EXPECT_EQ(moved->transferred_to, "session pending restore");
    EXPECT_TRUE(actor_record_terminal(*moved));

    for (const auto *record : { &*second, &*third, &*done, &*moved }) {
      EXPECT_EQ(record->operation, first.operation);
      EXPECT_EQ(record->boot, first.boot);
      EXPECT_EQ(record->obligation, first.obligation);
      EXPECT_EQ(record->obligation_detail, first.obligation_detail);
      EXPECT_EQ(record->version, first.version);
    }
  }

  TEST(TopologyActorRecord, IntentWithoutReceiptCanSettleDirectly) {
    const auto settled = settle_actor(intent(), op);
    ASSERT_TRUE(settled);
    EXPECT_EQ(settled->stage, actor_stage_e::cleanup_pending);
    EXPECT_FALSE(settled->receipt);
  }

  TEST(TopologyActorRecord, RejectsInvalidIntentAndReceipt) {
    EXPECT_FALSE(begin_actor_intent(0, std::string { "boot" }, actor_obligation_e::restore_legacy_display, "x"));
    EXPECT_FALSE(begin_actor_intent(op, std::string {}, actor_obligation_e::restore_legacy_display, "x"));
    EXPECT_TRUE(begin_actor_intent(op, std::nullopt, actor_obligation_e::restore_legacy_display, "x"));
    EXPECT_FALSE(record_actor_receipt(intent(), op, { 0, receipt.creation_time }));
    EXPECT_FALSE(record_actor_receipt(intent(), op, { receipt.pid, 0 }));
  }

  TEST(TopologyActorRecord, RejectsOutOfOrderTransitions) {
    const auto expect_transition_error = [](const result_t<actor_record_t> &result, const char *what) {
      SCOPED_TRACE(what);
      ASSERT_FALSE(result);
      EXPECT_EQ(result.error().code, error_code_e::invalid_transition);
    };
    expect_transition_error(record_actor_receipt(launched(), op, receipt), "second receipt");
    expect_transition_error(record_actor_receipt(pending(), op, receipt), "receipt after settle");
    expect_transition_error(settle_actor(pending(), op), "settle twice");
    expect_transition_error(complete_actor_cleanup(intent(), op), "complete before settle");
    expect_transition_error(complete_actor_cleanup(launched(), op), "complete while actor may run");
    expect_transition_error(transfer_actor_cleanup(launched(), op, "x"), "transfer while actor may run");
    const auto done = *complete_actor_cleanup(pending(), op);
    expect_transition_error(complete_actor_cleanup(done, op), "complete twice");
    expect_transition_error(transfer_actor_cleanup(done, op, "x"), "transfer after complete");
    expect_transition_error(settle_actor(done, op), "settle after complete");
    expect_transition_error(record_actor_receipt(done, op, receipt), "receipt after complete");
    EXPECT_FALSE(transfer_actor_cleanup(pending(), op, ""));
  }

  TEST(TopologyActorRecord, StaleOperationCannotAdvanceRecord) {
    for (const auto &[record, what] : { std::pair { intent(), "intent" }, std::pair { launched(), "launched" }, std::pair { pending(), "pending" } }) {
      SCOPED_TRACE(what);
      for (const auto &result : { record_actor_receipt(record, op + 1, receipt), settle_actor(record, op + 1),
             complete_actor_cleanup(record, op + 1), transfer_actor_cleanup(record, op + 1, "x") }) {
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, error_code_e::stale_token);
      }
    }
  }

  TEST(TopologyActorRecord, SerializationRoundTripsEveryStage) {
    const auto unknown_boot = *record_actor_receipt(intent(std::nullopt), op, receipt);
    for (const auto &record : { intent(), launched(), pending(), *settle_actor(intent(), op),
           *complete_actor_cleanup(pending(), op), *transfer_actor_cleanup(pending(), op, "journal"), unknown_boot }) {
      SCOPED_TRACE(std::string { to_string(record.stage) });
      const auto bytes = serialize_actor_record(record);
      ASSERT_TRUE(bytes);
      const auto back = deserialize_actor_record(*bytes);
      ASSERT_TRUE(back) << back.error().detail;
      EXPECT_EQ(*back, record);
    }
  }

  TEST(TopologyActorRecord, DeserializationRejectsInconsistentRecords) {
    auto text = *serialize_actor_record(launched());
    const auto replaced = [&](std::string_view from, std::string_view to) {
      auto copy = text;
      const auto at = copy.find(from);
      EXPECT_NE(at, std::string::npos) << from;
      if (at != std::string::npos) copy.replace(at, from.size(), to);
      return copy;
    };
    const auto version = deserialize_actor_record(replaced("\"version\":1", "\"version\":2"));
    ASSERT_FALSE(version);
    EXPECT_EQ(version.error().code, error_code_e::unsupported_version);
    for (const auto &bad : {
           std::string { "" },
           std::string { "{" },
           std::string { "[]" },
           replaced("\"stage\":\"launched\"", "\"stage\":\"exploded\""),
           replaced("\"stage\":\"launched\"", "\"stage\":\"intent\""),
           replaced("\"operation\":41", "\"operation\":0"),
           replaced("\"pid\":4242", "\"pid\":0"),
           replaced("\"obligation\":\"restore_legacy_display\"", "\"obligation\":\"format_disk\""),
         }) {
      SCOPED_TRACE(bad);
      const auto result = deserialize_actor_record(bad);
      ASSERT_FALSE(result);
      EXPECT_EQ(result.error().code, error_code_e::invalid_journal);
    }
    auto pending_text = *serialize_actor_record(*settle_actor(intent(), op));
    const auto at = pending_text.find("\"stage\":\"cleanup_pending\"");
    ASSERT_NE(at, std::string::npos);
    pending_text.replace(at, std::string_view { "\"stage\":\"cleanup_pending\"" }.size(), "\"stage\":\"launched\"");
    EXPECT_FALSE(deserialize_actor_record(pending_text)) << "launched without receipt";
    auto transfer_text = *serialize_actor_record(*transfer_actor_cleanup(pending(), op, "journal"));
    const auto destination = transfer_text.find("\"transferred_to\":\"journal\"");
    ASSERT_NE(destination, std::string::npos);
    transfer_text.replace(destination, std::string_view { "\"transferred_to\":\"journal\"" }.size(), "\"transferred_to\":\"\"");
    EXPECT_FALSE(deserialize_actor_record(transfer_text)) << "transferred without destination";
  }

  TEST(TopologyActorRecordStore, CreateOnlyWhenNoRecordExists) {
    temp_dir_t dir;
    file_actor_record_store_t store { dir.file() };
    EXPECT_EQ(store.load().status, actor_load_status_e::absent);
    ASSERT_TRUE(store.create(intent()));
    const auto first = store.load();
    ASSERT_EQ(first.status, actor_load_status_e::loaded);
    EXPECT_EQ(first.record, intent());

    const auto bytes = read_all(dir.file());
    auto other = *begin_actor_intent(op + 7, std::string { "boot-a" }, actor_obligation_e::restore_legacy_display, "other");
    EXPECT_FALSE(store.create(other));
    EXPECT_EQ(read_all(dir.file()), bytes);

    write_all(dir.file(), "corrupt");
    EXPECT_EQ(store.load().status, actor_load_status_e::invalid);
    EXPECT_FALSE(store.create(other)) << "an unusable record is still a record";
    EXPECT_EQ(read_all(dir.file()), "corrupt");
  }

  TEST(TopologyActorRecordStore, ReplaceRequiresCurrentDurableRecord) {
    temp_dir_t dir;
    file_actor_record_store_t store { dir.file() };
    EXPECT_FALSE(store.replace(intent(), launched())) << "nothing to replace";
    EXPECT_FALSE(std::filesystem::exists(dir.file()));

    ASSERT_TRUE(store.create(intent()));
    ASSERT_TRUE(store.replace(intent(), launched()));
    EXPECT_EQ(store.load().record, launched());

    const auto bytes = read_all(dir.file());
    EXPECT_FALSE(store.replace(intent(), pending())) << "stale expected copy";
    EXPECT_EQ(read_all(dir.file()), bytes);
    EXPECT_FALSE(store.replace(intent(), *settle_actor(intent(), op))) << "a stale copy's legal edge must not drop the durable receipt";
    EXPECT_EQ(read_all(dir.file()), bytes);

    auto foreign = launched();
    foreign.operation = op + 1;
    auto foreign_next = *settle_actor(foreign, op + 1);
    EXPECT_FALSE(store.replace(launched(), foreign_next)) << "next must keep the operation";
    EXPECT_EQ(read_all(dir.file()), bytes);

    ASSERT_TRUE(store.replace(launched(), pending()));
    EXPECT_EQ(store.load().record, pending());

    write_all(dir.file(), "corrupt");
    EXPECT_FALSE(store.replace(pending(), *complete_actor_cleanup(pending(), op)));
    EXPECT_EQ(read_all(dir.file()), "corrupt");
  }

  TEST(TopologyActorRecordStore, OnlyTerminalRecordCanBeRetired) {
    temp_dir_t dir;
    file_actor_record_store_t store { dir.file() };
    EXPECT_FALSE(store.retire(*complete_actor_cleanup(pending(), op))) << "nothing to retire";
    ASSERT_TRUE(store.create(intent()));
    for (const auto &record : { intent(), launched(), pending() }) {
      SCOPED_TRACE(std::string { to_string(record.stage) });
      ASSERT_EQ(store.load().record, record);
      EXPECT_FALSE(store.retire(record));
      EXPECT_TRUE(std::filesystem::exists(dir.file()));
      if (record.stage == actor_stage_e::intent) ASSERT_TRUE(store.replace(record, launched()));
      if (record.stage == actor_stage_e::launched) ASSERT_TRUE(store.replace(record, pending()));
    }
    const auto done = *complete_actor_cleanup(pending(), op);
    auto other_done = done;
    other_done.obligation_detail = "different";
    EXPECT_FALSE(store.retire(done)) << "durable record is still pending";
    ASSERT_TRUE(store.replace(pending(), done));
    EXPECT_FALSE(store.retire(other_done)) << "retire must name the durable record";
    EXPECT_TRUE(std::filesystem::exists(dir.file()));
    ASSERT_TRUE(store.retire(done));
    EXPECT_EQ(store.load().status, actor_load_status_e::absent);
    EXPECT_TRUE(store.create(intent())) << "a new operation may begin after retirement";
  }

  TEST(TopologyActorRecordStore, LeftoverTempFileIsIgnoredAndUnsupportedVersionIsReported) {
    temp_dir_t dir;
    file_actor_record_store_t store { dir.file() };
    write_all(durable_temp_path(dir.file()), *serialize_actor_record(launched()));
    EXPECT_EQ(store.load().status, actor_load_status_e::absent);
    EXPECT_FALSE(store.create(launched())) << "a new record must start as intent";
    ASSERT_TRUE(store.create(intent()));
    EXPECT_EQ(store.load().record, intent());

    auto text = *serialize_actor_record(launched());
    text.replace(text.find("\"version\":1"), std::string_view { "\"version\":1" }.size(), "\"version\":9");
    write_all(dir.file(), text);
    EXPECT_EQ(store.load().status, actor_load_status_e::unsupported_version);
  }

  TEST(TopologyActorRecord, DeserializationRejectsDuplicateAndUnknownFields) {
    const auto text = *serialize_actor_record(launched());
    const auto inserted = [&](std::string_view after, std::string_view addition) {
      auto copy = text;
      const auto at = copy.find(after);
      EXPECT_NE(at, std::string::npos) << after;
      if (at != std::string::npos) copy.insert(at + after.size(), addition);
      return copy;
    };
    for (const auto &bad : {
           inserted("{", "\"operation\":7,"),
           inserted("{", "\"stage\":\"completed\","),
           inserted("\"receipt\":{", "\"pid\":7,"),
           inserted("\"receipt\":{", "\"creation_time\":7,"),
           inserted("{", "\"extra\":1,"),
           inserted("\"receipt\":{", "\"handle\":1,"),
         }) {
      SCOPED_TRACE(bad);
      const auto result = deserialize_actor_record(bad);
      ASSERT_FALSE(result);
      EXPECT_EQ(result.error().code, error_code_e::invalid_journal);
    }
  }

  TEST(TopologyActorRecordStore, ReplaceAcceptsOnlyLifecycleEdgesWithImmutableObligation) {
    temp_dir_t dir;
    file_actor_record_store_t store { dir.file() };
    const auto expect_refused = [&](const actor_record_t &current, const actor_record_t &next, const char *what) {
      SCOPED_TRACE(what);
      std::filesystem::remove(dir.file());
      write_all(dir.file(), *serialize_actor_record(current));
      const auto bytes = read_all(dir.file());
      EXPECT_FALSE(store.replace(current, next));
      EXPECT_EQ(read_all(dir.file()), bytes);
    };
    const auto expect_accepted = [&](const actor_record_t &current, const actor_record_t &next, const char *what) {
      SCOPED_TRACE(what);
      std::filesystem::remove(dir.file());
      write_all(dir.file(), *serialize_actor_record(current));
      EXPECT_TRUE(store.replace(current, next));
      EXPECT_EQ(store.load().record, next);
    };
    const auto done = *complete_actor_cleanup(pending(), op);
    const auto moved = *transfer_actor_cleanup(pending(), op, "journal");

    expect_accepted(intent(), launched(), "intent -> launched");
    expect_accepted(intent(), *settle_actor(intent(), op), "intent -> cleanup_pending");
    expect_accepted(launched(), pending(), "launched -> cleanup_pending");
    expect_accepted(pending(), done, "cleanup_pending -> completed");
    expect_accepted(pending(), moved, "cleanup_pending -> transferred");

    auto skipped = intent();
    skipped.stage = actor_stage_e::completed;
    expect_refused(intent(), skipped, "intent -> completed");
    auto launched_done = launched();
    launched_done.stage = actor_stage_e::completed;
    expect_refused(launched(), launched_done, "launched -> completed");
    auto regressed = pending();
    regressed.stage = actor_stage_e::launched;
    expect_refused(pending(), regressed, "cleanup_pending -> launched");
    expect_refused(launched(), launched(), "launched -> launched");
    expect_refused(done, moved, "completed -> transferred");

    auto other_boot = launched();
    other_boot.boot = "boot-b";
    expect_refused(intent(), other_boot, "boot identity changed");
    auto other_detail = pending();
    other_detail.obligation_detail = "something else";
    expect_refused(launched(), other_detail, "obligation detail changed");
    auto other_receipt = pending();
    other_receipt.receipt = actor_receipt_t { receipt.pid + 1, receipt.creation_time };
    expect_refused(launched(), other_receipt, "receipt changed");
    auto dropped_receipt = pending();
    dropped_receipt.receipt.reset();
    expect_refused(launched(), dropped_receipt, "receipt dropped");
    auto late_receipt = *settle_actor(intent(), op);
    late_receipt.receipt = receipt;
    expect_refused(intent(), late_receipt, "receipt invented while settling");
  }

  TEST(TopologyActorRecordStore, StoreInstancesForOnePathShareOneLock) {
    temp_dir_t dir;
    const auto lock = actor_record_path_lock(dir.file().parent_path() / "." / dir.file().filename());
    ASSERT_EQ(lock, actor_record_path_lock(dir.file())) << "spellings of one path share a lock";
    ASSERT_NE(lock, actor_record_path_lock(dir.file().parent_path() / "other.json"));
    std::unique_lock held { *lock };
    std::atomic<bool> done {};
    std::thread writer { [&] {
      file_actor_record_store_t store { dir.file() };
      EXPECT_TRUE(store.create(intent()));
      done = true;
    } };
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    EXPECT_FALSE(done.load()) << "a second store instance bypassed the path lock";
    EXPECT_FALSE(std::filesystem::exists(dir.file()));
    held.unlock();
    writer.join();
    EXPECT_TRUE(done.load());
  }

  TEST(TopologyActorRecordStore, StoresOnOnePathShareCompareAndReplace) {
    temp_dir_t dir;
    constexpr int contenders = 8;
    for (int round = 0; round < 40; ++round) {
      std::filesystem::remove(dir.file());
      std::atomic<int> ready {};
      std::atomic<int> created {};
      std::atomic<int> advanced {};
      std::vector<std::thread> threads;
      for (int index = 0; index < contenders; ++index) {
        threads.emplace_back([&] {
          file_actor_record_store_t store { dir.file() };
          ready.fetch_add(1);
          while (ready.load() < contenders) std::this_thread::yield();
          if (store.create(intent())) created.fetch_add(1);
          while (store.load().status == actor_load_status_e::absent) std::this_thread::yield();
          if (store.replace(intent(), launched())) advanced.fetch_add(1);
        });
      }
      for (auto &thread : threads) thread.join();
      ASSERT_EQ(created.load(), 1) << "round " << round;
      ASSERT_EQ(advanced.load(), 1) << "round " << round;
      ASSERT_EQ(file_actor_record_store_t { dir.file() }.load().record, launched());
    }
  }

  TEST(TopologyActorRecordStore, OversizedRecordIsNotTruncatedIntoAValidOne) {
    temp_dir_t dir;
    file_actor_record_store_t store { dir.file() };
    auto exact = *serialize_actor_record(pending());
    exact.resize(actor_record_read_limit, ' ');
    write_all(dir.file(), exact);
    ASSERT_EQ(store.load().status, actor_load_status_e::loaded) << "exactly at the limit";

    write_all(dir.file(), exact + " ");
    EXPECT_EQ(store.load().status, actor_load_status_e::invalid) << "a parseable record past the limit is still corrupt";

    write_all(dir.file(), exact + "garbage");
    EXPECT_NE(store.load().status, actor_load_status_e::loaded);
    EXPECT_NE(store.load().status, actor_load_status_e::absent);
    EXPECT_FALSE(store.create(intent()));
    EXPECT_EQ(classify_actor_recovery(store.load(), std::string { "boot-a" }, never_observed()).disposition, actor_recovery_e::blocked);
  }

  TEST(TopologyActorRecovery, MalformedLoadedRecordStaysBlocked) {
    auto no_receipt = launched();
    no_receipt.receipt.reset();
    auto zero_pid = launched();
    zero_pid.receipt->pid = 0;
    auto unknown_stage = launched();
    unknown_stage.stage = static_cast<actor_stage_e>(99);
    auto undirected = *transfer_actor_cleanup(pending(), op, "journal");
    undirected.transferred_to.clear();
    auto stray_destination = pending();
    stray_destination.transferred_to = "journal";
    auto no_operation = pending();
    no_operation.operation = 0;
    for (const auto &[record, what] : { std::pair { no_receipt, "launched without receipt" }, std::pair { zero_pid, "zero pid" },
           std::pair { unknown_stage, "unknown stage" }, std::pair { undirected, "transfer without destination" },
           std::pair { stray_destination, "pending with destination" }, std::pair { no_operation, "no operation" } }) {
      SCOPED_TRACE(what);
      EXPECT_EQ(classify_actor_recovery(loaded(record), std::string { "boot-a" }, never_observed()).disposition, actor_recovery_e::blocked);
    }
  }

  TEST(TopologyActorRecovery, AbsentRecordNeedsNothing) {
    EXPECT_EQ(classify_actor_recovery({}, std::string { "boot-a" }, never_observed()).disposition, actor_recovery_e::none);
  }

  TEST(TopologyActorRecovery, UnusableRecordStaysBlocked) {
    for (const auto status : { actor_load_status_e::invalid, actor_load_status_e::unsupported_version, actor_load_status_e::unreadable }) {
      EXPECT_EQ(classify_actor_recovery({ status, std::nullopt, "bad" }, std::string { "boot-a" }, never_observed()).disposition,
        actor_recovery_e::blocked);
    }
  }

  TEST(TopologyActorRecovery, IntentWithoutReceiptBlocksUnlessBootPositivelyChanged) {
    EXPECT_EQ(classify_actor_recovery(loaded(intent()), std::string { "boot-a" }, never_observed()).disposition, actor_recovery_e::blocked);
    EXPECT_EQ(classify_actor_recovery(loaded(intent()), std::nullopt, never_observed()).disposition, actor_recovery_e::blocked);
    EXPECT_EQ(classify_actor_recovery(loaded(intent(std::nullopt)), std::string { "boot-b" }, never_observed()).disposition,
      actor_recovery_e::blocked);
    const auto changed = classify_actor_recovery(loaded(intent()), std::string { "boot-b" }, never_observed());
    EXPECT_EQ(changed.disposition, actor_recovery_e::actor_settled);
    EXPECT_EQ(changed.record, intent());
  }

  TEST(TopologyActorRecovery, LaunchedActorIsObservedOnTheSameBoot) {
    const std::pair<process_observation_e, actor_recovery_e> table[] {
      { process_observation_e::running, actor_recovery_e::actor_running },
      { process_observation_e::exited, actor_recovery_e::actor_settled },
      { process_observation_e::absent, actor_recovery_e::actor_settled },
      { process_observation_e::identity_mismatch, actor_recovery_e::actor_settled },
      { process_observation_e::inaccessible, actor_recovery_e::blocked },
    };
    for (const auto &[observation, expected] : table) {
      for (const auto &boot : { std::optional<std::string> { "boot-a" }, std::optional<std::string> {} }) {
        int calls {};
        const auto result = classify_actor_recovery(loaded(launched()), boot, observing(observation, &calls));
        EXPECT_EQ(result.disposition, expected) << static_cast<int>(observation) << " boot known " << boot.has_value();
        EXPECT_EQ(calls, 1);
        EXPECT_EQ(result.record, launched());
      }
    }
  }

  TEST(TopologyActorRecovery, LaunchedActorSettlesOnPositiveBootChangeWithoutObservation) {
    EXPECT_EQ(classify_actor_recovery(loaded(launched()), std::string { "boot-b" }, never_observed()).disposition,
      actor_recovery_e::actor_settled);
    int calls {};
    EXPECT_EQ(classify_actor_recovery(loaded(launched(std::nullopt)), std::string { "boot-b" },
                observing(process_observation_e::running, &calls)).disposition,
      actor_recovery_e::actor_running)
      << "an unknown recorded boot is not a positive change";
    EXPECT_EQ(calls, 1);
  }

  TEST(TopologyActorRecovery, ObservationFailureStaysBlocked) {
    const auto throwing = [](const actor_receipt_t &) -> process_observation_e { throw std::runtime_error { "denied" }; };
    EXPECT_EQ(classify_actor_recovery(loaded(launched()), std::string { "boot-a" }, throwing).disposition, actor_recovery_e::blocked);
    EXPECT_EQ(classify_actor_recovery(loaded(launched()), std::string { "boot-a" }, {}).disposition, actor_recovery_e::blocked);
  }

  TEST(TopologyActorRecovery, PersistedCleanupIsNeverDroppedByBootOrProcessState) {
    for (const auto &boot : { std::optional<std::string> { "boot-a" }, std::optional<std::string> { "boot-b" }, std::optional<std::string> {} }) {
      EXPECT_EQ(classify_actor_recovery(loaded(pending()), boot, never_observed()).disposition, actor_recovery_e::cleanup_pending);
      EXPECT_EQ(classify_actor_recovery(loaded(*complete_actor_cleanup(pending(), op)), boot, never_observed()).disposition,
        actor_recovery_e::finished);
      EXPECT_EQ(classify_actor_recovery(loaded(*transfer_actor_cleanup(pending(), op, "journal")), boot, never_observed()).disposition,
        actor_recovery_e::finished);
    }
  }

}  // namespace display_device::topology
