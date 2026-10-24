#pragma once

#include "topology_reservations.h"

namespace display_device::topology {

  enum class phase_e { idle,
    reserved,
    preparing,
    applying,
    settling,
    active,
    degraded,
    draining,
    restoring,
    aborted,
    recovery_required };

  struct lease_token_t {
    owner_id_t owner;
    transaction_id_t transaction;
    topology_revision_t base;
    std::uint64_t generation {};
    bool
    operator==(const lease_token_t &) const = default;
  };

  struct observer_grant_t {
    owner_id_t subject;
    std::uint64_t owner_generation {};
    bool view {};
    bool input {};
    monotonic_time_t expires {};
    bool
    operator==(const observer_grant_t &) const = default;
  };

  struct lease_state_t {
    phase_e phase { phase_e::idle };
    topology_revision_t applied_revision;
    std::uint64_t generation {};
    std::optional<lease_token_t> lease;
    std::optional<reservation_token_t> protected_hold;
    monotonic_time_t ticket_expires {};
    monotonic_time_t reconnect_grace {};
    std::optional<monotonic_time_t> grace_expires;
    monotonic_time_t last_event {};
    bool effects_possible {};
    bool late_observation {};
    bool allow_degraded {};
    std::optional<owner_id_t> input_controller;
    std::map<owner_id_t, observer_grant_t> observers;
    bool
    operator==(const lease_state_t &) const = default;
  };

  enum class lease_event_e { reserve,
    prepare,
    authorize_mutation,
    apply,
    settle,
    activate,
    activate_degraded,
    disconnect,
    reconnect,
    grant_observer,
    revoke_observer,
    cancel,
    drain,
    restore,
    restored,
    failed,
    observe_late_completion,
    tick };

  struct lease_event_t {
    lease_event_e kind;
    owner_id_t actor;
    lease_token_t token;
    bool authorized_owner {};
    bool authorized_input {};
    std::optional<reservation_token_t> protected_hold;
    monotonic_time_t ticket_expires {};
    monotonic_time_t reconnect_grace {};
    bool allow_degraded {};
    std::optional<observer_grant_t> grant;
  };

  enum class transition_disposition_e { applied,
    ignored_stale };

  struct lease_transition_t {
    lease_state_t state;
    transition_disposition_e disposition { transition_disposition_e::applied };
    // The reducer never frees resources. P1 executes this release intent after
    // processing the state change in its serialized queue.
    bool release_protected_hold {};
  };

  result_t<lease_transition_t>
  transition(const lease_state_t &state, const lease_event_t &event,
    monotonic_time_t now, const p1_reservation_ledger_t &ledger);

}  // namespace display_device::topology
