/**
 * @file src/video_admission.h
 * @brief Revisioned live admission budget for exact capture sources.
 *
 * Wraps the generic reservation ledger with per-adapter encode-session and
 * pixel-rate resources. Limits are caller-supplied policy, not hardware claims;
 * without a policy the budget is disabled and admits without accounting.
 */
#pragma once

#include <algorithm>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <utility>

#include "display_device/topology_reservations.h"

namespace video::admission {
  namespace topology = display_device::topology;

  enum class resource_kind_e { encode_session,
    pixel_rate };

  struct resource_key_t {
    resource_kind_e kind;
    std::uint64_t adapter_luid {};
    auto
    operator<=>(const resource_key_t &) const = default;
  };

  struct adapter_limits_t {
    std::uint64_t encode_sessions {};
    std::uint64_t pixel_rate {};
  };

  struct policy_t {
    std::map<std::uint64_t, adapter_limits_t> adapters;
    topology::monotonic_time_t max_pending {};
    std::size_t history_limit {};
  };

  struct request_t {
    std::uint64_t adapter_luid {};
    std::uint32_t width {};
    std::uint32_t height {};
    std::uint32_t fps {};
  };

  using clock_t = std::function<topology::monotonic_time_t()>;
  using ledger_t = topology::reservation_ledger_t<resource_key_t>;

  namespace detail {
    struct handle_t {
      // Absent once the record left the ledger during compaction; terminal says how.
      std::optional<topology::reservation_token_t> token;
      topology::reservation_state_e terminal { topology::reservation_state_e::pending };
    };

    struct state_t {
      std::mutex lock;
      policy_t policy;
      ledger_t::capacities_t capacities;
      clock_t clock;
      std::optional<ledger_t> ledger;
      std::uint64_t revision_base {};
      std::map<std::uint64_t, handle_t> handles;
      std::uint64_t next_handle { 1 };
      std::mt19937_64 random { std::random_device {}() };
      topology::owner_id_t owner;
      topology::monotonic_time_t last_now { std::numeric_limits<topology::monotonic_time_t::rep>::min() };

      // Clamped so a clock step backwards can never make a release or rollback fail.
      topology::monotonic_time_t
      now() {
        last_now = std::max(last_now, clock());
        return last_now;
      }

      template <class Id>
      Id
      new_id() {
        topology::uuid_bytes_t bytes {};
        do {
          for (std::size_t i = 0; i < bytes.size(); i += 8) {
            const auto value = random();
            for (std::size_t j = 0; j < 8; ++j) bytes[i + j] = static_cast<std::uint8_t>(value >> (j * 8));
          }
        } while (bytes == topology::uuid_bytes_t {});
        return Id { bytes };
      }

      std::uint64_t
      revision() const {
        return revision_base + ledger->revision().value;
      }

      // Rebuild the ledger with only live records so bounded history never
      // refuses admission while capacity is free; handles are remapped to their
      // new tokens. Nothing changes unless every live record moves, and the
      // exposed revision is unchanged because no holding changed.
      bool
      compact(topology::monotonic_time_t now) {
        const auto previous = revision();
        if (now.count() == std::numeric_limits<topology::monotonic_time_t::rep>::max()) return false;
        ledger_t fresh { capacities, policy.history_limit, policy.max_pending };
        std::map<std::uint64_t, handle_t> remapped;
        std::size_t live {};
        for (const auto &[id, handle] : handles) {
          auto next = handle;
          if (handle.token) {
            const auto record = ledger->inspect(*handle.token);
            if (record && (record->state == topology::reservation_state_e::pending || record->state == topology::reservation_state_e::committed)) {
              if (++live >= policy.history_limit) return false;
              auto request = record->request;
              const bool committed = record->state == topology::reservation_state_e::committed;
              if (committed) request.expires = now + topology::monotonic_time_t { 1 };
              auto moved = fresh.reserve(request, fresh.revision(), now);
              if (moved && committed) moved = fresh.commit(moved->token, now);
              if (!moved) return false;
              next.token = moved->token;
            } else {
              next.terminal = record ? record->state : topology::reservation_state_e::expired;
              next.token.reset();
            }
          }
          remapped.emplace(id, next);
        }
        revision_base = previous - fresh.revision().value;
        ledger.emplace(std::move(fresh));
        handles = std::move(remapped);
        return true;
      }
    };
  }  // namespace detail

  class lease_t {
  public:
    lease_t() = default;
    lease_t(std::shared_ptr<detail::state_t> state, std::uint64_t handle):
        _state(std::move(state)),
        _handle(handle) {}
    lease_t(lease_t &&other) noexcept:
        _state(std::move(other._state)),
        _handle(std::exchange(other._handle, 0)) {}
    lease_t &
    operator=(lease_t &&other) noexcept {
      if (this != &other) {
        reset();
        _state = std::move(other._state);
        _handle = std::exchange(other._handle, 0);
      }
      return *this;
    }
    lease_t(const lease_t &) = delete;
    lease_t &
    operator=(const lease_t &) = delete;
    ~lease_t() {
      reset();
    }

    void
    reset() noexcept {
      if (!_state || !_handle) {
        _state.reset();
        return;
      }
      std::lock_guard lg { _state->lock };
      if (const auto it = _state->handles.find(_handle); it != _state->handles.end()) {
        if (it->second.token) (void) _state->ledger->release(*it->second.token, _state->now());
        _state->handles.erase(it);
      }
      _state.reset();
      _handle = 0;
    }

  private:
    std::shared_ptr<detail::state_t> _state;
    std::uint64_t _handle {};
  };

  class pending_t {
  public:
    pending_t() = default;
    static pending_t
    unaccounted() {
      pending_t pending;
      pending._unaccounted = true;
      return pending;
    }
    pending_t(std::shared_ptr<detail::state_t> state, std::uint64_t handle):
        _state(std::move(state)),
        _handle(handle) {}
    pending_t(pending_t &&other) noexcept:
        _state(std::move(other._state)),
        _handle(std::exchange(other._handle, 0)),
        _unaccounted(std::exchange(other._unaccounted, false)),
        _consumed(std::exchange(other._consumed, false)) {}
    pending_t &
    operator=(pending_t &&other) noexcept {
      if (this != &other) {
        reset();
        _state = std::move(other._state);
        _handle = std::exchange(other._handle, 0);
        _unaccounted = std::exchange(other._unaccounted, false);
        _consumed = std::exchange(other._consumed, false);
      }
      return *this;
    }
    pending_t(const pending_t &) = delete;
    pending_t &
    operator=(const pending_t &) = delete;
    ~pending_t() {
      reset();
    }

    /** Commit exactly once; a committed lease never expires. */
    topology::result_t<lease_t>
    commit() {
      if (_consumed) return failure(topology::error_code_e::already_finalized, "Admission was already committed");
      if (_unaccounted) {
        _consumed = true;
        return lease_t {};
      }
      if (!_state || !_handle) return failure(topology::error_code_e::stale_token, "No admission to commit");
      std::lock_guard lg { _state->lock };
      const auto it = _state->handles.find(_handle);
      if (it == _state->handles.end()) return failure(topology::error_code_e::stale_token, "No admission to commit");
      if (!it->second.token) {
        return failure(it->second.terminal == topology::reservation_state_e::expired ? topology::error_code_e::expired : topology::error_code_e::already_finalized,
          "Admission left the budget before commit");
      }
      const auto now = _state->now();
      if (auto swept = _state->ledger->expire(now); !swept) return std::unexpected(swept.error());
      const auto record = _state->ledger->inspect(*it->second.token);
      if (record && record->state == topology::reservation_state_e::expired) {
        return failure(topology::error_code_e::expired, "Admission expired before commit");
      }
      auto committed = _state->ledger->commit(*it->second.token, now);
      if (!committed) return std::unexpected(committed.error());
      _consumed = true;
      return lease_t { std::exchange(_state, {}), std::exchange(_handle, 0) };
    }

    void
    reset() noexcept {
      if (!_state || !_handle) {
        _state.reset();
        return;
      }
      std::lock_guard lg { _state->lock };
      if (const auto it = _state->handles.find(_handle); it != _state->handles.end()) {
        if (it->second.token) {
          const auto record = _state->ledger->inspect(*it->second.token);
          if (record && record->state == topology::reservation_state_e::pending) {
            (void) _state->ledger->rollback(*it->second.token, _state->now());
          }
        }
        _state->handles.erase(it);
      }
      _state.reset();
      _handle = 0;
    }

  private:
    static std::unexpected<topology::error_t>
    failure(topology::error_code_e code, std::string detail) {
      return std::unexpected(topology::error_t { code, std::move(detail), {} });
    }

    std::shared_ptr<detail::state_t> _state;
    std::uint64_t _handle {};
    bool _unaccounted {};
    bool _consumed {};
  };

  class budget_t {
  public:
    budget_t(std::optional<policy_t> policy, clock_t clock):
        _state(std::make_shared<detail::state_t>()) {
      _state->clock = std::move(clock);
      if (!policy) return;
      _state->policy = std::move(*policy);
      for (const auto &[luid, limits] : _state->policy.adapters) {
        _state->capacities.emplace(resource_key_t { resource_kind_e::encode_session, luid }, limits.encode_sessions);
        _state->capacities.emplace(resource_key_t { resource_kind_e::pixel_rate, luid }, limits.pixel_rate);
      }
      _state->ledger.emplace(_state->capacities, _state->policy.history_limit, _state->policy.max_pending);
      _state->owner = _state->new_id<topology::owner_id_t>();
      _enforced = true;
    }

    bool
    enforced() const {
      return _enforced;
    }

    std::uint64_t
    revision() const {
      std::lock_guard lg { _state->lock };
      return _state->ledger ? _state->revision() : 0;
    }

    std::uint64_t
    held(resource_kind_e kind, std::uint64_t adapter_luid) const {
      std::lock_guard lg { _state->lock };
      return _state->ledger ? _state->ledger->held({ kind, adapter_luid }) : 0;
    }

    /** Atomically reserve one encode session and the mode's pixel rate on one adapter. */
    topology::result_t<pending_t>
    reserve(const request_t &request, topology::monotonic_time_t pending_for) {
      if (!_enforced) return pending_t::unaccounted();
      if (request.width == 0 || request.height == 0 || request.fps == 0) {
        return failure(topology::error_code_e::invalid_profile, "Admission needs a non-zero mode");
      }
      std::uint64_t pixels {};
      std::uint64_t rate {};
      if (__builtin_mul_overflow(std::uint64_t { request.width }, std::uint64_t { request.height }, &pixels) ||
          __builtin_mul_overflow(pixels, std::uint64_t { request.fps }, &rate)) {
        return failure(topology::error_code_e::arithmetic_overflow, "Admission pixel rate overflows");
      }

      std::lock_guard lg { _state->lock };
      const auto now = _state->now();
      topology::monotonic_time_t::rep deadline {};
      if (__builtin_add_overflow(now.count(), pending_for.count(), &deadline)) {
        return failure(topology::error_code_e::arithmetic_overflow, "Admission deadline overflows");
      }
      if (auto expired = _state->ledger->expire(now); !expired) return std::unexpected(expired.error());
      ledger_t::request_t reservation {
        _state->new_id<topology::reservation_id_t>(),
        _state->owner,
        _state->new_id<topology::transaction_id_t>(),
        {},
        { { { resource_kind_e::encode_session, request.adapter_luid }, 1 }, { { resource_kind_e::pixel_rate, request.adapter_luid }, rate } },
        topology::monotonic_time_t { deadline },
      };
      auto record = _state->ledger->reserve(reservation, _state->ledger->revision(), now);
      if (!record && record.error().code == topology::error_code_e::history_full && _state->compact(now)) {
        record = _state->ledger->reserve(reservation, _state->ledger->revision(), now);
      }
      if (!record) return std::unexpected(record.error());
      const auto handle = _state->next_handle++;
      _state->handles.emplace(handle, detail::handle_t { record->token });
      return pending_t { _state, handle };
    }

  private:
    static std::unexpected<topology::error_t>
    failure(topology::error_code_e code, std::string detail) {
      return std::unexpected(topology::error_t { code, std::move(detail), {} });
    }

    std::shared_ptr<detail::state_t> _state;
    bool _enforced {};
  };
}  // namespace video::admission
