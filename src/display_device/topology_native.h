/**
 * @file src/display_device/topology_native.h
 * @brief P1 managed (native) display path: profile, platform seam, executor and recovery.
 *
 * P1 manages exactly one monitor per session:
 *  - a physical display, resolved strictly by its OS display ID and captured as-is
 *    (no host writes, nothing to restore), or
 *  - one virtual display head, created only when no virtual head exists, journaled
 *    durably before creation and removed at the end only when it is provably ours.
 *
 * Field replay (restore managed fields that still hold the values this host applied)
 * is implemented generically so recovery honours journals with field steps, but the
 * production platform refuses field writes in P1.
 *
 * Every entry point takes the worker-only `mutation_capability_t`.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "topology_coordinator.h"
#include "topology_journal_store.h"

namespace display_device::topology {

  struct native_profile_t {
    enum class source_e : std::uint8_t {
      physical,
      virtual_head,
    };

    source_e source { source_e::physical };
    /** Physical only: the exact OS display ID to capture. */
    os_display_id_t display;

    bool
    operator==(const native_profile_t &) const = default;
  };

  /**
   * @brief Parse the `display_topology_profile` setting.
   * @return `nullopt` for an empty setting (legacy mode), the profile, or an error for
   *         anything P1 cannot honour exactly (it never falls back to another display).
   */
  result_t<std::optional<native_profile_t>>
  parse_native_profile(std::string_view text);

  class native_platform_t {
  public:
    virtual ~native_platform_t() = default;

    /**
     * Observe the host. `available == false` means unknown, never "no displays". Every
     * display is listed, including inactive ones, so an inactive virtual head counts too.
     */
    virtual inventory_t
    inventory() = 0;

    /** Whether `display` is a head of the virtual display driver. */
    virtual bool
    is_virtual_head(const observed_display_t &display) const = 0;

    /**
     * Ask the virtual display driver for one more head, then wait (bounded) for it to be
     * listed. An error means the driver refused, so no head of this request can appear;
     * success means it accepted, even if the head is not listed yet.
     */
    virtual result_t<void>
    create_head(const head_guid_t &intent) = 0;

    /** Remove every virtual head, then wait (bounded) for them to be unlisted. The driver offers no per-head removal. */
    virtual result_t<void>
    remove_all_heads() = 0;

    /** Write the set fields of one display; unset fields stay untouched. */
    virtual result_t<void>
    apply_fields(const os_display_id_t &display, const managed_values_t &values) = 0;
  };

  enum class native_verdict_e : std::uint8_t {
    /** Done; for a prepare, `target` is set. */
    ok,
    /** Nothing of ours is left on the host and no journal exists; the launch fails, health is clear. */
    refused,
    /** State cannot be proven; the journal is kept and health is In conflict. */
    conflict,
  };

  std::string_view
  to_string(native_verdict_e verdict) noexcept;

  struct native_target_t {
    os_display_id_t display;
    bool virtual_head {};

    bool
    operator==(const native_target_t &) const = default;
  };

  struct native_outcome_t {
    native_verdict_e verdict { native_verdict_e::ok };
    std::optional<native_target_t> target;
    std::string reason;
  };

  struct native_executor_options_t {
    /** Source of fresh, non-null identities (owner, transaction, request, head intent). */
    std::function<uuid_bytes_t()> new_uuid;
    /** Virtual head creation attempts; the first baseline is kept across them. */
    std::uint32_t create_attempts { 2 };
    /** Identity recorded for heads of the virtual display driver. */
    driver_instance_id_t driver;
  };

  /**
   * @brief Runs on the coordinator worker only (each entry point requires a capability).
   */
  class native_executor_t {
  public:
    native_executor_t(journal_store_t &store, native_platform_t &platform, native_executor_options_t options);

    /**
     * @brief Startup reconciliation. Nothing found on the host is provably ours after a
     *        restart, so a surviving virtual head is In conflict and never removed.
     */
    native_health_t
    recover(const mutation_capability_t &capability);

    /**
     * @brief Resolve (physical) or create (virtual) the managed monitor.
     * @details Fails closed while any journal exists. A virtual head is journaled before
     *          it is created, and every creation attempt keeps the first baseline.
     */
    native_outcome_t
    prepare(const mutation_capability_t &capability, const native_profile_t &profile);

    /** End of a managed session: undo what this run applied, then clear the journal. */
    native_outcome_t
    restore(const mutation_capability_t &capability);

    /** Explicit administrator reset: forget the journal without touching the host. It is the only way out of an unresolved step. */
    native_health_t
    reset(const mutation_capability_t &capability);

    const native_health_t &
    health() const noexcept {
      return health_;
    }

  private:
    native_outcome_t
    reconcile(const journal_record_t &record, bool same_run);

    native_outcome_t
    prepare_physical(const os_display_id_t &display);

    native_outcome_t
    prepare_virtual();

    native_outcome_t
    roll_back_head(const journal_record_t &record, const std::string &why);

    /** In conflict: the journal is kept. */
    native_outcome_t
    conflict(std::string reason);

    /** Only once no journal exists: nothing of ours is left on the host. */
    native_outcome_t
    refused(std::string reason);

    /** Remove the journal, then report `success`; In conflict if it cannot be removed. */
    native_outcome_t
    clear_journal(native_outcome_t success);

    /** An unexpected exception: In conflict if a journal may exist, otherwise refused. */
    native_outcome_t
    after_exception(const std::string &what);

    uuid_bytes_t
    fresh_uuid();

    journal_store_t &store_;
    native_platform_t &platform_;
    native_executor_options_t options_;
    /** Identity of this run; a journal written by another run is never "ours" to remove. */
    owner_id_t owner_;
    std::optional<transaction_id_t> created_in_this_run_;
    native_health_t health_;
  };

}  // namespace display_device::topology
