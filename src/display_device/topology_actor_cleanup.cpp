/**
 * @file src/display_device/topology_actor_cleanup.cpp
 * @brief Hands a settled external actor's durable cleanup to its coordinator.
 */
#include "topology_actor_cleanup.h"

#include <exception>
#include <new>

namespace display_device::topology {

  namespace {
    /**
     * Fail closed for the life of the process: a refused exclusion is never
     * released, even after the relay is gone. Refusals mean a wiring bug, so
     * the deliberate leak is bounded by the number of such bugs hit.
     */
    void
    quarantine(std::shared_ptr<void> &exclusion) noexcept {
      if (!exclusion) return;
      auto *kept = new (std::nothrow) std::shared_ptr<void> { std::move(exclusion) };
      if (!kept) {
        // Releasing it would lift a fence nobody discharged; there is no safer way out.
        std::terminate();
      }
    }

    /** Durable `cleanup_pending -> completed`, accepting a replace that committed but did not say so. */
    bool
    make_completed_durable(actor_record_store_t &store, const actor_record_t &record, std::uint64_t operation) {
      auto next = complete_actor_cleanup(record, operation);
      if (!next) {
        return false;
      }
      bool replaced = false;
      try {
        replaced = store.replace(record, *next).has_value();
      }
      catch (...) {
        replaced = false;
      }
      if (!replaced) {
        auto reloaded = store.load();
        if (reloaded.status != actor_load_status_e::loaded || reloaded.record != *next) {
          return false;
        }
      }
      try {
        (void) store.retire(*next);
      }
      catch (...) {
        // A completed record left behind is terminal; the next start retires it.
      }
      return true;
    }
  }  // namespace

  actor_cleanup_relay_t::actor_cleanup_relay_t(std::shared_ptr<external_cleanup_t> cleanup,
    std::shared_ptr<actor_record_store_t> store, std::uint64_t operation):
      cleanup_ { std::move(cleanup) },
      store_ { std::move(store) },
      operation_ { operation } {}

  void
  actor_cleanup_relay_t::accept(actor_cleanup_handoff_t handoff) noexcept {
    bool queue = false;
    try {
      // Only the obligation's own token proves dispatch is still held for it.
      const auto expected = cleanup_->exclusion();
      std::lock_guard lock { shared_->mutex };
      if (handoff_ || shared_->state == actor_cleanup_state_e::refused) {
        quarantine(handoff.exclusion);
        return;
      }
      if (handoff.record.operation != operation_ || handoff.record.stage != actor_stage_e::cleanup_pending ||
          !handoff.exclusion || handoff.exclusion != expected) {
        quarantine(handoff.exclusion);
        shared_->state = actor_cleanup_state_e::refused;
        return;
      }
      handoff_ = std::move(handoff);
      if (supplied_ && !claimed_) {
        claimed_ = true;
        queue = true;
      }
    }
    catch (...) {
      quarantine(handoff.exclusion);
      refuse();
      return;
    }
    if (queue) {
      try_queue();
    }
  }

  bool
  actor_cleanup_relay_t::submit(external_cleanup_body_t body) {
    bool queue = false;
    {
      std::lock_guard lock { shared_->mutex };
      if (!body || supplied_ || shared_->state == actor_cleanup_state_e::refused) {
        return false;
      }
      body_ = std::move(body);
      supplied_ = true;
      if (handoff_ && !claimed_) {
        claimed_ = true;
        queue = true;
      }
    }
    if (queue) {
      try_queue();
    }
    return state() != actor_cleanup_state_e::refused;
  }

  actor_cleanup_state_e
  actor_cleanup_relay_t::state() const {
    std::lock_guard lock { shared_->mutex };
    return shared_->state;
  }

  std::shared_ptr<void>
  actor_cleanup_relay_t::exclusion() const {
    return cleanup_->exclusion();
  }

  void
  actor_cleanup_relay_t::refuse() noexcept {
    try {
      std::lock_guard lock { shared_->mutex };
      shared_->state = actor_cleanup_state_e::refused;
      if (handoff_) quarantine(handoff_->exclusion);
    }
    catch (...) {
      // The exclusion stays in handoff_; the relay's owner keeps it.
    }
  }

  void
  actor_cleanup_relay_t::try_queue() noexcept {
    bool queued = false;
    try {
      actor_record_t record;
      external_cleanup_body_t body;
      {
        std::lock_guard lock { shared_->mutex };
        record = handoff_->record;
        body = std::move(body_);
      }
      auto wrapped = [shared = shared_, store = store_, record = std::move(record), operation = operation_,
                       body = std::move(body)](const mutation_capability_t &capability, const command_context_t &context) -> bool {
        auto outcome = actor_cleanup_state_e::failed;
        struct publish_t {
          shared_t &shared;
          actor_cleanup_state_e &outcome;
          ~publish_t() {
            std::lock_guard lock { shared.mutex };
            shared.state = outcome;
          }
        } publish { *shared, outcome };

        if (!body(capability, context) || !make_completed_durable(*store, record, operation)) {
          return false;
        }
        outcome = actor_cleanup_state_e::completed;
        return true;
      };
      queued = cleanup_->queue_first(std::move(wrapped));
    }
    catch (...) {
      queued = false;
    }
    if (!queued) {
      refuse();
      return;
    }

    // Dispatch is held by the hand-off's exclusion until it is released below,
    // so the queued body cannot have run yet.
    std::shared_ptr<void> release;
    try {
      std::lock_guard lock { shared_->mutex };
      if (shared_->state == actor_cleanup_state_e::awaiting) {
        shared_->state = actor_cleanup_state_e::queued;
      }
      release = std::move(handoff_->exclusion);
    }
    catch (...) {
      // Not released: the cleanup stays held behind it, fail closed.
    }
  }

}  // namespace display_device::topology
