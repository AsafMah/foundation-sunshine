/**
 * @file src/display_device/topology_capability.h
 * @brief Command kinds, gated operations and the worker-only mutation capability.
 *
 * Every monitor-mutating function takes a `mutation_capability_t`. Only the
 * topology coordinator can construct one, and only on its worker thread while it
 * runs a command body. The capability records the command kind and checks each
 * requested operation against a fixed per-kind table, so a body admitted as one
 * kind (for example a rotation) cannot perform another kind's operation (for
 * example destroying a virtual display).
 */
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace display_device::topology {

  enum class command_kind_e : std::uint8_t {
    startup_recovery,
    launch_prepare,
    launch_failed_restore,
    reconfigure,
    stream_restore,
    shutdown_restore,
    shutdown_vdd_destroy,
    rotate,
    scale,
    vdd_toggle,
    vdd_toggle_followup,
    vdd_create,
    vdd_destroy,
    reset_persistence,
    legacy_retry_tick,
    legacy_unlock_restore,
    native_restore,
  };

  enum class operation_e : std::uint8_t {
    configure_display,
    restore_state,
    reset_persistence,
    vdd_create,
    vdd_destroy,
    vdd_extended_mode,
    rotate,
    scale,
    legacy_deferred,
    native_apply,
    native_restore,
    native_recover,
  };

  std::string_view
  to_string(command_kind_e kind) noexcept;

  std::string_view
  to_string(operation_e operation) noexcept;

  /**
   * @brief Whether bodies of `kind` may perform `operation` in the given mode.
   * @details In managed mode the legacy whole-state operations (`configure_display`,
   *          `restore_state`) are refused for every kind except startup recovery,
   *          which still recovers legacy state persisted by an earlier legacy run.
   */
  bool
  kind_allows(command_kind_e kind, operation_e operation, bool managed_mode) noexcept;

  /**
   * @brief Cleanup kinds are exempt from owner, draining, fence and managed-session checks.
   */
  bool
  is_cleanup_kind(command_kind_e kind) noexcept;

  class capability_error_t: public std::logic_error {
  public:
    using std::logic_error::logic_error;
  };

  class coordinator_t;

  class mutation_capability_t {
  public:
    mutation_capability_t(const mutation_capability_t &) = delete;
    mutation_capability_t &
    operator=(const mutation_capability_t &) = delete;

    command_kind_e
    kind() const noexcept;

    bool
    managed_mode() const noexcept;

    /**
     * @brief Throw `capability_error_t` unless `operation` is allowed for this kind
     *        and the caller is the worker thread that received the capability.
     */
    void
    require(operation_e operation) const;

  private:
    friend class coordinator_t;

    mutation_capability_t(command_kind_e kind, bool managed_mode, std::thread::id worker) noexcept;

    command_kind_e kind_;
    bool managed_mode_;
    std::thread::id worker_;
  };

}  // namespace display_device::topology
