#pragma once

#include <string_view>

#include "topology.h"

namespace display_device::topology {

  struct managed_values_t {
    std::optional<position_t> position;
    std::optional<display_mode_t> mode;
    std::optional<rotation_e> rotation;
    std::optional<std::uint32_t> scale_percent;
    std::optional<hdr_state_e> hdr;
    std::optional<bool> primary;
    bool
    operator==(const managed_values_t &) const;
  };

  struct managed_display_t {
    source_binding_t source;
    managed_values_t baseline;
    managed_values_t intended;
    std::optional<managed_values_t> confirmed;
    bool
    operator==(const managed_display_t &) const = default;
  };

  enum class journal_operation_e { create_head,
    apply_fields,
    remove_head,
    restore_fields };
  enum class journal_step_state_e { planned,
    attempted,
    observed,
    failed,
    conflict };

  struct journal_step_t {
    std::uint32_t sequence {};
    journal_operation_e operation;
    std::optional<os_display_id_t> display;
    std::optional<head_guid_t> head_intent;
    journal_step_state_e state { journal_step_state_e::planned };
    std::optional<inventory_t> observed;
    std::string detail;
    bool
    operator==(const journal_step_t &) const = default;
  };

  struct managed_audio_t {
    std::string baseline;
    std::string intended;
    std::optional<std::string> confirmed;
    bool
    operator==(const managed_audio_t &) const = default;
  };

  struct journal_record_t {
    std::uint32_t version { 1 };
    owner_id_t owner;
    transaction_id_t transaction;
    request_id_t request;
    topology_revision_t base;
    inventory_t baseline;
    std::vector<managed_display_t> managed;
    std::vector<owned_head_t> owned_heads;
    std::vector<journal_step_t> steps;
    std::optional<managed_audio_t> audio;
    bool
    operator==(const journal_record_t &) const = default;
  };

  result_t<journal_record_t>
  prepare_journal(const std::optional<journal_record_t> &existing,
    owner_id_t owner, transaction_id_t transaction, request_id_t request,
    topology_revision_t base, const inventory_t &baseline);

  result_t<std::string>
  serialize_journal(const journal_record_t &record);
  result_t<journal_record_t>
  deserialize_journal(std::string_view serialized);

}  // namespace display_device::topology
