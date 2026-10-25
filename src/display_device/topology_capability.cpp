/**
 * @file src/display_device/topology_capability.cpp
 * @brief Per-kind operation table and capability checks.
 */
#include "topology_capability.h"

#include <string>

namespace display_device::topology {

  std::string_view
  to_string(command_kind_e kind) noexcept {
    switch (kind) {
      case command_kind_e::startup_recovery:
        return "startup_recovery";
      case command_kind_e::launch_prepare:
        return "launch_prepare";
      case command_kind_e::launch_failed_restore:
        return "launch_failed_restore";
      case command_kind_e::reconfigure:
        return "reconfigure";
      case command_kind_e::stream_restore:
        return "stream_restore";
      case command_kind_e::shutdown_restore:
        return "shutdown_restore";
      case command_kind_e::shutdown_vdd_destroy:
        return "shutdown_vdd_destroy";
      case command_kind_e::rotate:
        return "rotate";
      case command_kind_e::scale:
        return "scale";
      case command_kind_e::vdd_toggle:
        return "vdd_toggle";
      case command_kind_e::vdd_toggle_followup:
        return "vdd_toggle_followup";
      case command_kind_e::vdd_create:
        return "vdd_create";
      case command_kind_e::vdd_destroy:
        return "vdd_destroy";
      case command_kind_e::reset_persistence:
        return "reset_persistence";
      case command_kind_e::legacy_retry_tick:
        return "legacy_retry_tick";
      case command_kind_e::legacy_unlock_restore:
        return "legacy_unlock_restore";
      case command_kind_e::native_restore:
        return "native_restore";
    }
    return "unknown";
  }

  std::string_view
  to_string(operation_e operation) noexcept {
    switch (operation) {
      case operation_e::configure_display:
        return "configure_display";
      case operation_e::restore_state:
        return "restore_state";
      case operation_e::reset_persistence:
        return "reset_persistence";
      case operation_e::vdd_create:
        return "vdd_create";
      case operation_e::vdd_destroy:
        return "vdd_destroy";
      case operation_e::vdd_extended_mode:
        return "vdd_extended_mode";
      case operation_e::rotate:
        return "rotate";
      case operation_e::scale:
        return "scale";
      case operation_e::legacy_deferred:
        return "legacy_deferred";
      case operation_e::native_apply:
        return "native_apply";
      case operation_e::native_restore:
        return "native_restore";
      case operation_e::native_recover:
        return "native_recover";
    }
    return "unknown";
  }

  namespace {
    bool
    table_allows(command_kind_e kind, operation_e operation) noexcept {
      using enum operation_e;
      switch (kind) {
        case command_kind_e::startup_recovery:
          return operation == native_recover || operation == restore_state;
        case command_kind_e::launch_prepare:
          return operation == configure_display || operation == restore_state || operation == native_apply;
        case command_kind_e::launch_failed_restore:
        case command_kind_e::stream_restore:
        case command_kind_e::shutdown_restore:
          return operation == restore_state || operation == native_restore;
        case command_kind_e::reconfigure:
          return operation == configure_display;
        case command_kind_e::shutdown_vdd_destroy:
          return operation == vdd_destroy;
        case command_kind_e::rotate:
          return operation == rotate;
        case command_kind_e::scale:
          return operation == scale;
        case command_kind_e::vdd_toggle:
          return operation == vdd_create || operation == vdd_destroy;
        case command_kind_e::vdd_toggle_followup:
          return operation == vdd_extended_mode || operation == vdd_destroy;
        case command_kind_e::vdd_create:
          return operation == vdd_create;
        case command_kind_e::vdd_destroy:
          return operation == vdd_destroy;
        case command_kind_e::reset_persistence:
          return operation == reset_persistence;
        case command_kind_e::legacy_retry_tick:
        case command_kind_e::legacy_unlock_restore:
          return operation == legacy_deferred;
        case command_kind_e::native_restore:
          return operation == native_restore;
      }
      return false;
    }

    bool
    legacy_whole_state(operation_e operation) noexcept {
      return operation == operation_e::configure_display || operation == operation_e::restore_state;
    }
  }  // namespace

  bool
  kind_allows(command_kind_e kind, operation_e operation, bool managed_mode) noexcept {
    if (!table_allows(kind, operation)) {
      return false;
    }
    if (managed_mode && legacy_whole_state(operation) && kind != command_kind_e::startup_recovery) {
      return false;
    }
    return true;
  }

  bool
  is_cleanup_kind(command_kind_e kind) noexcept {
    switch (kind) {
      case command_kind_e::launch_failed_restore:
      case command_kind_e::stream_restore:
      case command_kind_e::shutdown_restore:
      case command_kind_e::shutdown_vdd_destroy:
      case command_kind_e::native_restore:
        return true;
      default:
        return false;
    }
  }

  mutation_capability_t::mutation_capability_t(command_kind_e kind, bool managed_mode, std::thread::id worker) noexcept:
      kind_ { kind },
      managed_mode_ { managed_mode },
      worker_ { worker } {}

  command_kind_e
  mutation_capability_t::kind() const noexcept {
    return kind_;
  }

  bool
  mutation_capability_t::managed_mode() const noexcept {
    return managed_mode_;
  }

  void
  mutation_capability_t::require(operation_e operation) const {
    if (std::this_thread::get_id() != worker_) {
      throw capability_error_t { std::string { "Topology operation " } + std::string { to_string(operation) } +
                                 " used outside the coordinator worker" };
    }
    if (!kind_allows(kind_, operation, managed_mode_)) {
      throw capability_error_t { std::string { "Topology operation " } + std::string { to_string(operation) } +
                                 " is not allowed for command " + std::string { to_string(kind_) } +
                                 (managed_mode_ ? " in managed mode" : "") };
    }
  }

}  // namespace display_device::topology
