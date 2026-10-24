#include <gtest/gtest.h>

#include <algorithm>
#include <functional>
#include <limits>
#include <tuple>
#include <type_traits>

#include "display_topology_fixtures.h"

namespace {
  using namespace display_topology_test;
  using namespace std::chrono_literals;

  template <class A, class B>
  constexpr bool distinct = !std::is_same_v<A, B> &&
                            !std::is_convertible_v<A, B> && !std::is_constructible_v<A, B> && !std::is_assignable_v<B &, A>;

  constexpr bool
  separate(std::tuple<>) {
    return true;
  }

  template <class First, class... Rest>
  constexpr bool
  separate(std::tuple<First, Rest...>) {
    return ((distinct<First, Rest> && distinct<Rest, First>) && ...) && separate(std::tuple<Rest...> {});
  }

  TEST(DisplayTopologyIdentity, DomainsAreNotInterconvertible) {
    using domains = std::tuple<dt::profile_id_t, dt::monitor_id_t, dt::os_display_id_t,
      dt::head_guid_t, dt::driver_instance_id_t, dt::adapter_luid_t, dt::head_incarnation_t,
      dt::source_incarnation_t, dt::track_id_t, dt::track_epoch_t,
      dt::topology_revision_t, dt::inventory_revision_t, dt::ledger_revision_t>;
    EXPECT_TRUE(separate(domains {}));
    EXPECT_FALSE((std::is_same_v<dt::monitor_id_t, dt::head_guid_t>) );
    EXPECT_FALSE((std::is_same_v<dt::head_incarnation_t, dt::source_incarnation_t>) );
    EXPECT_FALSE((std::is_same_v<dt::track_id_t, dt::track_epoch_t>) );
    EXPECT_FALSE((std::is_convertible_v<dt::uuid_bytes_t, dt::monitor_id_t>) );
    EXPECT_FALSE((std::is_convertible_v<std::uint64_t, dt::track_id_t>) );
  }

  TEST(DisplayTopologyIdentity, FullGuidAndWideIncarnationsAreRetained) {
    const auto a = id<dt::head_guid_t>(10), b = id<dt::head_guid_t>(11);
    EXPECT_TRUE(std::equal(a.value.begin(), a.value.begin() + 4, b.value.begin()));
    EXPECT_NE(a, b);
    const dt::head_identity_t first { id<dt::driver_instance_id_t>(1), a, dt::head_incarnation_t { 0x1'0001'0001ULL } };
    auto second = first;
    second.incarnation = dt::head_incarnation_t { 1 };
    EXPECT_NE(first, second);
    second = first;
    second.driver = id<dt::driver_instance_id_t>(2);
    EXPECT_NE(first, second);
  }

  TEST(DisplayTopologyResolver, TwoPhysicalDisplaysBindExactIds) {
    const auto fixture = physical();
    const auto results = dt::resolve_profile(fixture.profile, fixture.inventory);
    ASSERT_EQ(results.size(), 2u);
    for (std::size_t i = 0; i < results.size(); ++i) {
      EXPECT_EQ(results[i].state, dt::binding_state_e::bound);
      ASSERT_TRUE(results[i].binding);
      EXPECT_EQ(results[i].binding->display, fixture.inventory.displays[i].id);
      EXPECT_EQ(results[i].binding->incarnation, fixture.inventory.displays[i].incarnation);
      EXPECT_FALSE(results[i].binding->head);
      EXPECT_FALSE(results[i].error);
    }
  }

  TEST(DisplayTopologyResolver, SameSizedData1CollidingVirtualHeadsStayDistinct) {
    const auto fixture = mixed();
    const auto results = dt::resolve_profile(fixture.profile, fixture.inventory);
    ASSERT_EQ(results.size(), 3u);
    ASSERT_TRUE(results[0].binding);
    EXPECT_FALSE(results[0].binding->head);
    for (std::size_t i = 1; i < 3; ++i) {
      ASSERT_TRUE(results[i].binding);
      ASSERT_TRUE(results[i].binding->head);
      EXPECT_EQ(results[i].binding->head->identity, fixture.inventory.heads[i - 1].identity);
      EXPECT_EQ(results[i].binding->head->owner, *fixture.inventory.heads[i - 1].owner);
    }
    EXPECT_NE(results[1].binding, results[2].binding);
  }

  TEST(DisplayTopologyResolver, NamesAndEnumerationOrderHaveNoAuthority) {
    auto fixture = mixed();
    const auto original = dt::resolve_profile(fixture.profile, fixture.inventory);
    std::reverse(fixture.inventory.displays.begin(), fixture.inventory.displays.end());
    std::reverse(fixture.inventory.heads.begin(), fixture.inventory.heads.end());
    for (auto &display : fixture.inventory.displays) {
      display.info.display_name = "DISPLAY1";
      display.info.friendly_name = "Same renamed monitor";
    }
    EXPECT_EQ(dt::resolve_profile(fixture.profile, fixture.inventory), original);
  }

  TEST(DisplayTopologyResolver, MissingAndAmbiguousPhysicalIdsRequireRebind) {
    for (const bool duplicate : { false, true }) {
      SCOPED_TRACE(duplicate ? "duplicate ID evidence" : "port move");
      auto fixture = physical();
      if (duplicate)
        fixture.inventory.displays.push_back(fixture.inventory.displays[0]);
      else
        fixture.inventory.displays[0].id = dt::os_display_id_t { "new-port-same-name-and-mode" };
      const auto result = dt::resolve_profile(fixture.profile, fixture.inventory)[0];
      EXPECT_EQ(result.state, dt::binding_state_e::needs_rebind);
      EXPECT_FALSE(result.binding);
      ASSERT_TRUE(result.error);
      EXPECT_EQ(result.error->code, duplicate ? dt::error_code_e::ambiguous_identity : dt::error_code_e::missing_identity);
    }
  }

  TEST(DisplayTopologyResolver, UnprovenVirtualOwnershipIsUnavailable) {
    for (const bool foreign : { false, true }) {
      auto fixture = mixed();
      fixture.inventory.heads[0].owner = foreign ? std::optional { id<dt::owner_id_t>(99) } : std::nullopt;
      const auto result = dt::resolve_profile(fixture.profile, fixture.inventory)[1];
      EXPECT_EQ(result.state, dt::binding_state_e::unavailable);
      EXPECT_FALSE(result.binding);
      ASSERT_TRUE(result.error);
      EXPECT_EQ(result.error->code, dt::error_code_e::not_owned);
    }
  }

  TEST(DisplayTopologyResolver, ProducerAndCaptureAdapterMismatchAreRejected) {
    for (const bool producer : { false, true }) {
      SCOPED_TRACE(producer ? "producer LUID" : "capture LUID");
      auto fixture = mixed();
      if (producer)
        fixture.inventory.heads[0].adapter = dt::adapter_luid_t { 8 };
      else
        fixture.inventory.displays[1].adapter = dt::adapter_luid_t { 8 };
      const auto result = dt::resolve_profile(fixture.profile, fixture.inventory)[1];
      EXPECT_FALSE(result.binding);
      ASSERT_TRUE(result.error);
      EXPECT_EQ(result.error->code, dt::error_code_e::adapter_mismatch);
    }
    auto fixture = physical();
    fixture.inventory.displays[0].adapter.reset();
    EXPECT_EQ(dt::resolve_profile(fixture.profile, fixture.inventory)[0].error->code, dt::error_code_e::adapter_mismatch);
  }

  TEST(DisplayTopologyResolver, FullRuntimeCorrelationMustAgree) {
    auto fixture = mixed();
    fixture.inventory.displays[1].head->incarnation = dt::head_incarnation_t { 999 };
    auto result = dt::resolve_profile(fixture.profile, fixture.inventory)[1];
    ASSERT_TRUE(result.error);
    EXPECT_EQ(result.error->code, dt::error_code_e::source_kind_mismatch);
    fixture = mixed();
    fixture.inventory.heads.push_back(fixture.inventory.heads[0]);
    fixture.inventory.heads.back().identity.driver = id<dt::driver_instance_id_t>(99);
    result = dt::resolve_profile(fixture.profile, fixture.inventory)[1];
    EXPECT_EQ(result.state, dt::binding_state_e::needs_rebind);
    ASSERT_TRUE(result.error);
    EXPECT_EQ(result.error->code, dt::error_code_e::ambiguous_identity);
  }

  TEST(DisplayTopologyResolver, RestartProducesANewBoundIncarnationNotANewLogicalMonitor) {
    auto fixture = mixed();
    const auto before = dt::resolve_profile(fixture.profile, fixture.inventory)[1];
    fixture.inventory.heads[0].identity.driver = id<dt::driver_instance_id_t>(99);
    fixture.inventory.heads[0].identity.incarnation = dt::head_incarnation_t { 0x1'0000'0001ULL };
    fixture.inventory.displays[1].head = fixture.inventory.heads[0].identity;
    fixture.inventory.displays[1].incarnation = dt::source_incarnation_t { 1000 };
    const auto after = dt::resolve_profile(fixture.profile, fixture.inventory)[1];
    EXPECT_EQ(after.state, dt::binding_state_e::bound);
    EXPECT_EQ(after.monitor, before.monitor);
    EXPECT_NE(after.binding, before.binding);
    ASSERT_TRUE(after.binding);
    ASSERT_TRUE(after.binding->head);
    EXPECT_EQ(after.binding->incarnation, fixture.inventory.displays[1].incarnation);
    EXPECT_EQ(after.binding->head->identity, fixture.inventory.heads[0].identity);
  }

  TEST(DisplayTopologyResolver, FailedObservationIsNotAnEmptyDesktop) {
    auto fixture = mixed();
    fixture.inventory.available = false;
    for (const auto &result : dt::resolve_profile(fixture.profile, fixture.inventory)) {
      EXPECT_EQ(result.state, dt::binding_state_e::unavailable);
      EXPECT_FALSE(result.binding);
      ASSERT_TRUE(result.error);
      EXPECT_EQ(result.error->code, dt::error_code_e::inventory_unavailable);
    }
  }

  TEST(DisplayTopologyValidation, RejectsInvalidProfilesWithTypedErrors) {
    struct case_t {
      const char *name;
      std::function<void(fixture_t &)> corrupt;
      dt::error_code_e expected;
    };
    const std::vector<case_t> cases {
      { "duplicate logical ID", [](auto &f) { f.profile.entries[1].id = f.profile.entries[0].id; }, dt::error_code_e::duplicate_monitor },
      { "duplicate physical source", [](auto &f) { f.profile.entries[1].source = f.profile.entries[0].source; }, dt::error_code_e::duplicate_source },
      { "duplicate disconnected physical source", [](auto &f) { f.profile.entries[1].source = f.profile.entries[0].source; f.inventory.displays.clear(); f.inventory.clone_groups.clear(); }, dt::error_code_e::duplicate_source },
      { "bad navigation permutation", [](auto &f) { f.profile.order[1] = f.profile.order[0]; }, dt::error_code_e::invalid_order },
      { "missing fallback", [](auto &f) { f.profile.fallback = id<dt::monitor_id_t>(99); }, dt::error_code_e::invalid_order },
      { "zero dimension", [](auto &f) { f.profile.entries[0].desired->mode.resolution.width = 0; }, dt::error_code_e::invalid_geometry },
      { "zero denominator", [](auto &f) { f.profile.entries[0].desired->mode.refresh_rate.denominator = 0; }, dt::error_code_e::invalid_refresh },
      { "zero numerator", [](auto &f) { f.profile.entries[0].desired->mode.refresh_rate.numerator = 0; }, dt::error_code_e::invalid_refresh },
      { "unsupported rotation", [](auto &f) { f.profile.entries[0].desired->rotation = static_cast<dt::rotation_e>(45); }, dt::error_code_e::unsupported_rotation },
      { "coordinate overflow", [](auto &f) { f.profile.entries[0].desired->position.x = std::numeric_limits<std::int32_t>::max(); }, dt::error_code_e::arithmetic_overflow },
      { "dimension policy", [](auto &f) { f.profile.limits.dimensions.width = 1000; }, dt::error_code_e::limit_exceeded },
      { "per-monitor rate policy", [](auto &f) { f.profile.limits.pixels_per_second = 1; }, dt::error_code_e::limit_exceeded },
      { "total rate policy", [](auto &f) { f.profile.limits.total_pixels_per_second = 1; }, dt::error_code_e::limit_exceeded },
      { "count policy", [](auto &f) { f.profile.limits.monitors = 1; }, dt::error_code_e::limit_exceeded },
      { "overlapping rectangles", [](auto &f) { f.profile.entries[0].desired->position.x = -100; }, dt::error_code_e::overlap },
      { "physical permissions", [](auto &f) { f.profile.permissions.change_physical = false; f.profile.entries[0].desired->position.y = 100; }, dt::error_code_e::permission_denied },
      { "primary permission", [](auto &f) { f.profile.permissions.primary = false; f.profile.entries[0].primary = dt::primary_intent_e::make_primary; }, dt::error_code_e::permission_denied },
      { "mode capability", [](auto &f) { f.profile.entries[0].desired->mode.refresh_rate = { 120, 1 }; }, dt::error_code_e::unsupported_mode },
      { "invalid identity", [](auto &f) { f.profile.entries[0].id = {}; }, dt::error_code_e::invalid_identity }
    };
    for (const auto &test : cases) {
      SCOPED_TRACE(test.name);
      auto fixture = physical();
      fixture.profile.permissions = edit_permissions();
      fixture.profile.entries[0].desired = fixture.inventory.displays[0].layout;
      test.corrupt(fixture);
      const auto result = dt::validate_profile(fixture.profile, fixture.inventory, dt::topology_revision_t { 1 }, dt::topology_revision_t { 1 });
      ASSERT_FALSE(result) << test.name;
      EXPECT_EQ(result.error().code, test.expected);
    }
  }

  TEST(DisplayTopologyValidation, NegativeCoordinatesGapsPortraitAndUnselectedPrimaryAreValid) {
    auto fixture = physical();
    fixture.inventory.displays[0].layout = layout(-1200, -1000, 1080, 1920);
    fixture.inventory.displays[0].layout->rotation = dt::rotation_e::clockwise90;
    fixture.inventory.displays[0].layout->scale_percent = 150;
    fixture.inventory.displays[0].layout->mode.refresh_rate = { 60000, 1001 };
    fixture.profile.entries.resize(1);
    fixture.profile.order.resize(1);
    fixture.profile.preferred = fixture.profile.entries[0].id;
    EXPECT_TRUE(dt::validate_profile(fixture.profile, fixture.inventory, dt::topology_revision_t { 1 }, dt::topology_revision_t { 1 }));
    const auto original = fixture.inventory;
    std::reverse(fixture.profile.order.begin(), fixture.profile.order.end());
    EXPECT_EQ(fixture.inventory, original);
  }

  TEST(DisplayTopologyValidation, StaleRevisionAndFailedInventoryAreTyped) {
    auto fixture = physical();
    auto result = dt::validate_profile(fixture.profile, fixture.inventory, dt::topology_revision_t { 1 }, dt::topology_revision_t { 2 });
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, dt::error_code_e::stale_revision);
    fixture.inventory.available = false;
    result = dt::validate_profile(fixture.profile, fixture.inventory, {}, {});
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, dt::error_code_e::inventory_unavailable);
  }

  TEST(DisplayTopologyValidation, CloneGroupsAreSourcesNotOrderedLayouts) {
    auto fixture = physical();
    fixture.inventory.clone_groups = { { "os-B", "os-A" } };
    fixture.inventory.displays[0].layout = fixture.inventory.displays[1].layout;
    auto result = dt::validate_profile(fixture.profile, fixture.inventory, {}, {});
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, dt::error_code_e::duplicate_source);
    fixture.profile.entries.resize(1);
    fixture.profile.order.resize(1);
    fixture.profile.preferred = fixture.profile.entries[0].id;
    EXPECT_TRUE(dt::validate_profile(fixture.profile, fixture.inventory, {}, {}));
    fixture.profile.permissions = edit_permissions();
    fixture.profile.entries[0].desired = fixture.inventory.displays[0].layout;
    fixture.profile.entries[0].desired->position.y = 2000;
    result = dt::validate_profile(fixture.profile, fixture.inventory, {}, {});
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, dt::error_code_e::partial_clone_edit);
  }

  TEST(DisplayTopologyValidation, HostOverlayIncludesUnmanagedDisplays) {
    auto fixture = physical();
    fixture.profile.entries.resize(1);
    fixture.profile.order.resize(1);
    fixture.profile.preferred = fixture.profile.entries[0].id;
    fixture.profile.permissions = edit_permissions();
    fixture.profile.entries[0].desired = layout(-100);
    const auto original = fixture.inventory;
    const auto result = dt::validate_profile(fixture.profile, fixture.inventory, {}, {});
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, dt::error_code_e::overlap);
    EXPECT_EQ(fixture.inventory, original);
  }

  TEST(DisplayTopologyValidation, RationalRateRoundsUpAndMultiplicationIsChecked) {
    auto fixture = physical();
    fixture.inventory.displays[0].layout->mode.refresh_rate = { 60000, 1001 };
    fixture.profile.entries.resize(1);
    fixture.profile.order.resize(1);
    fixture.profile.preferred = fixture.profile.entries[0].id;
    fixture.profile.limits.pixels_per_second = (1920ULL * 1080 * 60000) / 1001;
    const auto fractional = dt::validate_profile(fixture.profile, fixture.inventory, {}, {});
    ASSERT_FALSE(fractional);
    EXPECT_EQ(fractional.error().code, dt::error_code_e::limit_exceeded);
    fixture.profile.limits = { 8, 8, { UINT32_MAX, UINT32_MAX }, UINT64_MAX, UINT64_MAX };
    fixture.inventory.displays[0].layout = layout(INT32_MIN, INT32_MIN, UINT32_MAX, UINT32_MAX);
    fixture.inventory.displays[0].layout->mode.refresh_rate = { UINT32_MAX, 1 };
    const auto overflow = dt::validate_profile(fixture.profile, fixture.inventory, {}, {});
    ASSERT_FALSE(overflow);
    EXPECT_EQ(overflow.error().code, dt::error_code_e::arithmetic_overflow);
  }

  TEST(DisplayTopologyValidation, VirtualSourcesAndAdapterFailuresRemainExplicit) {
    auto fixture = mixed();
    EXPECT_TRUE(dt::validate_profile(fixture.profile, fixture.inventory, {}, {}));
    fixture.profile.entries[2].source = fixture.profile.entries[1].source;
    EXPECT_TRUE(error_is(dt::validate_profile(fixture.profile, fixture.inventory, {}, {}), dt::error_code_e::duplicate_source));
    fixture = mixed();
    fixture.profile.limits.virtual_monitors = 1;
    EXPECT_TRUE(error_is(dt::validate_profile(fixture.profile, fixture.inventory, {}, {}), dt::error_code_e::limit_exceeded));
    fixture = mixed();
    fixture.inventory.heads[0].adapter = dt::adapter_luid_t { 8 };
    EXPECT_TRUE(error_is(dt::validate_profile(fixture.profile, fixture.inventory, {}, {}), dt::error_code_e::adapter_mismatch));
  }

  TEST(DisplayTopologyRequests, ReplayPrecedesStalenessAndHistoryLimit) {
    const auto fixture = physical();
    const auto requested = request(fixture);
    dt::request_history_t history(1);
    const auto first = history.remember(requested, requested.base);
    ASSERT_TRUE(first);
    const auto replay = history.remember(requested, dt::topology_revision_t { 99 });
    ASSERT_TRUE(replay);
    EXPECT_EQ(*replay, *first);
    auto next = requested;
    next.request = id<dt::request_id_t>(71);
    const auto full = history.remember(next, next.base);
    ASSERT_FALSE(full);
    EXPECT_EQ(full.error().code, dt::error_code_e::history_full);
    next = requested;
    next.profile.order = { next.profile.order[1], next.profile.order[0] };
    const auto conflict = history.remember(next, next.base);
    ASSERT_FALSE(conflict);
    EXPECT_EQ(conflict.error().code, dt::error_code_e::request_conflict);
  }

  TEST(DisplayTopologyRequests, IdempotencyIsScopedToOwnerAndCompleteBody) {
    auto fixture = physical();
    dt::request_history_t history(3);
    auto first = request(fixture);
    ASSERT_TRUE(history.remember(first, first.base));
    auto other_owner = first;
    other_owner.owner = id<dt::owner_id_t>(51);
    const auto other = history.remember(other_owner, other_owner.base);
    ASSERT_TRUE(other);
    EXPECT_EQ(other->sequence, 2u);
    first.profile.entries[0].required = false;
    const auto conflict = history.remember(first, first.base);
    ASSERT_FALSE(conflict);
    EXPECT_EQ(conflict.error().code, dt::error_code_e::request_conflict);
  }

  TEST(DisplayTopologyRequests, AppliedReadbackCannotBeRequestedOrImplicitlyAdjusted) {
    auto fixture = physical();
    fixture.profile.entries[0].desired = fixture.inventory.displays[0].layout;
    auto requested = request(fixture);
    auto actual = applied(fixture);
    EXPECT_TRUE(dt::validate_applied(requested, actual));
    actual.monitors[0].layout.reset();
    auto invalid = dt::validate_applied(requested, actual);
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().code, dt::error_code_e::unknown_applied_state);
    actual = applied(fixture);
    actual.monitors[0].layout->mode.resolution.width = 1280;
    invalid = dt::validate_applied(requested, actual);
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().code, dt::error_code_e::permission_denied);
    requested.profile.allow_adjustments = true;
    actual.monitors[0].state = dt::applied_state_e::adjusted;
    actual.monitors[0].reason = "OS selected supported mode";
    EXPECT_TRUE(dt::validate_applied(requested, actual));
  }

  TEST(DisplayTopologyRequests, EveryUnavailableEntryNeedsAuthorizedDegradationAndReason) {
    const auto fixture = physical();
    auto requested = request(fixture);
    auto actual = applied(fixture);
    auto &missing = actual.monitors[1];
    missing.state = dt::applied_state_e::unavailable;
    missing.source.reset();
    missing.layout.reset();
    missing.host_primary.reset();
    missing.reason = "Disconnected";
    EXPECT_FALSE(dt::validate_applied(requested, actual));
    requested.profile.allow_degraded = true;
    requested.profile.entries[1].required = false;
    EXPECT_TRUE(dt::validate_applied(requested, actual));
    missing.reason.clear();
    const auto invalid = dt::validate_applied(requested, actual);
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().code, dt::error_code_e::unknown_applied_state);
  }

  TEST(DisplayTopologyRequests, CompletionIsExactAndIdempotent) {
    const auto fixture = mixed();
    const auto requested = request(fixture);
    auto actual = applied(fixture);
    dt::request_history_t history(2);
    ASSERT_TRUE(history.remember(requested, requested.base));
    const auto first = history.complete(actual);
    ASSERT_TRUE(first);
    ASSERT_TRUE(first->applied);
    EXPECT_EQ(*first->applied, actual);
    const auto replay = history.complete(actual);
    ASSERT_TRUE(replay);
    EXPECT_EQ(*replay, *first);
    actual.revision.value += 1;
    const auto conflict = history.complete(actual);
    ASSERT_FALSE(conflict);
    EXPECT_EQ(conflict.error().code, dt::error_code_e::request_conflict);
  }

  TEST(DisplayTopologyRequests, AppliedCannotSubstituteSourcesAdaptersOrUnknownStates) {
    const auto fixture = mixed();
    const auto requested = request(fixture);
    for (int field = 0; field < 6; ++field) {
      SCOPED_TRACE(field);
      auto actual = applied(fixture);
      if (field == 0) actual.monitors[0].source->display = dt::os_display_id_t { "wrong-monitor" };
      if (field == 1) actual.monitors[1].source->head->identity.guid = id<dt::head_guid_t>(99);
      if (field == 2) actual.monitors[1].source->adapter = dt::adapter_luid_t { 8 };
      if (field == 3) actual.monitors[0].state = static_cast<dt::applied_state_e>(99);
      if (field == 4) actual.monitors[0].layout->mode.resolution.width = 0;
      if (field == 5) actual.monitors[0].layout->mode.refresh_rate.denominator = 0;
      const auto result = dt::validate_applied(requested, actual);
      ASSERT_FALSE(result) << "Unrelated or invalid readback must not become applied success";
      EXPECT_EQ(result.error().code, field == 2 ? dt::error_code_e::adapter_mismatch :
                                     field == 3 ? dt::error_code_e::invalid_profile :
                                     field == 4 ? dt::error_code_e::invalid_geometry :
                                     field == 5 ? dt::error_code_e::invalid_refresh :
                                                  dt::error_code_e::source_kind_mismatch);
    }
  }

  TEST(DisplayTopologyLegacy, GoldenOneWayMappingsPreserveScalarIntent) {
    struct case_t {
      std::string selector;
      bool vdd;
      display_device::parsed_config_t::device_prep_e prep;
      display_device::parsed_config_t::vdd_prep_e vdd_prep;
      bool keep;
      bool headless;
      bool reuse;
    };
    using parsed = display_device::parsed_config_t;
    const std::vector<case_t> cases {
      { "", false, parsed::device_prep_e::no_operation, parsed::vdd_prep_e::no_operation, false, false, true },
      { "unknown-stale-device", false, parsed::device_prep_e::ensure_only_display, parsed::vdd_prep_e::no_operation, false, false, false },
      { "os-A", false, parsed::device_prep_e::ensure_secondary, parsed::vdd_prep_e::no_operation, false, true, true },
      { "ZakoHDR", true, parsed::device_prep_e::ensure_primary, parsed::vdd_prep_e::vdd_as_primary, true, false, true },
      { "per-client-virtual-request", true, parsed::device_prep_e::ensure_only_display, parsed::vdd_prep_e::display_off, false, true, false }
    };
    for (const auto &test : cases) {
      SCOPED_TRACE(test.selector);
      dt::legacy_intent_t settings;
      settings.output_name = test.selector;
      settings.parsed.device_id = test.selector;
      settings.parsed.device_prep = test.prep;
      settings.parsed.vdd_prep = test.vdd_prep;
      settings.parsed.use_vdd = test.vdd;
      settings.parsed.resolution = display_device::resolution_t { 2560, 1440 };
      settings.parsed.refresh_rate = display_device::refresh_rate_t { 60000, 1001 };
      settings.parsed.change_hdr_state = true;
      settings.keep_enabled = test.keep;
      settings.headless_create = test.headless;
      settings.reuse = test.reuse;
      const auto original = settings;
      const auto result = dt::synthesize_legacy(settings, id<dt::profile_id_t>(1), id<dt::monitor_id_t>(2),
        id<dt::head_guid_t>(3), id<dt::owner_id_t>(4), dt::adapter_luid_t { 7 }, limits());
      ASSERT_TRUE(result);
      dt::profile_t expected;
      expected.id = id<dt::profile_id_t>(1);
      expected.name = "Legacy display";
      expected.limits = limits();
      expected.order = { id<dt::monitor_id_t>(2) };
      expected.preferred = expected.order[0];
      expected.fallback = expected.order[0];
      expected.legacy = settings;
      dt::monitor_entry_t entry { .id = expected.order[0] };
      if (test.vdd)
        entry.source = dt::virtual_source_t { id<dt::head_guid_t>(3), id<dt::owner_id_t>(4), dt::adapter_luid_t { 7 } };
      else if (test.selector.empty())
        entry.source = dt::legacy_primary_t {};
      else
        entry.source = dt::physical_source_t { dt::os_display_id_t { test.selector }, {} };
      expected.entries = { entry };
      EXPECT_EQ(*result, expected);
      EXPECT_EQ(settings, original);
    }
  }

  TEST(DisplayTopologyLegacy, EmptyAndUnknownSelectorsDoNotGuessAnObservedMonitor) {
    auto fixture = physical();
    dt::legacy_intent_t settings;
    for (const auto *selector : { "", "unknown" }) {
      settings.output_name = selector;
      const auto profile = dt::synthesize_legacy(settings, id<dt::profile_id_t>(1), id<dt::monitor_id_t>(2), {}, {}, {}, limits());
      ASSERT_TRUE(profile);
      const auto result = dt::resolve_profile(*profile, fixture.inventory)[0];
      EXPECT_FALSE(result.binding);
      ASSERT_TRUE(result.error);
      EXPECT_EQ(result.error->code, dt::error_code_e::missing_identity);
    }
  }
}  // namespace
