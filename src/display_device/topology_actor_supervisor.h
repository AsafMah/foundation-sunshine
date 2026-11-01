/**
 * @file src/display_device/topology_actor_supervisor.h
 * @brief Bounded, strongly owned supervision of one external topology actor.
 *
 * `start` persists the actor's intent, publishes the supervisor (and with it the
 * caller's exclusion) and only then starts a background thread that runs the
 * actor body exactly once. The thread owns the supervisor, so no caller, worker
 * or session has to stay alive or join it.
 *
 * The caller's launch result is decided once, against an absolute deadline of at
 * most `actor_launch_deadline_limit` from `start`. That deadline covers process
 * creation and any lock the body waits for. A decision that would arrive after the
 * deadline is `uncertain`, never a late success. The durable record still follows
 * what actually happened, and nothing is retried or terminated.
 *
 * An exception, a contract violation or a failed durable write leaves the
 * supervisor `blocked`. It keeps the exclusion and every retained resource for as
 * long as it is referenced. The body and its captures are destroyed on the body
 * thread. Resources the body passes to `retain` are handed over to the supervisor
 * and are destroyed wherever its last reference is released, after the body has
 * stopped using them. A settled supervisor gives up its exclusion only through
 * `hand_off_cleanup`.
 */
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "topology_actor_record.h"

namespace display_device::topology {

  /** Hard ceiling on the caller's wait for an external actor to start. */
  inline constexpr std::chrono::milliseconds actor_launch_deadline_limit { 10'000 };

  /** Given to the body on the supervisor thread only. */
  class actor_body_context_t {
  public:
    virtual ~actor_body_context_t() = default;

    /**
     * Report once, right after the process exists. Returns false if the receipt
     * could not be made durable or was already reported. The body must still keep
     * its handles and wait for the actor; it must not terminate it.
     */
    virtual bool
    report_launched(actor_receipt_t receipt) = 0;

    /** Keep `resource` alive for the supervisor's lifetime, even if the body throws. */
    virtual void
    retain(std::shared_ptr<void> resource) = 0;
  };

  enum class actor_body_exit_e : std::uint8_t {
    /** Creation definitely failed and no process exists. Only valid before a report. */
    not_launched,
    /** The reported actor has exited. */
    exited,
    /** The actor's state can no longer be determined. */
    unobservable,
  };

  struct actor_body_result_t {
    actor_body_exit_e exit { actor_body_exit_e::unobservable };
    std::string detail;
  };

  using actor_body_t = std::function<actor_body_result_t(actor_body_context_t &)>;

  /** Runs `work` on a new thread that is never joined; throws if it cannot. */
  using actor_spawn_t = std::function<void(std::function<void()> work)>;

  enum class actor_launch_e : std::uint8_t {
    launched,
    not_launched,
    uncertain,
  };

  struct actor_launch_result_t {
    actor_launch_e status { actor_launch_e::uncertain };
    std::optional<actor_receipt_t> receipt;
    std::string detail;
    bool
    operator==(const actor_launch_result_t &) const = default;
  };

  enum class actor_phase_e : std::uint8_t {
    launching,
    running,
    /** The actor is gone and the record durably owes cleanup. */
    settled,
    /** The actor's state is not durably known; the exclusion stays held. */
    blocked,
  };

  struct actor_supervisor_snapshot_t {
    actor_phase_e phase { actor_phase_e::launching };
    actor_record_t record;
    std::optional<actor_launch_result_t> launch;
    std::string detail;
  };

  /** Cleanup ownership handed from a settled supervisor to the next durable owner. */
  struct actor_cleanup_handoff_t {
    /** The durable `cleanup_pending` record the new owner must discharge. */
    actor_record_t record;
    std::shared_ptr<void> exclusion;
  };

  /** Receives a settled supervisor's cleanup automatically. */
  class actor_cleanup_sink_t {
  public:
    virtual ~actor_cleanup_sink_t() = default;

    /**
     * Called once, on whichever thread durably settled the record, before the
     * supervisor reports `settled`. It must not block or call back into the
     * supervisor, and it owns the exclusion from here on.
     */
    virtual void
    accept(actor_cleanup_handoff_t handoff) noexcept = 0;
  };

  struct actor_supervisor_options_t {
    /** Clamped to `actor_launch_deadline_limit`; must be positive. */
    std::chrono::milliseconds deadline { actor_launch_deadline_limit };
    /**
     * Empty uses a detached `std::thread`. A spawner that throws may still have
     * kept or run the work: whichever of the work and the failure path claims the
     * launch first wins, so kept work can never start the actor after a failure.
     */
    actor_spawn_t spawn;
    /**
     * Fixed at start. When set, every settlement (including a launch abandoned
     * because publication or spawning failed) is delivered here, and
     * `hand_off_cleanup` is refused. A blocked supervisor never delivers.
     */
    std::shared_ptr<actor_cleanup_sink_t> cleanup_sink;
  };

  class actor_supervisor_t {
    struct private_t {};

  public:
    /** Must keep the supervisor alive while the exclusion matters; failure prevents launch. */
    using publish_t = std::function<result_t<void>(const std::shared_ptr<actor_supervisor_t> &)>;

    actor_supervisor_t(private_t, std::shared_ptr<actor_record_store_t> store, actor_record_t intent,
      std::shared_ptr<void> exclusion, actor_body_t body, std::chrono::steady_clock::time_point deadline);
    actor_supervisor_t(const actor_supervisor_t &) = delete;
    actor_supervisor_t &operator=(const actor_supervisor_t &) = delete;

    /**
     * Persist `intent`, publish, then launch. If the intent cannot be written,
     * nothing is published or launched. If publishing fails, nothing is launched,
     * the intent is settled into pending cleanup, and a supervisor the publisher
     * kept anyway is already finished as never launched.
     *
     * The deadline starts on entry, so the intent write and publication consume
     * it. Both are expected to be short: one local durable write and one
     * non-blocking registration.
     */
    static result_t<std::shared_ptr<actor_supervisor_t>>
    start(std::shared_ptr<actor_record_store_t> store, actor_record_t intent, std::shared_ptr<void> exclusion,
      const publish_t &publish, actor_body_t body, actor_supervisor_options_t options = {});

    /** Waits no later than the deadline; the first decision is final. */
    actor_launch_result_t
    wait_launch();

    actor_supervisor_snapshot_t
    snapshot() const;

    /** Bounded wait for `settled` or `blocked`. */
    bool
    wait_finished(std::chrono::milliseconds timeout) const;

    /**
     * Once settled, move the exclusion and the pending cleanup to the next owner,
     * exactly once. A running or blocked supervisor keeps both. Refused when a
     * cleanup sink was configured.
     */
    result_t<actor_cleanup_handoff_t>
    hand_off_cleanup();

    std::chrono::steady_clock::time_point
    deadline() const noexcept {
      return deadline_;
    }

  private:
    /** Owned by the supervisor, so a leaked reference never dangles; it refuses use off the body thread or after the body returns. */
    class context_t final: public actor_body_context_t {
    public:
      explicit context_t(actor_supervisor_t &owner):
          owner_(owner) {}

      bool
      report_launched(actor_receipt_t receipt) override;

      void
      retain(std::shared_ptr<void> resource) override;

    private:
      actor_supervisor_t &owner_;
    };

    void
    run() noexcept;

    /** Whoever claims first decides whether the body may ever run. */
    bool
    claim_launch() noexcept;

    void
    decide_launch_locked(actor_launch_result_t result);

    bool
    persist_receipt(actor_receipt_t receipt);

    void
    finish(actor_phase_e phase, std::string detail);

    /** After a durable settlement: deliver to the sink, if any, then report `settled`. */
    void
    finish_settled(std::string detail);

    /** Replace the durable record; an ambiguous failure is reconciled by reloading. */
    bool
    write_record(const actor_record_t &current, const actor_record_t &next);

    bool
    settle_record();

    std::shared_ptr<actor_record_store_t> store_;
    actor_body_t body_;
    const std::chrono::steady_clock::time_point deadline_;
    context_t context_ { *this };
    std::atomic<bool> claimed_ {};

    mutable std::mutex mutex_;
    mutable std::condition_variable changed_;
    std::shared_ptr<void> exclusion_;
    std::shared_ptr<actor_cleanup_sink_t> sink_;
    /** Set once at start; never cleared, so manual hand-off stays refused after delivery. */
    bool automatic_ {};
    actor_record_t record_;
    /** The durable record no longer matches anything this supervisor wrote. */
    bool record_unknown_ {};
    actor_phase_e phase_ { actor_phase_e::launching };
    std::optional<actor_launch_result_t> launch_;
    std::string detail_;
    std::thread::id body_thread_;
    bool body_open_ {};
    bool reported_ {};
    bool report_violation_ {};
    bool handed_off_ {};
    std::vector<std::shared_ptr<void>> retained_;
  };

}  // namespace display_device::topology
