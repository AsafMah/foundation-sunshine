#include <gtest/gtest.h>

#include "display_topology_fixtures.h"
#include "src/display_device/topology_lifecycle.h"

namespace {
  using namespace display_topology_test;
  using namespace std::chrono_literals;

  dt::p1_reservation_ledger_t::request_t
  reservation(std::uint8_t identity = 1) {
    return { id<dt::reservation_id_t>(identity), id<dt::owner_id_t>(50), id<dt::transaction_id_t>(identity),
      dt::topology_revision_t { 0 }, { { dt::owner_resource, 1 } }, 100ms };
  }

  dt::p1_reservation_ledger_t
  ledger(std::size_t history_limit = 16) {
    return { { { dt::owner_resource, 1 }, { dt::ticket_resource, 2 } }, history_limit, 1000ms };
  }

  TEST(DisplayTopologyReservations, ConcurrentPendingRequestsCannotDoubleSpend) {
    auto pool = ledger();
    auto first = reservation();
    first.amounts = { { dt::ticket_resource, 1 } };
    auto second = first;
    second.id = id<dt::reservation_id_t>(2);
    auto third = second;
    third.id = id<dt::reservation_id_t>(3);
    const auto revision = pool.revision();
    ASSERT_TRUE(pool.reserve(first, revision, 0ms));
    const auto stale = pool.reserve(second, revision, 0ms);
    ASSERT_FALSE(stale);
    EXPECT_EQ(stale.error().code, dt::error_code_e::stale_revision);
    ASSERT_TRUE(pool.reserve(second, pool.revision(), 0ms));
    const auto full = pool.reserve(third, pool.revision(), 0ms);
    ASSERT_FALSE(full);
    EXPECT_EQ(full.error().code, dt::error_code_e::resource_unavailable);
    EXPECT_EQ(pool.held(dt::ticket_resource), 2u);
  }

  TEST(DisplayTopologyReservations, WholeDemandRefusalDoesNotSpendEarlierResources) {
    auto pool = ledger();
    auto request = reservation();
    request.amounts[dt::ticket_resource] = 3;
    const auto before = pool;
    const auto refusal = pool.reserve(request, pool.revision(), 0ms);
    ASSERT_FALSE(refusal);
    EXPECT_EQ(refusal.error().code, dt::error_code_e::resource_unavailable);
    EXPECT_EQ(pool, before);
    request.amounts[dt::ticket_resource] = 2;
    EXPECT_TRUE(pool.reserve(request, pool.revision(), 0ms));
  }

  TEST(DisplayTopologyReservations, CommitRollbackAndReleaseAreExactlyOnce) {
    auto pool = ledger();
    auto first = pool.reserve(reservation(), pool.revision(), 0ms);
    ASSERT_TRUE(first);
    ASSERT_TRUE(pool.commit(first->token, 1ms));
    const auto committed = pool;
    EXPECT_TRUE(error_is(pool.commit(first->token, 2ms), dt::error_code_e::already_finalized));
    EXPECT_TRUE(error_is(pool.rollback(first->token, 2ms), dt::error_code_e::already_finalized));
    EXPECT_EQ(pool, committed);
    ASSERT_TRUE(pool.release(first->token, 2ms));
    EXPECT_EQ(pool.held(dt::owner_resource), 0u);
    EXPECT_TRUE(error_is(pool.release(first->token, 3ms), dt::error_code_e::already_finalized));
    auto second = pool.reserve(reservation(2), pool.revision(), 3ms);
    ASSERT_TRUE(second);
    ASSERT_TRUE(pool.rollback(second->token, 4ms));
    const auto rolled_back = pool;
    EXPECT_TRUE(error_is(pool.rollback(second->token, 5ms), dt::error_code_e::already_finalized));
    EXPECT_TRUE(error_is(pool.commit(second->token, 5ms), dt::error_code_e::already_finalized));
    EXPECT_EQ(pool, rolled_back);
  }

  TEST(DisplayTopologyReservations, ExpiryAndLateCommitCannotResurrectOrReleaseNewHolds) {
    auto pool = ledger();
    const auto first = pool.reserve(reservation(), pool.revision(), 0ms);
    ASSERT_TRUE(first);
    EXPECT_TRUE(error_is(pool.commit(first->token, 100ms), dt::error_code_e::expired));
    const auto expired = pool.expire(100ms);
    ASSERT_TRUE(expired);
    ASSERT_EQ(expired->size(), 1u);
    EXPECT_EQ((*expired)[0], first->token);
    EXPECT_EQ(pool.held(dt::owner_resource), 0u);
    auto next = reservation(2);
    next.expires = 200ms;
    const auto second = pool.reserve(next, pool.revision(), 100ms);
    ASSERT_TRUE(second);
    const auto before = pool;
    EXPECT_TRUE(error_is(pool.commit(first->token, 101ms), dt::error_code_e::already_finalized));
    EXPECT_TRUE(error_is(pool.rollback(first->token, 101ms), dt::error_code_e::already_finalized));
    EXPECT_EQ(pool, before);
    EXPECT_EQ(pool.held(dt::owner_resource), 1u);
  }

  TEST(DisplayTopologyReservations, CommittedMutationFencesNeverExpire) {
    auto pool = ledger();
    const auto first = pool.reserve(reservation(), pool.revision(), 0ms);
    ASSERT_TRUE(first);
    ASSERT_TRUE(pool.commit(first->token, 1ms));
    ASSERT_TRUE(pool.expire(1000ms));
    EXPECT_EQ(pool.held(dt::owner_resource), 1u);
    auto next = reservation(2);
    next.expires = 1100ms;
    const auto refusal = pool.reserve(next, pool.revision(), 1000ms);
    ASSERT_FALSE(refusal);
    EXPECT_EQ(refusal.error().code, dt::error_code_e::resource_unavailable);
  }

  TEST(DisplayTopologyReservations, ExactReplaySurvivesExpiryAndFullHistoryWithoutRenewal) {
    auto pool = ledger(1);
    const auto request = reservation();
    const auto first = pool.reserve(request, pool.revision(), 0ms);
    ASSERT_TRUE(first);
    ASSERT_TRUE(pool.expire(100ms));
    const auto replay = pool.reserve(request, {}, 100ms);
    ASSERT_TRUE(replay);
    EXPECT_EQ(replay->token, first->token);
    EXPECT_EQ(replay->request.expires, 100ms);
    EXPECT_EQ(replay->state, dt::reservation_state_e::expired);
    auto changed = request;
    changed.expires = 200ms;
    EXPECT_TRUE(error_is(pool.reserve(changed, pool.revision(), 100ms), dt::error_code_e::request_conflict));
    changed.id = id<dt::reservation_id_t>(2);
    EXPECT_TRUE(error_is(pool.reserve(changed, pool.revision(), 100ms), dt::error_code_e::history_full));
  }

  TEST(DisplayTopologyReservations, TokensNotUnrelatedLedgerRevisionsIdentifyCompletions) {
    auto pool = ledger();
    const auto first = pool.reserve(reservation(), pool.revision(), 0ms);
    ASSERT_TRUE(first);
    auto second = reservation(2);
    second.amounts = { { dt::ticket_resource, 1 } };
    ASSERT_TRUE(pool.reserve(second, pool.revision(), 1ms));
    ASSERT_TRUE(pool.commit(first->token, 2ms));
    auto forged = first->token;
    forged.transaction = id<dt::transaction_id_t>(99);
    const auto before = pool;
    EXPECT_TRUE(error_is(pool.release(forged, 3ms), dt::error_code_e::stale_token));
    EXPECT_EQ(pool, before);
  }

  TEST(DisplayTopologyReservations, InvalidDeadlinesAndBackwardsClockDoNotMutate) {
    auto pool = ledger();
    auto request = reservation();
    request.expires = 0ms;
    const auto before = pool;
    EXPECT_TRUE(error_is(pool.reserve(request, pool.revision(), 0ms), dt::error_code_e::invalid_deadline));
    EXPECT_EQ(pool, before);
    request.expires = 1001ms;
    EXPECT_TRUE(error_is(pool.reserve(request, pool.revision(), 0ms), dt::error_code_e::invalid_deadline));
    request.expires = 100ms;
    const auto held = pool.reserve(request, pool.revision(), 10ms);
    ASSERT_TRUE(held);
    const auto after = pool;
    EXPECT_TRUE(error_is(pool.commit(held->token, 9ms), dt::error_code_e::clock_reversed));
    EXPECT_EQ(pool, after);
  }

  TEST(DisplayTopologyReservations, UnknownOccupiedHeadsConsumeSlotsAndCannotBeReused) {
    auto fixture = mixed();
    fixture.inventory.heads.push_back({ { id<dt::driver_instance_id_t>(60), id<dt::head_guid_t>(99), dt::head_incarnation_t { 9 } },
      {}, dt::adapter_luid_t { 7 }, 2, {} });
    const auto capacities = dt::p1_capacities(fixture.inventory, 2, 3);
    ASSERT_TRUE(capacities);
    dt::p1_reservation_ledger_t pool(*capacities, 8, 1000ms);
    auto request = reservation();
    const dt::resource_key_t unknown_slot { dt::resource_kind_e::vdd_slot, id<dt::driver_instance_id_t>(60), dt::adapter_luid_t { 7 }, 2 };
    request.amounts = { { unknown_slot, 1 } };
    const auto refusal = pool.reserve(request, pool.revision(), 0ms);
    ASSERT_FALSE(refusal);
    EXPECT_EQ(refusal.error().code, dt::error_code_e::resource_unavailable);
    const auto &unknown = fixture.inventory.heads.back();
    EXPECT_TRUE(error_is(dt::reuse_owned_head(fixture.inventory, { unknown.identity, id<dt::owner_id_t>(50), unknown.adapter, unknown.slot }), dt::error_code_e::not_owned));
  }

  TEST(DisplayTopologyReservations, OwnedReuseDoesNotSpendOrFreeAnOccupiedSlot) {
    const auto fixture = mixed();
    const auto &head = fixture.inventory.heads[0];
    const dt::owned_head_t token { head.identity, *head.owner, head.adapter, head.slot };
    const auto original = dt::p1_capacities(fixture.inventory, 2, 3);
    ASSERT_TRUE(original);
    const auto reused = dt::reuse_owned_head(fixture.inventory, token);
    ASSERT_TRUE(reused);
    EXPECT_EQ(*reused, token);
    const auto after_reuse = dt::p1_capacities(fixture.inventory, 2, 3);
    ASSERT_TRUE(after_reuse);
    EXPECT_EQ(*after_reuse, *original);
    dt::p1_reservation_ledger_t pool(*original, 8, 1000ms);
    auto request = reservation();
    const dt::resource_key_t free_slot { dt::resource_kind_e::vdd_slot, head.identity.driver, head.adapter, 2 };
    request.amounts[free_slot] = 1;
    const auto held = pool.reserve(request, pool.revision(), 0ms);
    ASSERT_TRUE(held);
    ASSERT_TRUE(pool.rollback(held->token, 1ms));
    const dt::resource_key_t occupied_slot { dt::resource_kind_e::vdd_slot, head.identity.driver, head.adapter, head.slot };
    request.id = id<dt::reservation_id_t>(2);
    request.amounts = { { occupied_slot, 1 } };
    EXPECT_TRUE(error_is(pool.reserve(request, pool.revision(), 2ms), dt::error_code_e::resource_unavailable));
    auto restarted = fixture.inventory;
    restarted.heads[0].identity.driver = id<dt::driver_instance_id_t>(61);
    EXPECT_TRUE(error_is(dt::reuse_owned_head(restarted, token), dt::error_code_e::not_owned));
  }

  TEST(DisplayTopologyReservations, CapacitySnapshotRejectsAmbiguityAndMissingObservation) {
    auto fixture = mixed();
    fixture.inventory.heads.push_back(fixture.inventory.heads[0]);
    EXPECT_TRUE(error_is(dt::p1_capacities(fixture.inventory, 2, 3), dt::error_code_e::ambiguous_identity));
    fixture = mixed();
    fixture.inventory.heads[0].slot = 99;
    EXPECT_TRUE(error_is(dt::p1_capacities(fixture.inventory, 2, 3), dt::error_code_e::unknown_resource));
    fixture = mixed();
    EXPECT_TRUE(error_is(dt::p1_capacities(fixture.inventory, 2, 2), dt::error_code_e::limit_exceeded));
    fixture.inventory.available = false;
    EXPECT_TRUE(error_is(dt::p1_capacities(fixture.inventory, 2, 3), dt::error_code_e::inventory_unavailable));
  }

  class DisplayTopologyLease: public ::testing::Test {
  protected:
    fake_clock_t clock;
    dt::p1_reservation_ledger_t pool = ledger();
    dt::lease_state_t state;
    dt::reservation_token_t hold;

    void
    SetUp() override {
      const auto reserved = pool.reserve(reservation(), pool.revision(), clock.now);
      ASSERT_TRUE(reserved);
      hold = reserved->token;
      dt::lease_event_t start { .kind = dt::lease_event_e::reserve,
        .actor = hold.owner,
        .token = { hold.owner, hold.transaction, {}, 0 },
        .authorized_owner = true,
        .protected_hold = hold,
        .ticket_expires = 100ms,
        .reconnect_grace = 20ms };
      const auto result = dt::transition(state, start, clock.now, pool);
      ASSERT_TRUE(result) << (result ? "" : result.error().detail);
      state = result->state;
    }

    dt::lease_event_t
    event(dt::lease_event_e kind) const {
      return { .kind = kind, .actor = hold.owner, .token = state.lease.value_or(dt::lease_token_t {}), .authorized_owner = true, .authorized_input = true };
    }

    void
    apply(dt::lease_event_e kind) {
      const auto next = dt::transition(state, event(kind), clock.now, pool);
      ASSERT_TRUE(next) << (next ? "" : next.error().detail);
      state = next->state;
    }

    void
    fence() {
      ASSERT_TRUE(pool.commit(hold, clock.now));
      apply(dt::lease_event_e::authorize_mutation);
    }

    void
    active() {
      apply(dt::lease_event_e::prepare);
      fence();
      apply(dt::lease_event_e::apply);
      apply(dt::lease_event_e::settle);
      apply(dt::lease_event_e::activate);
    }
  };

  TEST_F(DisplayTopologyLease, OrderedTransitionsAndInvalidEdgesAreTableDriven) {
    const std::vector<std::pair<dt::lease_event_e, dt::phase_e>> path {
      { dt::lease_event_e::prepare, dt::phase_e::preparing },
      { dt::lease_event_e::apply, dt::phase_e::applying },
      { dt::lease_event_e::settle, dt::phase_e::settling },
      { dt::lease_event_e::activate, dt::phase_e::active },
      { dt::lease_event_e::drain, dt::phase_e::draining },
      { dt::lease_event_e::restore, dt::phase_e::restoring },
      { dt::lease_event_e::restored, dt::phase_e::idle }
    };
    for (const auto &[action, expected] : path) {
      SCOPED_TRACE(static_cast<int>(action));
      const auto before = state;
      const auto invalid = dt::transition(state, event(dt::lease_event_e::reconnect), clock.now, pool);
      ASSERT_FALSE(invalid);
      EXPECT_EQ(invalid.error().code, dt::error_code_e::invalid_transition);
      EXPECT_EQ(state, before);
      if (action == dt::lease_event_e::apply) fence();
      const auto next = dt::transition(state, event(action), clock.now, pool);
      ASSERT_TRUE(next);
      state = next->state;
      EXPECT_EQ(state.phase, expected);
      if (action == dt::lease_event_e::restored) {
        EXPECT_TRUE(next->release_protected_hold);
        EXPECT_FALSE(state.lease);
        ASSERT_TRUE(pool.release(hold, clock.now));
      }
    }
    EXPECT_EQ(pool.held(dt::owner_resource), 0u);
  }

  TEST_F(DisplayTopologyLease, OneOwnerAcrossPendingActiveAndReconnectGrace) {
    for (int phase = 0; phase < 3; ++phase) {
      SCOPED_TRACE(phase);
      if (phase == 1) active();
      if (phase == 2) apply(dt::lease_event_e::disconnect);
      auto competing = event(dt::lease_event_e::reserve);
      competing.actor = id<dt::owner_id_t>(51);
      competing.token.owner = competing.actor;
      competing.token.transaction = id<dt::transaction_id_t>(2);
      const auto before = state;
      const auto result = dt::transition(state, competing, clock.now, pool);
      ASSERT_FALSE(result);
      EXPECT_EQ(result.error().code, dt::error_code_e::owner_busy);
      EXPECT_EQ(state, before);
    }
  }

  TEST_F(DisplayTopologyLease, NoMutationMayStartWithoutCommittedResourceFence) {
    apply(dt::lease_event_e::prepare);
    const auto before = state;
    EXPECT_TRUE(error_is(dt::transition(state, event(dt::lease_event_e::authorize_mutation), clock.now, pool), dt::error_code_e::fence_required));
    EXPECT_TRUE(error_is(dt::transition(state, event(dt::lease_event_e::apply), clock.now, pool), dt::error_code_e::fence_required));
    EXPECT_EQ(state, before);
    fence();
    EXPECT_TRUE(state.effects_possible);
  }

  TEST_F(DisplayTopologyLease, PendingExpiryBeforeMutationReleasesWithoutResurrection) {
    const auto old_event = event(dt::lease_event_e::prepare);
    clock.advance(100ms);
    EXPECT_TRUE(error_is(dt::transition(state, old_event, clock.now, pool), dt::error_code_e::expired));
    const auto expired = dt::transition(state, event(dt::lease_event_e::tick), clock.now, pool);
    ASSERT_TRUE(expired);
    EXPECT_EQ(expired->state.phase, dt::phase_e::aborted);
    EXPECT_TRUE(expired->release_protected_hold);
    EXPECT_FALSE(expired->state.lease);
    ASSERT_TRUE(pool.expire(clock.now));
    EXPECT_EQ(pool.held(dt::owner_resource), 0u);
    const auto late = dt::transition(expired->state, old_event, clock.now, pool);
    ASSERT_TRUE(late);
    EXPECT_EQ(late->disposition, dt::transition_disposition_e::ignored_stale);
    EXPECT_EQ(late->state, expired->state);
  }

  TEST_F(DisplayTopologyLease, PreparationExpiryAfterPossibleVddCreateRetainsRecoveryFence) {
    apply(dt::lease_event_e::prepare);
    fence();
    clock.advance(100ms);
    const auto expired = dt::transition(state, event(dt::lease_event_e::tick), clock.now, pool);
    ASSERT_TRUE(expired);
    state = expired->state;
    EXPECT_EQ(state.phase, dt::phase_e::recovery_required);
    EXPECT_TRUE(state.lease);
    EXPECT_FALSE(expired->release_protected_hold);
    ASSERT_TRUE(pool.expire(clock.now));
    EXPECT_EQ(pool.held(dt::owner_resource), 1u);
    const auto late = dt::transition(state, event(dt::lease_event_e::observe_late_completion), clock.now, pool);
    ASSERT_TRUE(late);
    EXPECT_TRUE(late->state.late_observation);
    EXPECT_EQ(late->state.phase, dt::phase_e::recovery_required);
    EXPECT_FALSE(late->state.input_controller);
    auto second = reservation(2);
    second.expires = 200ms;
    EXPECT_TRUE(error_is(pool.reserve(second, pool.revision(), clock.now), dt::error_code_e::resource_unavailable));
    apply(dt::lease_event_e::restore);
    const auto restored = dt::transition(state, event(dt::lease_event_e::restored), clock.now, pool);
    ASSERT_TRUE(restored);
    EXPECT_TRUE(restored->release_protected_hold);
    ASSERT_TRUE(pool.release(hold, clock.now));
    EXPECT_TRUE(pool.reserve(second, pool.revision(), clock.now));
  }

  TEST_F(DisplayTopologyLease, CancelledLeaseCompletionCannotReleaseANewerOwner) {
    const auto stale = event(dt::lease_event_e::restored);
    const auto cancelled = dt::transition(state, event(dt::lease_event_e::cancel), clock.now, pool);
    ASSERT_TRUE(cancelled);
    EXPECT_TRUE(cancelled->release_protected_hold);
    state = cancelled->state;
    ASSERT_TRUE(pool.rollback(hold, clock.now));
    auto next = reservation(2);
    next.owner = id<dt::owner_id_t>(51);
    const auto reserved = pool.reserve(next, pool.revision(), clock.now);
    ASSERT_TRUE(reserved);
    dt::lease_event_t start { .kind = dt::lease_event_e::reserve, .actor = next.owner, .token = { next.owner, next.transaction, {}, 0 }, .authorized_owner = true, .protected_hold = reserved->token, .ticket_expires = 100ms, .reconnect_grace = 20ms };
    const auto newer = dt::transition(state, start, clock.now, pool);
    ASSERT_TRUE(newer);
    EXPECT_GT(newer->state.generation, stale.token.generation);
    const auto late = dt::transition(newer->state, stale, clock.now, pool);
    ASSERT_TRUE(late);
    EXPECT_EQ(late->disposition, dt::transition_disposition_e::ignored_stale);
    EXPECT_EQ(late->state, newer->state);
    EXPECT_FALSE(late->release_protected_hold);
    EXPECT_EQ(pool.held(dt::owner_resource), 1u);
  }

  TEST_F(DisplayTopologyLease, StaleTransactionRevisionAndGenerationAreIgnored) {
    for (int field = 0; field < 3; ++field) {
      auto stale = event(dt::lease_event_e::cancel);
      if (field == 0) stale.token.transaction = id<dt::transaction_id_t>(99);
      if (field == 1) ++stale.token.base.value;
      if (field == 2) ++stale.token.generation;
      const auto result = dt::transition(state, stale, clock.now, pool);
      ASSERT_TRUE(result);
      EXPECT_EQ(result->disposition, dt::transition_disposition_e::ignored_stale);
      EXPECT_EQ(result->state, state);
      EXPECT_FALSE(result->release_protected_hold);
    }
  }

  TEST_F(DisplayTopologyLease, GraceIsFiniteAndCannotBeRenewedByRepeatedDisconnect) {
    active();
    apply(dt::lease_event_e::disconnect);
    EXPECT_FALSE(state.input_controller);
    ASSERT_TRUE(state.grace_expires);
    EXPECT_EQ(*state.grace_expires, 20ms);
    clock.advance(5ms);
    const auto duplicate = dt::transition(state, event(dt::lease_event_e::disconnect), clock.now, pool);
    ASSERT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error().code, dt::error_code_e::invalid_transition);
    clock.advance(15ms);
    EXPECT_TRUE(error_is(dt::transition(state, event(dt::lease_event_e::reconnect), clock.now, pool), dt::error_code_e::expired));
    apply(dt::lease_event_e::tick);
    EXPECT_EQ(state.phase, dt::phase_e::draining);
    EXPECT_TRUE(state.lease);
    EXPECT_FALSE(state.input_controller);
  }

  TEST_F(DisplayTopologyLease, ReconnectRevalidatesAuthorityWithoutRevivingRevokedInput) {
    active();
    const auto applied_revision = state.applied_revision;
    apply(dt::lease_event_e::disconnect);
    auto reconnect = event(dt::lease_event_e::reconnect);
    reconnect.authorized_owner = false;
    EXPECT_TRUE(error_is(dt::transition(state, reconnect, clock.now, pool), dt::error_code_e::permission_denied));
    reconnect.authorized_owner = true;
    reconnect.authorized_input = false;
    const auto result = dt::transition(state, reconnect, clock.now, pool);
    ASSERT_TRUE(result);
    EXPECT_FALSE(result->state.input_controller);
    EXPECT_FALSE(result->state.grace_expires);
    EXPECT_EQ(result->state.applied_revision, applied_revision);
    EXPECT_EQ(result->state.lease, state.lease);
  }

  TEST_F(DisplayTopologyLease, ObserversNeverPromoteAndInputHasOneExplicitController) {
    active();
    auto grant = event(dt::lease_event_e::grant_observer);
    const auto observer = id<dt::owner_id_t>(51);
    grant.grant = dt::observer_grant_t { observer, state.generation, true, true, 50ms };
    auto result = dt::transition(state, grant, clock.now, pool);
    ASSERT_TRUE(result);
    state = result->state;
    EXPECT_EQ(state.input_controller, observer);
    auto mutate = event(dt::lease_event_e::drain);
    mutate.actor = observer;
    EXPECT_TRUE(error_is(dt::transition(state, mutate, clock.now, pool), dt::error_code_e::permission_denied));
    auto revoke = event(dt::lease_event_e::revoke_observer);
    revoke.grant = grant.grant;
    result = dt::transition(state, revoke, clock.now, pool);
    ASSERT_TRUE(result);
    state = result->state;
    EXPECT_FALSE(state.input_controller);
    EXPECT_TRUE(state.observers.empty());
    apply(dt::lease_event_e::disconnect);
    clock.advance(20ms);
    apply(dt::lease_event_e::tick);
    ASSERT_TRUE(state.lease);
    EXPECT_EQ(state.lease->owner, hold.owner);
    EXPECT_EQ(state.phase, dt::phase_e::draining);
  }

  TEST_F(DisplayTopologyLease, ObserverGrantExpiryAndGenerationAreEnforced) {
    active();
    auto grant = event(dt::lease_event_e::grant_observer);
    grant.grant = dt::observer_grant_t { id<dt::owner_id_t>(51), state.generation + 1, true, true, 10ms };
    EXPECT_TRUE(error_is(dt::transition(state, grant, clock.now, pool), dt::error_code_e::permission_denied));
    grant.grant->owner_generation = state.generation;
    const auto result = dt::transition(state, grant, clock.now, pool);
    ASSERT_TRUE(result);
    state = result->state;
    clock.advance(10ms);
    apply(dt::lease_event_e::tick);
    EXPECT_TRUE(state.observers.empty());
    EXPECT_FALSE(state.input_controller);
    EXPECT_EQ(state.phase, dt::phase_e::active);
  }

  TEST_F(DisplayTopologyLease, DegradationRequiresPriorPermission) {
    apply(dt::lease_event_e::prepare);
    fence();
    apply(dt::lease_event_e::apply);
    apply(dt::lease_event_e::settle);
    EXPECT_TRUE(error_is(dt::transition(state, event(dt::lease_event_e::activate_degraded), clock.now, pool), dt::error_code_e::permission_denied));
    state.allow_degraded = true;
    apply(dt::lease_event_e::activate_degraded);
    EXPECT_EQ(state.phase, dt::phase_e::degraded);
  }

  TEST_F(DisplayTopologyLease, ReleasedFenceCannotPublishLateSuccess) {
    apply(dt::lease_event_e::prepare);
    fence();
    apply(dt::lease_event_e::apply);
    apply(dt::lease_event_e::settle);
    ASSERT_TRUE(pool.release(hold, clock.now));
    const auto before = state;
    const auto late = dt::transition(state, event(dt::lease_event_e::activate), clock.now, pool);
    ASSERT_FALSE(late);
    EXPECT_EQ(late.error().code, dt::error_code_e::fence_required);
    EXPECT_EQ(state, before);
  }
}  // namespace
