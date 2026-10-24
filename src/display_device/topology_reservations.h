#pragma once

#include <limits>

#include "topology.h"

namespace display_device::topology {

  using reservation_id_t = identity_t<struct reservation_id_tag, uuid_bytes_t>;

  struct reservation_token_t {
    reservation_id_t id;
    owner_id_t owner;
    transaction_id_t transaction;
    ledger_revision_t issued;
    bool
    operator==(const reservation_token_t &) const = default;
  };

  enum class reservation_state_e { pending,
    committed,
    rolled_back,
    expired,
    released };

  template <class Resource>
  struct reservation_request_t {
    reservation_id_t id;
    owner_id_t owner;
    transaction_id_t transaction;
    topology_revision_t topology;
    std::map<Resource, std::uint64_t> amounts;
    monotonic_time_t expires {};
    bool
    operator==(const reservation_request_t &) const = default;
  };

  template <class Resource>
  struct reservation_record_t {
    reservation_token_t token;
    reservation_request_t<Resource> request;
    reservation_state_e state { reservation_state_e::pending };
    bool
    operator==(const reservation_record_t &) const = default;
  };

  // No lock, allocator, clock or external side effects. A P1 command queue owns
  // this value. Pending expiry is explicit; committed fences never time out.
  template <class Resource>
  class reservation_ledger_t {
  public:
    using request_t = reservation_request_t<Resource>;
    using record_t = reservation_record_t<Resource>;
    using capacities_t = std::map<Resource, std::uint64_t>;

    reservation_ledger_t(capacities_t capacities, std::size_t history_limit, monotonic_time_t max_pending):
        capacities_(std::move(capacities)),
        history_limit_(history_limit),
        max_pending_(max_pending) {}

    ledger_revision_t
    revision() const {
      return revision_;
    }

    std::uint64_t
    held(const Resource &resource) const {
      std::uint64_t amount {};
      for (const auto &[key, record] : records_) {
        if (record.state != reservation_state_e::pending && record.state != reservation_state_e::committed) continue;
        if (const auto it = record.request.amounts.find(resource); it != record.request.amounts.end()) amount += it->second;
      }
      return amount;
    }

    result_t<record_t>
    inspect(const reservation_token_t &token) const {
      const auto it = records_.find({ token.owner, token.id });
      if (it == records_.end() || it->second.token != token) return failure(error_code_e::stale_token, "Reservation token does not identify this operation");
      return it->second;
    }

    result_t<record_t>
    reserve(const request_t &request, ledger_revision_t expected, monotonic_time_t now) {
      const auto key = std::pair { request.owner, request.id };
      if (const auto it = records_.find(key); it != records_.end()) {
        if (it->second.request != request) return failure(error_code_e::request_conflict, "Reservation ID reused with different content");
        return it->second;
      }
      if (const auto check = writable(now); !check) return std::unexpected(check.error());
      if (expected != revision_) return failure(error_code_e::stale_revision, "Reservation ledger revision changed");
      if (!valid_uuid(request.id) || !valid_uuid(request.owner) || !valid_uuid(request.transaction)) {
        return failure(error_code_e::invalid_identity, "Reservation requires operation, owner and transaction identities");
      }
      if (request.expires <= now || max_pending_.count() <= 0 || request.expires - now > max_pending_) {
        return failure(error_code_e::invalid_deadline, "Pending reservation requires a bounded future deadline");
      }
      if (records_.size() >= history_limit_) return failure(error_code_e::history_full, "Reservation history is full; no terminal record is evicted");
      if (request.amounts.empty()) return failure(error_code_e::resource_unavailable, "Empty resource request");
      for (const auto &[resource, amount] : request.amounts) {
        const auto capacity = capacities_.find(resource);
        if (capacity == capacities_.end()) return failure(error_code_e::unknown_resource, "Resource is not in this immutable capacity snapshot");
        if (amount == 0 || amount > capacity->second - held(resource)) {
          return failure(error_code_e::resource_unavailable, "Complete required resource set is unavailable");
        }
      }
      advance(now);
      record_t record { { request.id, request.owner, request.transaction, revision_ }, request };
      records_.emplace(key, record);
      return record;
    }

    result_t<record_t>
    commit(const reservation_token_t &token, monotonic_time_t now) {
      return finish(token, now, reservation_state_e::pending, reservation_state_e::committed, true);
    }

    result_t<record_t>
    rollback(const reservation_token_t &token, monotonic_time_t now) {
      return finish(token, now, reservation_state_e::pending, reservation_state_e::rolled_back, false);
    }

    result_t<record_t>
    release(const reservation_token_t &token, monotonic_time_t now) {
      return finish(token, now, reservation_state_e::committed, reservation_state_e::released, false);
    }

    result_t<std::vector<reservation_token_t>>
    expire(monotonic_time_t now) {
      if (const auto check = writable(now); !check) return std::unexpected(check.error());
      std::vector<reservation_token_t> expired;
      for (auto &[key, record] : records_) {
        if (record.state == reservation_state_e::pending && now >= record.request.expires) {
          record.state = reservation_state_e::expired;
          expired.push_back(record.token);
        }
      }
      if (!expired.empty()) advance(now);
      return expired;
    }

    bool
    operator==(const reservation_ledger_t &) const = default;

  private:
    static std::unexpected<error_t>
    failure(error_code_e code, std::string detail) {
      return std::unexpected(error_t { code, std::move(detail), {} });
    }

    result_t<void>
    writable(monotonic_time_t now) const {
      if (now < last_write_) return failure(error_code_e::clock_reversed, "Monotonic time moved backwards");
      if (revision_.value == std::numeric_limits<std::uint64_t>::max()) return failure(error_code_e::arithmetic_overflow, "Ledger revision exhausted");
      return {};
    }

    void
    advance(monotonic_time_t now) {
      ++revision_.value;
      last_write_ = now;
    }

    result_t<record_t>
    finish(const reservation_token_t &token, monotonic_time_t now, reservation_state_e from, reservation_state_e to, bool check_expiry) {
      const auto current = inspect(token);
      if (!current) return std::unexpected(current.error());
      if (current->state != from) return failure(error_code_e::already_finalized, "Reservation already left the required state");
      if (check_expiry && now >= current->request.expires) return failure(error_code_e::expired, "Late completion cannot commit an expired reservation");
      if (const auto check = writable(now); !check) return std::unexpected(check.error());
      auto &record = records_.at({ token.owner, token.id });
      record.state = to;
      advance(now);
      return record;
    }

    capacities_t capacities_;
    std::size_t history_limit_;
    monotonic_time_t max_pending_;
    ledger_revision_t revision_;
    monotonic_time_t last_write_ {};
    std::map<std::pair<owner_id_t, reservation_id_t>, record_t> records_;
  };

  enum class resource_kind_e { topology_owner,
    launch_ticket,
    vdd_slot };

  struct resource_key_t {
    resource_kind_e kind;
    driver_instance_id_t driver;
    adapter_luid_t adapter;
    std::uint32_t slot {};
    auto
    operator<=>(const resource_key_t &) const = default;
  };

  inline const resource_key_t owner_resource { resource_kind_e::topology_owner, {}, {}, 0 };
  inline const resource_key_t ticket_resource { resource_kind_e::launch_ticket, {}, {}, 0 };
  using p1_reservation_ledger_t = reservation_ledger_t<resource_key_t>;

  result_t<p1_reservation_ledger_t::capacities_t>
  p1_capacities(const inventory_t &inventory, std::uint32_t tickets, std::uint32_t max_slots);

  result_t<owned_head_t>
  reuse_owned_head(const inventory_t &inventory, const owned_head_t &expected);

}  // namespace display_device::topology
