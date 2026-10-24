#include <gtest/gtest.h>

#include "display_topology_fixtures.h"
#include "src/display_device/topology_journal.h"

namespace {
  using namespace display_topology_test;

  dt::journal_record_t
  journal() {
    const auto fixture = mixed();
    dt::journal_record_t record { 1, id<dt::owner_id_t>(50), id<dt::transaction_id_t>(80), id<dt::request_id_t>(70),
      dt::topology_revision_t { 10 }, fixture.inventory, {}, {}, {} };
    const auto bindings = dt::resolve_profile(fixture.profile, fixture.inventory);
    dt::managed_values_t baseline;
    const auto &actual = *fixture.inventory.displays[0].layout;
    baseline.position = actual.position;
    baseline.mode = actual.mode;
    baseline.rotation = actual.rotation;
    baseline.scale_percent = actual.scale_percent;
    baseline.hdr = actual.hdr;
    baseline.primary = false;
    auto intended = baseline;
    intended.position->y = -200;
    record.managed.push_back({ *bindings[0].binding, baseline, intended, intended });
    for (const auto &head : fixture.inventory.heads) record.owned_heads.push_back({ head.identity, *head.owner, head.adapter, head.slot });
    auto observed = fixture.inventory;
    observed.revision = dt::inventory_revision_t { 8 };
    observed.displays[0].layout->position.y = -200;
    record.steps = {
      { 1, dt::journal_operation_e::create_head, {}, id<dt::head_guid_t>(99), dt::journal_step_state_e::attempted, {}, {} },
      { 2, dt::journal_operation_e::apply_fields, dt::os_display_id_t { "os-A" }, {}, dt::journal_step_state_e::observed, observed, {} },
      { 3, dt::journal_operation_e::remove_head, {}, id<dt::head_guid_t>(99), dt::journal_step_state_e::failed, {}, "Departure needs retry" },
      { 4, dt::journal_operation_e::restore_fields, dt::os_display_id_t { "os-A" }, {}, dt::journal_step_state_e::conflict, observed, "External edit" }
    };
    record.audio = dt::managed_audio_t { *record.baseline.audio_endpoint, "temporary-endpoint", "temporary-endpoint" };
    return record;
  }

  TEST(DisplayTopologyJournal, RoundTripRetainsBaselineManagedFieldsFullHeadsAndStepEvidence) {
    auto record = journal();
    record.owned_heads[0].identity.incarnation = dt::head_incarnation_t { 0x1'0001'0001ULL };
    record.managed[0].source.incarnation = dt::source_incarnation_t { 0x2'0000'0001ULL };
    record.baseline.displays[0].incarnation = record.managed[0].source.incarnation;
    const auto serialized = dt::serialize_journal(record);
    ASSERT_TRUE(serialized) << (serialized ? "" : serialized.error().detail);
    const auto decoded = dt::deserialize_journal(*serialized);
    ASSERT_TRUE(decoded) << (decoded ? "" : decoded.error().detail);
    EXPECT_EQ(decoded->audio, record.audio);
    EXPECT_EQ(*decoded, record);
    EXPECT_NE(decoded->baseline, *decoded->steps[1].observed);
    EXPECT_NE(decoded->owned_heads[0].identity.guid, decoded->owned_heads[1].identity.guid);
  }

  TEST(DisplayTopologyJournal, FirstBaselineSurvivesRetriedPrepare) {
    const auto fixture = physical();
    const auto first = dt::prepare_journal({}, id<dt::owner_id_t>(50), id<dt::transaction_id_t>(80),
      id<dt::request_id_t>(70), dt::topology_revision_t { 10 }, fixture.inventory);
    ASSERT_TRUE(first);
    auto changed = fixture.inventory;
    changed.displays[0].layout = layout(5000);
    changed.audio_endpoint = "temporary-virtual-endpoint";
    const auto retry = dt::prepare_journal(*first, first->owner, first->transaction, first->request, first->base, changed);
    ASSERT_TRUE(retry);
    EXPECT_EQ(*retry, *first);
    const auto conflict = dt::prepare_journal(*first, first->owner, id<dt::transaction_id_t>(81), first->request, first->base, changed);
    ASSERT_FALSE(conflict);
    EXPECT_EQ(conflict.error().code, dt::error_code_e::request_conflict);
  }

  TEST(DisplayTopologyJournal, UnknownVersionsAreRejectedBeforeRecovery) {
    auto record = journal();
    record.version = 2;
    const auto invalid = dt::serialize_journal(record);
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().code, dt::error_code_e::unsupported_version);
    const auto minimal = dt::deserialize_journal(R"({"version":99})");
    ASSERT_FALSE(minimal);
    EXPECT_EQ(minimal.error().code, dt::error_code_e::unsupported_version);
  }

  TEST(DisplayTopologyJournal, MalformedAndTruncatedIdentityDataAreNotAccepted) {
    const auto serialized = dt::serialize_journal(journal());
    ASSERT_TRUE(serialized);
    using json = nlohmann::json;
    const auto original = json::parse(*serialized);
    std::vector<json> malformed;
    auto value = original;
    value["owner"] = json::array({ 1, 2, 3, 4 });
    malformed.push_back(value);
    value = original;
    value["ownedHeads"][0]["identity"]["incarnation"] = -1;
    malformed.push_back(value);
    value = original;
    value["baseline"]["displays"][0]["layout"]["position"]["x"] = 2147483648ULL;
    malformed.push_back(value);
    value = original;
    value["baseline"]["displays"][0]["layout"]["mode"]["denominator"] = 0;
    malformed.push_back(value);
    value = original;
    value["steps"][0]["state"] = 99;
    malformed.push_back(value);
    value = original;
    value["managed"][0]["source"]["incarnation"] = 1.5;
    malformed.push_back(value);
    value = original;
    value["owner"][0] = 256;
    malformed.push_back(value);
    value = original;
    value["steps"][0]["headIntent"] = json::array({ 0x12, 0x34, 0x56, 0x78 });
    malformed.push_back(value);
    for (std::size_t i = 0; i < malformed.size(); ++i) {
      SCOPED_TRACE(i);
      const auto result = dt::deserialize_journal(malformed[i].dump());
      ASSERT_FALSE(result);
      EXPECT_EQ(result.error().code, dt::error_code_e::invalid_journal);
    }
    EXPECT_TRUE(error_is(dt::deserialize_journal("{"), dt::error_code_e::invalid_journal));
    EXPECT_TRUE(error_is(dt::deserialize_journal(std::string(1024 * 1024 + 1, ' ')), dt::error_code_e::invalid_journal));
  }

  TEST(DisplayTopologyJournal, UnknownBaselineCannotAuthorizeMutation) {
    auto fixture = physical();
    fixture.inventory.displays[1].layout.reset();
    auto result = dt::prepare_journal({}, id<dt::owner_id_t>(50), id<dt::transaction_id_t>(80),
      id<dt::request_id_t>(70), {}, fixture.inventory);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, dt::error_code_e::invalid_journal);
    fixture = physical();
    fixture.inventory.available = false;
    result = dt::prepare_journal({}, id<dt::owner_id_t>(50), id<dt::transaction_id_t>(80), id<dt::request_id_t>(70), {}, fixture.inventory);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, dt::error_code_e::invalid_journal);
  }

  TEST(DisplayTopologyJournal, OwnedHeadTokensCannotClaimForeignOwnership) {
    auto record = journal();
    record.owned_heads[0].owner = id<dt::owner_id_t>(99);
    const auto invalid = dt::serialize_journal(record);
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().code, dt::error_code_e::invalid_journal);
  }

  TEST(DisplayTopologyJournal, ManagedFieldsRequireBaselineAndCannotExpandOnReadback) {
    auto record = journal();
    record.managed[0].baseline.position.reset();
    auto invalid = dt::serialize_journal(record);
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().code, dt::error_code_e::invalid_journal);
    record = journal();
    record.baseline.audio_endpoint.reset();
    EXPECT_FALSE(dt::serialize_journal(record));
    record = journal();
    record.managed[0].intended.primary.reset();
    invalid = dt::serialize_journal(record);
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().code, dt::error_code_e::invalid_journal);
  }

  TEST(DisplayTopologyJournal, AttemptedIsNotObservedAndConflictsNeedDiagnostics) {
    auto record = journal();
    record.steps[1].observed.reset();
    auto invalid = dt::serialize_journal(record);
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().code, dt::error_code_e::invalid_journal);
    record = journal();
    record.steps[0].observed = record.baseline;
    EXPECT_FALSE(dt::serialize_journal(record));
    record = journal();
    record.steps[3].detail.clear();
    EXPECT_FALSE(dt::serialize_journal(record));
  }

  TEST(DisplayTopologyJournal, ManagedBaselineMustMatchObservedIdentityAndValues) {
    for (const bool identity : { false, true }) {
      auto record = journal();
      if (identity)
        record.managed[0].source.incarnation = dt::source_incarnation_t { 999 };
      else
        record.managed[0].baseline.position->x = -9999;
      const auto invalid = dt::serialize_journal(record);
      ASSERT_FALSE(invalid) << "Restore baseline must not refer to a different incarnation or invented field value";
      EXPECT_EQ(invalid.error().code, dt::error_code_e::invalid_journal);
    }
  }
}  // namespace
