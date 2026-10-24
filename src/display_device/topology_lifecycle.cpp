#include "topology_lifecycle.h"

#include <algorithm>
#include <limits>
#include <set>

namespace display_device::topology {
  namespace {
    std::unexpected<error_t>
    fail(error_code_e code, std::string detail) {
      return std::unexpected(error_t { code, std::move(detail), {} });
    }

    bool
    serving(phase_e phase) {
      return phase == phase_e::active || phase == phase_e::degraded;
    }

    bool
    pending(phase_e phase) {
      return phase == phase_e::reserved || phase == phase_e::preparing ||
             phase == phase_e::applying || phase == phase_e::settling;
    }
  }  // namespace

  result_t<p1_reservation_ledger_t::capacities_t>
  p1_capacities(const inventory_t &inventory, std::uint32_t tickets, std::uint32_t max_slots) {
    if (!inventory.available) return fail(error_code_e::inventory_unavailable, "No capacity can be inferred from a failed inventory");
    p1_reservation_ledger_t::capacities_t capacities { { owner_resource, 1 }, { ticket_resource, tickets } };
    std::set<std::pair<driver_instance_id_t, adapter_luid_t>> drivers;
    std::uint64_t total_slots {};
    for (const auto &driver : inventory.drivers) {
      if (!valid_uuid(driver.driver) || !drivers.emplace(driver.driver, driver.adapter).second) {
        return fail(error_code_e::ambiguous_identity, "Invalid or duplicate driver capacity identity");
      }
      total_slots += driver.slots;
      if (total_slots > max_slots) return fail(error_code_e::limit_exceeded, "Inventory exceeds the caller's capacity-record bound");
      for (std::uint32_t slot = 0; slot < driver.slots; ++slot) {
        capacities.emplace(resource_key_t { resource_kind_e::vdd_slot, driver.driver, driver.adapter, slot }, 1);
      }
    }
    std::set<resource_key_t> occupied;
    for (const auto &head : inventory.heads) {
      const resource_key_t key { resource_kind_e::vdd_slot, head.identity.driver, head.adapter, head.slot };
      const auto capacity = capacities.find(key);
      if (capacity == capacities.end()) return fail(error_code_e::unknown_resource, "Occupied head has no corresponding fixed capacity slot");
      if (!occupied.insert(key).second) return fail(error_code_e::ambiguous_identity, "Several head records occupy the same slot");
      capacity->second = 0;
    }
    return capacities;
  }

  result_t<owned_head_t>
  reuse_owned_head(const inventory_t &inventory, const owned_head_t &expected) {
    if (!inventory.available) return fail(error_code_e::inventory_unavailable, "Head reuse requires a successful inventory");
    const auto count = std::count_if(inventory.heads.begin(), inventory.heads.end(), [&](const auto &head) {
      return head.identity == expected.identity && head.owner == expected.owner &&
             head.adapter == expected.adapter && head.slot == expected.slot;
    });
    if (count != 1) return fail(error_code_e::not_owned, "Exact current head ownership is not proven");
    return expected;
  }

  result_t<lease_transition_t>
  transition(const lease_state_t &state, const lease_event_t &event, monotonic_time_t now, const p1_reservation_ledger_t &ledger) {
    if (event.kind != lease_event_e::reserve && event.kind != lease_event_e::tick &&
        (!state.lease || event.token != *state.lease)) {
      return lease_transition_t { state, transition_disposition_e::ignored_stale };
    }
    if (now < state.last_event) return fail(error_code_e::clock_reversed, "Monotonic event time moved backwards");
    lease_transition_t next { state };
    auto &value = next.state;
    const auto relinquish = [&](phase_e phase) {
      value.phase = phase;
      value.lease.reset();
      value.input_controller.reset();
      value.observers.clear();
      value.grace_expires.reset();
      value.effects_possible = false;
      next.release_protected_hold = value.protected_hold.has_value();
      value.protected_hold.reset();
    };
    const auto interrupt = [&]() {
      value.input_controller.reset();
      value.grace_expires.reset();
      for (auto &[subject, grant] : value.observers) grant.input = false;
      if (value.effects_possible) {
        value.phase = phase_e::recovery_required;
      }
      else {
        relinquish(phase_e::aborted);
      }
    };
    if (event.kind == lease_event_e::tick) {
      for (auto it = value.observers.begin(); it != value.observers.end();) {
        if (now >= it->second.expires) {
          if (value.input_controller == it->first) value.input_controller.reset();
          it = value.observers.erase(it);
        }
        else {
          ++it;
        }
      }
      if (pending(value.phase) && now >= value.ticket_expires) interrupt();
      if (serving(value.phase) && value.grace_expires && now >= *value.grace_expires) {
        value.phase = phase_e::draining;
        value.input_controller.reset();
        value.observers.clear();
        value.grace_expires.reset();
      }
      value.last_event = now;
      return next;
    }
    if (event.kind == lease_event_e::reserve) {
      if (state.lease) return fail(error_code_e::owner_busy, "Pending, active or recovering owner already holds the desktop");
      if (!event.authorized_owner || !valid_uuid(event.actor) || !valid_uuid(event.token.transaction) || event.token.owner != event.actor) {
        return fail(error_code_e::permission_denied, "Only an explicitly authorized owner may reserve topology");
      }
      if (event.token.base != state.applied_revision) return fail(error_code_e::stale_revision, "Lease base revision changed");
      if (event.ticket_expires <= now || event.reconnect_grace.count() < 0 ||
          event.reconnect_grace.count() > std::numeric_limits<std::int64_t>::max() - now.count()) {
        return fail(error_code_e::invalid_deadline, "Lease needs finite ticket and reconnect deadlines");
      }
      if (state.generation == std::numeric_limits<std::uint64_t>::max()) return fail(error_code_e::arithmetic_overflow, "Lease generation exhausted");
      if (!event.protected_hold) return fail(error_code_e::fence_required, "Owner reservation must precede launch preparation");
      const auto hold = ledger.inspect(*event.protected_hold);
      if (!hold || hold->state != reservation_state_e::pending || now >= hold->request.expires ||
          hold->token.owner != event.actor || hold->token.transaction != event.token.transaction ||
          hold->request.topology != event.token.base || !hold->request.amounts.contains(owner_resource) ||
          hold->request.amounts.at(owner_resource) != 1) {
        return fail(error_code_e::fence_required, "Valid pending owner hold is required");
      }
      value = lease_state_t {};
      value.applied_revision = state.applied_revision;
      value.generation = state.generation + 1;
      value.lease = lease_token_t { event.actor, event.token.transaction, event.token.base, value.generation };
      value.phase = phase_e::reserved;
      value.protected_hold = event.protected_hold;
      value.ticket_expires = std::min(event.ticket_expires, hold->request.expires);
      value.reconnect_grace = event.reconnect_grace;
      value.allow_degraded = event.allow_degraded;
      value.last_event = now;
      return next;
    }
    if (event.actor != state.lease->owner) return fail(error_code_e::permission_denied, "Observer is not the topology owner");
    if (pending(state.phase) && now >= state.ticket_expires &&
        event.kind != lease_event_e::cancel && event.kind != lease_event_e::failed) {
      return fail(error_code_e::expired, "Expired startup must abort or reconcile, not publish late success");
    }
    if (event.kind == lease_event_e::apply || event.kind == lease_event_e::settle ||
        event.kind == lease_event_e::activate || event.kind == lease_event_e::activate_degraded) {
      if (!state.protected_hold) return fail(error_code_e::fence_required, "Startup lost its owner reservation");
      const auto hold = ledger.inspect(*state.protected_hold);
      if (!hold || hold->state != reservation_state_e::committed) return fail(error_code_e::fence_required, "Released or expired holds cannot publish late success");
    }
    switch (event.kind) {
      case lease_event_e::prepare:
        if (state.phase != phase_e::reserved) return fail(error_code_e::invalid_transition, "Prepare requires a reserved lease");
        value.phase = phase_e::preparing;
        break;
      case lease_event_e::authorize_mutation: {
        if (state.phase != phase_e::preparing || state.effects_possible) return fail(error_code_e::invalid_transition, "Mutation authorization requires untouched preparation");
        if (!state.protected_hold) return fail(error_code_e::fence_required, "Mutation has no protected owner hold");
        const auto hold = ledger.inspect(*state.protected_hold);
        if (!hold || hold->state != reservation_state_e::committed) return fail(error_code_e::fence_required, "Commit owner and affected-slot holds before ANY possible side effect");
        value.effects_possible = true;
        break;
      }
      case lease_event_e::apply:
        if (state.phase != phase_e::preparing) return fail(error_code_e::invalid_transition, "Apply requires preparation");
        if (!state.effects_possible) return fail(error_code_e::fence_required, "Mutation must be fenced before apply");
        value.phase = phase_e::applying;
        break;
      case lease_event_e::settle:
        if (state.phase != phase_e::applying) return fail(error_code_e::invalid_transition, "Settle requires applying");
        value.phase = phase_e::settling;
        break;
      case lease_event_e::activate:
      case lease_event_e::activate_degraded:
        if (state.phase != phase_e::settling) return fail(error_code_e::invalid_transition, "Activation requires settled readback");
        if (event.kind == lease_event_e::activate_degraded && !state.allow_degraded) return fail(error_code_e::permission_denied, "Degradation was not authorized");
        if (state.applied_revision.value == std::numeric_limits<std::uint64_t>::max()) return fail(error_code_e::arithmetic_overflow, "Topology revision exhausted");
        ++value.applied_revision.value;
        value.phase = event.kind == lease_event_e::activate ? phase_e::active : phase_e::degraded;
        value.input_controller = event.authorized_input ? std::optional { event.actor } : std::nullopt;
        break;
      case lease_event_e::disconnect:
        if (!serving(state.phase) || state.grace_expires) return fail(error_code_e::invalid_transition, "Disconnect requires a connected active owner");
        if (state.reconnect_grace.count() > std::numeric_limits<std::int64_t>::max() - now.count()) return fail(error_code_e::invalid_deadline, "Reconnect deadline overflow");
        value.input_controller.reset();
        for (auto &[subject, grant] : value.observers) grant.input = false;
        value.grace_expires = now + state.reconnect_grace;
        break;
      case lease_event_e::reconnect:
        if (!serving(state.phase) || !state.grace_expires) return fail(error_code_e::invalid_transition, "Reconnect requires owner grace");
        if (now >= *state.grace_expires) return fail(error_code_e::expired, "Reconnect grace expired");
        if (!event.authorized_owner) return fail(error_code_e::permission_denied, "Reconnect authority must be revalidated");
        value.grace_expires.reset();
        value.input_controller = event.authorized_input ? std::optional { event.actor } : std::nullopt;
        break;
      case lease_event_e::grant_observer:
        if (!serving(state.phase) || state.grace_expires || !event.grant) return fail(error_code_e::invalid_transition, "Observer grants require a connected active owner");
        if (!event.grant->view || !valid_uuid(event.grant->subject) || event.grant->subject == event.actor ||
            event.grant->owner_generation != state.generation || event.grant->expires <= now) {
          return fail(error_code_e::permission_denied, "Observer grant must name a subject, current owner generation and future expiry");
        }
        value.observers[event.grant->subject] = *event.grant;
        if (value.input_controller == event.grant->subject) value.input_controller.reset();
        if (event.grant->input) {
          if (!event.authorized_input) return fail(error_code_e::permission_denied, "Input delegation is not authorized");
          for (auto &[subject, grant] : value.observers) grant.input = subject == event.grant->subject;
          value.input_controller = event.grant->subject;
        }
        break;
      case lease_event_e::revoke_observer:
        if (!event.grant || !value.observers.contains(event.grant->subject)) return fail(error_code_e::invalid_transition, "Unknown observer grant");
        value.observers.erase(event.grant->subject);
        if (value.input_controller == event.grant->subject) value.input_controller.reset();
        break;
      case lease_event_e::cancel:
      case lease_event_e::failed:
        if (!pending(state.phase) && !serving(state.phase)) return fail(error_code_e::invalid_transition, "Cancellation/failure is not valid in this phase");
        interrupt();
        break;
      case lease_event_e::drain:
        if (!serving(state.phase)) return fail(error_code_e::invalid_transition, "Drain requires an active owner");
        value.phase = phase_e::draining;
        value.input_controller.reset();
        value.observers.clear();
        value.grace_expires.reset();
        break;
      case lease_event_e::restore:
        if (state.phase != phase_e::draining && state.phase != phase_e::recovery_required) return fail(error_code_e::invalid_transition, "Restore requires drain or recovery");
        value.phase = phase_e::restoring;
        break;
      case lease_event_e::restored:
        if (state.phase != phase_e::restoring) return fail(error_code_e::invalid_transition, "Restoration acknowledgement requires restoring");
        relinquish(phase_e::idle);
        break;
      case lease_event_e::observe_late_completion:
        if (state.phase != phase_e::recovery_required && state.phase != phase_e::restoring) return fail(error_code_e::invalid_transition, "Late readback is only recovery evidence");
        value.late_observation = true;
        break;
      default:
        return fail(error_code_e::invalid_transition, "Unsupported lease event");
    }
    value.last_event = now;
    return next;
  }
}  // namespace display_device::topology
