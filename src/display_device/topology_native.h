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
 * P1 Windows implementation must refuse field writes.
 *
 * Every mutating entry point takes the worker-only `mutation_capability_t`.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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
    /**
     * Virtual only: how many heads one prepare creates, all or nothing (1..max_virtual_heads).
     * The setting parser always yields 1; more heads need multi-track streaming (P3), so
     * only code (tests and the gated hardware kit) asks for them.
     */
    std::uint8_t virtual_heads { 1 };
    /**
     * Virtual only: the most heads a display-group launch may prepare (setting key
     * `max_heads`, 1..max_virtual_heads). An ordinary launch still prepares `virtual_heads`.
     */
    std::uint8_t max_heads { 1 };

    bool
    operator==(const native_profile_t &) const = default;
  };

  inline constexpr std::uint8_t max_virtual_heads = 2;

  /**
   * @brief Parse the `display_topology_profile` setting.
   * @return `nullopt` for an empty setting (legacy mode), the profile, or an error for
   *         anything P1 cannot honour exactly (it never falls back to another display).
   */
  result_t<std::optional<native_profile_t>>
  parse_native_profile(std::string_view text);

  enum class creation_delivery_e : std::uint8_t {
    uncertain = 0,
    accepted,
  };

  /** Where one display's top-left corner goes on the host desktop (host pixels; 0,0 is the primary). */
  struct placement_t {
    os_display_id_t display;
    position_t position;
    bool
    operator==(const placement_t &) const = default;
  };

  /** The operation that placing displays needs under a capability of `kind`. */
  operation_e
  arrange_operation(command_kind_e kind) noexcept;

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
     * an accepted value confirms delivery, even if the head is not listed yet.
     * An uncertain value is not an acknowledgement or a refusal. In particular,
     * expected truthiness alone does not mean that delivery was accepted.
     */
    virtual result_t<creation_delivery_e>
    create_head(const mutation_capability_t &capability, const head_guid_t &intent) final;

    /** Remove every virtual head, then wait (bounded) for them to be unlisted. The driver offers no per-head removal. */
    virtual result_t<void>
    remove_all_heads(const mutation_capability_t &capability) final;

    /** Write the set fields of one display; unset fields stay untouched. */
    virtual result_t<void>
    apply_fields(const mutation_capability_t &capability, const os_display_id_t &display, const managed_values_t &values) final;

    /**
     * Put the virtual head `head` (as last observed) in `mode`, then wait (bounded) for it to be
     * listed with that resolution. The driver may recreate the head to do so: same GUID, new
     * incarnation, possibly a new OS identity. An error leaves the applied state unknown, so the
     * caller re-observes the host.
     */
    virtual result_t<void>
    change_head_mode(const mutation_capability_t &capability, const observed_display_t &head, const display_mode_t &mode) final;

    /**
     * Add the virtual head `head` (as last observed) to the desktop as its own extended display,
     * then wait (bounded) for it to be listed active. A newly created head can arrive inactive,
     * with no OS display name to capture. Other displays keep their topology groups. A head that
     * is already active is left alone. An error leaves the applied state unknown.
     */
    virtual result_t<void>
    activate_head(const mutation_capability_t &capability, const observed_display_t &head) final;

    /**
     * Move each listed desktop display's top-left corner to its placement in one desktop
     * write, then wait (bounded) for them to be listed there. The display placed at 0,0
     * becomes the primary. Modes, HDR and topology groups are not changed, and displays not
     * listed keep their corners. An error leaves the applied state unknown.
     */
    virtual result_t<void>
    arrange(const mutation_capability_t &capability, const std::vector<placement_t> &placements) final;

  protected:
    // Keep concrete hooks nonpublic and carry the capability through to raw setters.
    // The entry guards check authority, not proof that a head belongs to this request.
    virtual result_t<creation_delivery_e>
    do_create_head(const mutation_capability_t &capability, const head_guid_t &intent) = 0;

    virtual result_t<void>
    do_remove_all_heads(const mutation_capability_t &capability) = 0;

    virtual result_t<void>
    do_apply_fields(const mutation_capability_t &capability, const os_display_id_t &display, const managed_values_t &values) = 0;

    virtual result_t<void>
    do_change_head_mode(const mutation_capability_t &capability, const observed_display_t &head, const display_mode_t &mode) = 0;

    virtual result_t<void>
    do_activate_head(const mutation_capability_t &capability, const observed_display_t &head) = 0;

    virtual result_t<void>
    do_arrange(const mutation_capability_t &capability, const std::vector<placement_t> &placements) = 0;
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
    /** Every prepared display in creation order; `target` is the first. */
    std::vector<native_target_t> targets {};
  };

  enum class mode_change_verdict_e : std::uint8_t {
    /** The head now has the requested resolution; the journal names it as it is now. */
    applied,
    /** The head already had the requested resolution; nothing was written. */
    unchanged,
    /** The head keeps its previous mode (verified); the journal and health are unchanged. */
    refused,
    /** The head's state cannot be proven; the journal is kept and health is In conflict. */
    conflict,
  };

  std::string_view
  to_string(mode_change_verdict_e verdict) noexcept;

  struct mode_change_outcome_t {
    mode_change_verdict_e verdict { mode_change_verdict_e::refused };
    /** The head's OS identity afterwards (set for applied, unchanged and a verified refusal). */
    std::optional<os_display_id_t> display;
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
     *        An unconfirmed create remains unresolved even when no head is listed.
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

    /**
     * @brief Change the resolution of the one virtual head this run created; refresh is kept.
     * @details Only for a head this executor created in its current transaction and still
     *          proves as the sole matching head. A failed change is rolled back to the previous
     *          mode and re-verified: verified is a refusal that leaves health alone, otherwise
     *          In conflict. A recreated head (same GUID) is re-anchored in the journal so
     *          restore still proves and removes it.
     */
    mode_change_outcome_t
    change_mode(const mutation_capability_t &capability, const resolution_t &resolution);

    /**
     * @brief Change the resolution of one virtual head of this run's set; refresh is kept.
     * @details `head` is the 0-based index in creation order. One owned head is exactly the
     *          single-head change above. With several, the other heads must stay listed with the
     *          same identity, state and mode, and no other display may appear, vanish or change
     *          state; anything else is rolled back like the single-head change. A recreated head
     *          is re-anchored in its own journal slot only.
     */
    mode_change_outcome_t
    change_mode(const mutation_capability_t &capability, std::size_t head, const resolution_t &resolution);

    const native_health_t &
    health() const noexcept {
      return health_;
    }

  private:
    native_outcome_t
    reconcile(const mutation_capability_t &capability, const journal_record_t &record, bool same_run);

    native_outcome_t
    prepare_physical(const os_display_id_t &display);

    native_outcome_t
    prepare_virtual(const mutation_capability_t &capability, std::uint8_t count);

    /** Remove every virtual head when `proven` holds for the current inventory, then clear the journal. */
    native_outcome_t
    roll_back_heads(const mutation_capability_t &capability, const journal_record_t &record,
      const std::function<bool(const inventory_t &)> &proven, const std::string &why);

    /**
     * With no virtual display left, puts other displays back at their baseline corners when
     * only corners or the primary drifted (`keep_moves`: only if the primary or a state did).
     * nullopt when they match; otherwise why not, starting with `what`.
     */
    std::optional<std::string>
    put_back(const mutation_capability_t &capability, const journal_record_t &record, inventory_t &after, const std::string &what, bool keep_moves);

    /** Point the journal's observed creation and owned head at `head` as listed in `after`. */
    mode_change_outcome_t
    re_anchor(journal_record_t record, const observed_display_t &head, const inventory_t &after,
      mode_change_verdict_e verdict, std::string reason);

    /** Point the `index`-th observed creation and owned head at `head` as listed in `after`. */
    mode_change_outcome_t
    re_anchor_head(journal_record_t record, std::size_t index, const observed_display_t &head, const inventory_t &after,
      mode_change_verdict_e verdict, std::string reason);

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

  /**
   * @brief The latched `display_topology_profile` of a managed process bound to its executor.
   * @details Only a non-blank setting selects managed mode. One that does not parse keeps the
   *          process In conflict (never legacy, never another display): every launch is refused,
   *          while recovery, restore and reset still reconcile any journal of an earlier run.
   *          Entry points run on the coordinator worker, like the executor's.
   */
  class managed_native_t {
  public:
    /**
     * @param executor May be null only when `profile` is an error (no native platform was built).
     * @throws std::invalid_argument for a valid profile without an executor.
     */
    managed_native_t(native_executor_t *executor, result_t<native_profile_t> profile);

    native_health_t
    health() const;

    native_health_t
    recover(const mutation_capability_t &capability);

    /** Idempotent: lease end and shutdown. */
    native_health_t
    restore(const mutation_capability_t &capability);

    native_health_t
    reset(const mutation_capability_t &capability);

    native_outcome_t
    prepare(const mutation_capability_t &capability);

    /**
     * Prepare a display group of `heads` virtual heads, all or nothing. Refused, without
     * touching the host, unless the profile is virtual and offers that many (`max_heads`).
     */
    native_outcome_t
    prepare(const mutation_capability_t &capability, std::uint8_t heads);

    /** The most heads a display-group launch may ask for: 0 without a usable virtual profile. */
    std::uint8_t
    group_max_heads() const noexcept;

    /** The executor's `change_mode`; refused without a usable profile. */
    mode_change_outcome_t
    change_mode(const mutation_capability_t &capability, const resolution_t &resolution);

    /** The executor's per-head `change_mode` (`head` in creation order); refused without a usable profile. */
    mode_change_outcome_t
    change_mode(const mutation_capability_t &capability, std::size_t head, const resolution_t &resolution);

  private:
    native_executor_t *executor_;
    result_t<native_profile_t> profile_;
  };

  /** Builds the executor (and what it uses) of a managed process; may throw or return null. */
  using native_executor_factory_t = std::function<std::shared_ptr<native_executor_t>()>;

  /**
   * @brief Latch the `display_topology_profile` setting of this process.
   * @return nullptr for a blank setting (legacy mode; `make_executor` is not called).
   *         Otherwise a managed binding that keeps the executor alive. A setting that does not
   *         parse, or an executor that cannot be built, leaves it In conflict: never legacy.
   */
  std::shared_ptr<managed_native_t>
  bind_native_profile(std::string_view setting, const native_executor_factory_t &make_executor);

  /** Read-only admin report: the saved setting (Desired) against the setting latched by this process (Applied). */
  struct topology_status_report_t {
    /** `legacy`, `managed` or `invalid` (a restart would leave the process In conflict). */
    std::string desired;
    std::string desired_error;
    /** `legacy`, `managed` or `in_conflict`. */
    std::string applied;
    /** The saved setting means something other than the latched one. */
    bool restart_required {};
    bool journal_present {};
    std::string reason;
    std::size_t attachments {};
  };

  topology_status_report_t
  describe_topology_status(std::string_view desired_setting, std::string_view applied_setting, const coordinator_status_t &status);

}  // namespace display_device::topology
