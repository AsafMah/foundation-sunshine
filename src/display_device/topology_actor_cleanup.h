/**
 * @file src/display_device/topology_actor_cleanup.h
 * @brief Hands a settled external actor's durable cleanup to its coordinator.
 *
 * The relay is the supervisor's cleanup sink. Once both the supervisor's
 * `cleanup_pending` hand-off and the caller's cleanup body are present, it
 * queues the body ahead of all pending work while dispatch is still held, and
 * only then lets the hand-off's exclusion go. The queued cleanup reports success
 * only after `cleanup_pending -> completed` is durable; until then the record
 * stays pending and the coordinator stays fenced.
 */
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>

#include "topology_actor_supervisor.h"
#include "topology_coordinator.h"

namespace display_device::topology {

  enum class actor_cleanup_state_e : std::uint8_t {
    /** Waiting for the hand-off, the body, or both. */
    awaiting,
    /** Queued with the coordinator; not yet run. */
    queued,
    /** The cleanup ran and `completed` is durable. */
    completed,
    /** The cleanup ran but failed, threw, or `completed` could not be made durable. */
    failed,
    /** A hand-off did not match, or the coordinator refused the cleanup. Exclusion retained. */
    refused,
  };

  class actor_cleanup_relay_t final: public actor_cleanup_sink_t {
  public:
    actor_cleanup_relay_t(std::shared_ptr<external_cleanup_t> cleanup, std::shared_ptr<actor_record_store_t> store,
      std::uint64_t operation);

    /**
     * Accepts only the first `cleanup_pending` hand-off of `operation` that carries
     * the obligation's own exclusion. Anything else, and any hand-off the
     * coordinator refuses, is quarantined for the life of the process.
     */
    void
    accept(actor_cleanup_handoff_t handoff) noexcept override;

    /**
     * Supply the cleanup body, once. True when the relay owns the cleanup from
     * here on (even before the hand-off arrives); the caller must then not run
     * its own restoration. False when `body` is empty, a body was already given,
     * or the cleanup was refused.
     */
    bool
    submit(external_cleanup_body_t body);

    actor_cleanup_state_e
    state() const;

    /** The coordinator obligation's exclusion, for the actor's supervisor. */
    std::shared_ptr<void>
    exclusion() const;

  private:
    struct shared_t {
      mutable std::mutex mutex;
      actor_cleanup_state_e state { actor_cleanup_state_e::awaiting };
    };

    void
    refuse() noexcept;

    void
    try_queue() noexcept;

    std::shared_ptr<external_cleanup_t> cleanup_;
    std::shared_ptr<actor_record_store_t> store_;
    std::uint64_t operation_;
    std::shared_ptr<shared_t> shared_ { std::make_shared<shared_t>() };
    /** Guarded by shared_->mutex. */
    std::optional<actor_cleanup_handoff_t> handoff_;
    external_cleanup_body_t body_;
    bool supplied_ {};
    bool claimed_ {};
  };

}  // namespace display_device::topology
