#pragma once

#include <array>
#include <chrono>
#include <compare>
#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "parsed_config.h"

namespace display_device::topology {

  template <class Tag, class Value>
  struct identity_t {
    Value value {};

    identity_t() = default;
    explicit constexpr identity_t(Value value):
        value(std::move(value)) {}
    auto
    operator<=>(const identity_t &) const = default;
  };

  using uuid_bytes_t = std::array<std::uint8_t, 16>;
  using profile_id_t = identity_t<struct profile_id_tag, uuid_bytes_t>;
  using monitor_id_t = identity_t<struct monitor_id_tag, uuid_bytes_t>;
  using os_display_id_t = identity_t<struct os_display_id_tag, std::string>;
  using head_guid_t = identity_t<struct head_guid_tag, uuid_bytes_t>;
  using driver_instance_id_t = identity_t<struct driver_instance_id_tag, uuid_bytes_t>;
  using owner_id_t = identity_t<struct owner_id_tag, uuid_bytes_t>;
  using request_id_t = identity_t<struct request_id_tag, uuid_bytes_t>;
  using transaction_id_t = identity_t<struct transaction_id_tag, uuid_bytes_t>;
  using adapter_luid_t = identity_t<struct adapter_luid_tag, std::uint64_t>;
  using head_incarnation_t = identity_t<struct head_incarnation_tag, std::uint64_t>;
  using source_incarnation_t = identity_t<struct source_incarnation_tag, std::uint64_t>;
  using track_id_t = identity_t<struct track_id_tag, std::uint64_t>;
  using track_epoch_t = identity_t<struct track_epoch_tag, std::uint64_t>;
  using topology_revision_t = identity_t<struct topology_revision_tag, std::uint64_t>;
  using inventory_revision_t = identity_t<struct inventory_revision_tag, std::uint64_t>;
  using ledger_revision_t = identity_t<struct ledger_revision_tag, std::uint64_t>;
  using monotonic_time_t = std::chrono::milliseconds;

  enum class error_code_e {
    invalid_identity,
    invalid_profile,
    duplicate_monitor,
    duplicate_source,
    invalid_order,
    invalid_geometry,
    arithmetic_overflow,
    invalid_refresh,
    unsupported_rotation,
    overlap,
    partial_clone_edit,
    permission_denied,
    limit_exceeded,
    unsupported_mode,
    stale_revision,
    inventory_unavailable,
    missing_identity,
    ambiguous_identity,
    source_kind_mismatch,
    adapter_mismatch,
    not_owned,
    unknown_applied_state,
    request_conflict,
    history_full,
    invalid_transition,
    owner_busy,
    expired,
    invalid_deadline,
    clock_reversed,
    stale_token,
    resource_unavailable,
    unknown_resource,
    already_finalized,
    fence_required,
    unsupported_version,
    invalid_journal
  };

  struct error_t {
    error_code_e code;
    std::string detail;
    std::optional<monitor_id_t> monitor;
    bool
    operator==(const error_t &) const = default;
  };

  template <class T>
  using result_t = std::expected<T, error_t>;

  template <class Tag>
  bool
  valid_uuid(const identity_t<Tag, uuid_bytes_t> &id) {
    return id.value != uuid_bytes_t {};
  }

  enum class rotation_e : std::uint16_t { identity = 0,
    clockwise90 = 90,
    clockwise180 = 180,
    clockwise270 = 270 };

  struct position_t {
    std::int32_t x {};
    std::int32_t y {};
    bool
    operator==(const position_t &) const = default;
  };

  // Resolution and position are already oriented host-desktop pixels, not DIPs
  // or unrotated panel pixels. Rotation must not swap these extents again.
  struct layout_t {
    position_t position;
    display_mode_t mode {};
    rotation_e rotation { rotation_e::identity };
    std::optional<std::uint32_t> scale_percent;
    hdr_state_e hdr { hdr_state_e::unknown };
    bool
    operator==(const layout_t &) const;
  };

  struct head_identity_t {
    driver_instance_id_t driver;
    head_guid_t guid;
    head_incarnation_t incarnation;
    auto
    operator<=>(const head_identity_t &) const = default;
  };

  struct owned_head_t {
    head_identity_t identity;
    owner_id_t owner;
    adapter_luid_t adapter;
    std::uint32_t slot {};
    bool
    operator==(const owned_head_t &) const = default;
  };

  struct observed_display_t {
    os_display_id_t id;
    device_info_t info {};
    std::optional<layout_t> layout;
    std::optional<adapter_luid_t> adapter;
    source_incarnation_t incarnation;
    std::optional<head_identity_t> head;
    std::vector<display_mode_t> supported_modes;
    bool
    operator==(const observed_display_t &) const;
  };

  struct observed_head_t {
    head_identity_t identity;
    std::optional<owner_id_t> owner;
    adapter_luid_t adapter;
    std::uint32_t slot {};
    std::optional<os_display_id_t> display;
    bool
    operator==(const observed_head_t &) const = default;
  };

  struct driver_capacity_t {
    driver_instance_id_t driver;
    adapter_luid_t adapter;
    std::uint32_t slots {};
    bool
    operator==(const driver_capacity_t &) const = default;
  };

  struct inventory_t {
    inventory_revision_t revision;
    bool available { false };
    std::vector<observed_display_t> displays;
    std::vector<observed_head_t> heads;
    std::vector<driver_capacity_t> drivers;
    active_topology_t clone_groups;
    std::optional<std::string> audio_endpoint;
    bool
    operator==(const inventory_t &) const = default;
  };

  struct physical_source_t {
    os_display_id_t id;
    std::optional<adapter_luid_t> adapter;
    bool
    operator==(const physical_source_t &) const = default;
  };

  struct virtual_source_t {
    head_guid_t guid;
    owner_id_t owner;
    adapter_luid_t adapter;
    bool
    operator==(const virtual_source_t &) const = default;
  };

  struct legacy_primary_t {
    bool
    operator==(const legacy_primary_t &) const = default;
  };

  using source_selection_t = std::variant<physical_source_t, virtual_source_t, legacy_primary_t>;
  enum class primary_intent_e { preserve,
    make_primary };

  struct monitor_entry_t {
    monitor_id_t id;
    source_selection_t source;
    std::optional<layout_t> desired;
    primary_intent_e primary { primary_intent_e::preserve };
    bool required { true };
    bool
    operator==(const monitor_entry_t &) const = default;
  };

  struct permissions_t {
    bool view { true };
    bool input { false };
    bool change_topology { false };
    bool change_physical { false };
    bool create_virtual { false };
    bool resize { false };
    bool rotate { false };
    bool hdr { false };
    bool scale { false };
    bool primary { false };
    bool
    operator==(const permissions_t &) const = default;
  };

  struct limits_t {
    std::uint32_t monitors {};
    std::uint32_t virtual_monitors {};
    resolution_t dimensions {};
    std::uint64_t pixels_per_second {};
    std::uint64_t total_pixels_per_second {};
    bool
    operator==(const limits_t &) const;
  };

  // Raw compatibility intent is kept, not executed or promoted into native
  // permissions. The production parser and its fallback remain a P1 adapter.
  struct legacy_intent_t {
    std::string output_name;
    parsed_config_t parsed {};
    bool keep_enabled {};
    bool headless_create {};
    bool reuse {};
    bool
    operator==(const legacy_intent_t &) const;
  };

  struct profile_t {
    std::uint32_t version { 1 };
    profile_id_t id;
    std::string name;
    std::vector<monitor_entry_t> entries;
    std::vector<monitor_id_t> order;
    monitor_id_t fallback;
    monitor_id_t preferred;
    permissions_t permissions;
    limits_t limits;
    bool allow_degraded { false };
    bool allow_adjustments { false };
    monotonic_time_t reconnect_grace {};
    std::optional<legacy_intent_t> legacy;
    bool
    operator==(const profile_t &) const = default;
  };

  enum class binding_state_e { bound,
    unavailable,
    needs_rebind };

  struct source_binding_t {
    os_display_id_t display;
    adapter_luid_t adapter;
    source_incarnation_t incarnation;
    std::optional<owned_head_t> head;
    bool
    operator==(const source_binding_t &) const = default;
  };

  struct resolution_result_t {
    monitor_id_t monitor;
    binding_state_e state { binding_state_e::unavailable };
    std::optional<source_binding_t> binding;
    std::optional<error_t> error;
    bool
    operator==(const resolution_result_t &) const = default;
  };

  std::vector<resolution_result_t>
  resolve_profile(const profile_t &profile, const inventory_t &inventory);

  result_t<void>
  validate_profile(const profile_t &profile, const inventory_t &inventory,
    topology_revision_t base, topology_revision_t current);

  struct requested_topology_t {
    owner_id_t owner;
    request_id_t request;
    topology_revision_t base;
    inventory_revision_t inventory;
    profile_t profile;
    bool
    operator==(const requested_topology_t &) const = default;
  };

  enum class applied_state_e { applied,
    adjusted,
    unavailable,
    conflict };

  struct applied_monitor_t {
    monitor_id_t monitor;
    applied_state_e state { applied_state_e::unavailable };
    std::optional<source_binding_t> source;
    std::optional<layout_t> layout;
    std::optional<bool> host_primary;
    std::string reason;
    bool
    operator==(const applied_monitor_t &) const = default;
  };

  struct applied_topology_t {
    owner_id_t owner;
    request_id_t request;
    topology_revision_t base;
    topology_revision_t revision;
    std::vector<applied_monitor_t> monitors;
    bool
    operator==(const applied_topology_t &) const = default;
  };

  result_t<void>
  validate_applied(const requested_topology_t &requested, const applied_topology_t &applied);

  struct request_receipt_t {
    std::uint64_t sequence {};
    requested_topology_t requested;
    std::optional<applied_topology_t> applied;
    bool
    operator==(const request_receipt_t &) const = default;
  };

  class request_history_t {
  public:
    explicit request_history_t(std::size_t limit):
        limit_(limit) {}

    result_t<request_receipt_t>
    remember(const requested_topology_t &request, topology_revision_t current);
    result_t<request_receipt_t>
    complete(const applied_topology_t &applied);

  private:
    std::size_t limit_;
    std::map<std::pair<owner_id_t, request_id_t>, request_receipt_t> records_;
  };

  result_t<profile_t>
  synthesize_legacy(const legacy_intent_t &settings, profile_id_t profile,
    monitor_id_t monitor, head_guid_t head, owner_id_t owner,
    adapter_luid_t adapter, const limits_t &limits);

}  // namespace display_device::topology
