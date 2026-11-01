/**
 * @file src/display_device/topology_actor_record.h
 * @brief Durable write-ahead record for one external topology actor and its cleanup.
 *
 * An external actor (for example a legacy display adapter restart helper) can
 * mutate topology outside the coordinator. Its record is written before the
 * actor is launched and is never discarded until its cleanup obligation has
 * completed or been durably transferred:
 *
 *   intent -> launched (PID + creation-time receipt) -> cleanup_pending
 *          -> completed | transferred
 *
 * `intent` may move straight to `cleanup_pending`. That is used when the parent
 * died inside process creation before a receipt and the boot has positively
 * changed. Operation identity, boot identity and the cleanup obligation are
 * immutable after intent. Every transition names the operation, so a callback
 * from an older operation cannot advance a newer record.
 *
 * A receipt identifies a process, never ownership of a display head. Cleanup
 * recovered after a restart is marker-only: it must not destroy unknown heads.
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include "topology.h"

namespace display_device::topology {

  enum class actor_stage_e : std::uint8_t {
    intent,
    launched,
    cleanup_pending,
    completed,
    transferred,
  };

  std::string_view
  to_string(actor_stage_e stage) noexcept;

  enum class actor_obligation_e : std::uint8_t {
    restore_legacy_display,
  };

  struct actor_receipt_t {
    std::uint32_t pid {};
    /** Platform process creation time; nonzero. Distinguishes a reused PID. */
    std::uint64_t creation_time {};
    bool
    operator==(const actor_receipt_t &) const = default;
  };

  struct actor_record_t {
    std::uint32_t version { 1 };
    std::uint64_t operation {};
    /** Trustworthy OS boot identity at intent, or unknown. */
    std::optional<std::string> boot;
    actor_obligation_e obligation { actor_obligation_e::restore_legacy_display };
    std::string obligation_detail;
    actor_stage_e stage { actor_stage_e::intent };
    std::optional<actor_receipt_t> receipt;
    std::string transferred_to;
    bool
    operator==(const actor_record_t &) const = default;
  };

  /** Whether the record's obligation is discharged and it may be retired. */
  bool
  actor_record_terminal(const actor_record_t &record) noexcept;

  result_t<actor_record_t>
  begin_actor_intent(std::uint64_t operation, std::optional<std::string> boot,
    actor_obligation_e obligation, std::string obligation_detail);

  result_t<actor_record_t>
  record_actor_receipt(const actor_record_t &record, std::uint64_t operation, actor_receipt_t receipt);

  /** The actor can no longer mutate topology; its cleanup is still owed. */
  result_t<actor_record_t>
  settle_actor(const actor_record_t &record, std::uint64_t operation);

  result_t<actor_record_t>
  complete_actor_cleanup(const actor_record_t &record, std::uint64_t operation);

  /** `destination` names the durable owner that has already accepted the obligation. */
  result_t<actor_record_t>
  transfer_actor_cleanup(const actor_record_t &record, std::uint64_t operation, std::string destination);

  result_t<std::string>
  serialize_actor_record(const actor_record_t &record);

  result_t<actor_record_t>
  deserialize_actor_record(std::string_view serialized);

  /** Largest durable record accepted; anything longer is corrupt, never truncated. */
  inline constexpr std::size_t actor_record_read_limit = 64 * 1024;

  enum class actor_load_status_e : std::uint8_t {
    absent,
    loaded,
    invalid,
    unsupported_version,
    unreadable,
  };

  struct actor_load_t {
    actor_load_status_e status { actor_load_status_e::absent };
    std::optional<actor_record_t> record;
    std::string detail;
  };

  /**
   * One durable actor record. Writes are compare-and-replace against the durable
   * contents, so a stale in-memory copy cannot overwrite a newer record. There is
   * deliberately no unconditional clear: only a terminal record can be retired.
   */
  class actor_record_store_t {
  public:
    virtual ~actor_record_store_t() = default;

    virtual actor_load_t
    load() = 0;

    /** Write `record` only when no record exists, usable or not. */
    virtual result_t<void>
    create(const actor_record_t &record) = 0;

    /** Replace only when the durable record equals `expected` and has the same operation. */
    virtual result_t<void>
    replace(const actor_record_t &expected, const actor_record_t &next) = 0;

    /** Remove only a terminal durable record equal to `expected`. */
    virtual result_t<void>
    retire(const actor_record_t &expected) = 0;
  };

  /**
   * The process-wide lock for one record path. Every store instance naming the
   * same file serializes its read/compare/write through this lock. It does not
   * exclude another process; cross-process exclusion is not provided yet.
   */
  std::shared_ptr<std::mutex>
  actor_record_path_lock(const std::filesystem::path &path);

  class file_actor_record_store_t final: public actor_record_store_t {
  public:
    explicit file_actor_record_store_t(std::filesystem::path path);

    actor_load_t
    load() override;

    result_t<void>
    create(const actor_record_t &record) override;

    result_t<void>
    replace(const actor_record_t &expected, const actor_record_t &next) override;

    result_t<void>
    retire(const actor_record_t &expected) override;

    const std::filesystem::path &
    path() const noexcept {
      return path_;
    }

  private:
    actor_load_t
    load_locked();

    std::filesystem::path path_;
    std::shared_ptr<std::mutex> mutex_;
  };

  /**
   * Result of opening the receipt's process once and using that same handle for
   * both identity (creation time) and liveness.
   */
  enum class process_observation_e : std::uint8_t {
    running,
    exited,
    /** No process has this PID. */
    absent,
    /** The PID belongs to a process with a different creation time. */
    identity_mismatch,
    /** Access was denied or the evidence was ambiguous. */
    inaccessible,
  };

  using process_observer_t = std::function<process_observation_e(const actor_receipt_t &)>;

  enum class actor_recovery_e : std::uint8_t {
    /** No record: nothing external to recover. */
    none,
    /** Unusable or insufficient evidence: topology stays fenced. */
    blocked,
    /** The recorded actor is still alive: adopt it and wait; never relaunch. */
    actor_running,
    /** The actor is gone but the record does not say so yet: persist settle, then clean up. */
    actor_settled,
    /** The durable record already owes cleanup. */
    cleanup_pending,
    /** The obligation is discharged; the record may be retired. */
    finished,
  };

  std::string_view
  to_string(actor_recovery_e disposition) noexcept;

  struct actor_recovery_t {
    actor_recovery_e disposition { actor_recovery_e::none };
    std::optional<actor_record_t> record;
    std::string detail;
  };

  /**
   * Classify a loaded record at startup. `current_boot` is the trustworthy boot
   * identity now, or unknown. `observe` is consulted only for a launched record
   * whose boot has not positively changed; if it throws, recovery is blocked.
   */
  actor_recovery_t
  classify_actor_recovery(const actor_load_t &loaded, const std::optional<std::string> &current_boot,
    const process_observer_t &observe);

}  // namespace display_device::topology
