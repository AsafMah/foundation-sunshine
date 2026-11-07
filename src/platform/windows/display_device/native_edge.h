/**
 * @file src/platform/windows/display_device/native_edge.h
 * @brief The Win32 `native_edge_t`: CCD for the display set, SetupAPI for monitor devnode
 *        identity, and the version-gated VDD control IOCTL for head creation and removal.
 *
 * Per display path the monitor devnode supplies:
 *  - ContainerId: ZakoVDD sets it to the `CREATEMONITOR` client GUID, so for a driver head it
 *    is the head GUID;
 *  - LastArrivalDate: the incarnation, since a head recreated with the same GUID arrives again;
 *  - the parent devnode's hardware IDs: a parent listing `ROOT\ZakoVDD` makes it a driver head.
 *    The monitor's own EDID vendor is not used: ZakoVDD v0.18.2 only applies its manufacturer
 *    when manufacturer spoofing is not prevented.
 * Any read needed for the classification that fails leaves the display `unknown`, which makes
 * the whole inventory unavailable.
 */
#pragma once

#include <optional>
#include <string>

#include <windows.h>

#include "src/display_device/topology_windows_native.h"

namespace display_device::topology::win32_edge {

  /** GUID fields in textual order: Data1..Data3 big-endian, then Data4. */
  uuid_bytes_t
  container_bytes(const GUID &guid);

  std::uint64_t
  filetime_ticks(const FILETIME &time);

  /** nullopt for a rotation that CCD reports but the topology model cannot express. */
  std::optional<rotation_e>
  rotation_from(DISPLAYCONFIG_ROTATION rotation);

  /**
   * Classifies a monitor by its parent's REG_MULTI_SZ hardware IDs (each entry NUL-terminated,
   * the list ending with an empty entry). nullopt (unreadable) is `unknown`; an entry equal to
   * `ROOT\ZakoVDD`, ignoring case, is `driver_head`; any other readable list is `physical`.
   */
  head_class_e
  classify_parent_hardware_ids(const std::optional<std::wstring> &multi_sz);

  class win32_native_edge_t final: public native_edge_t {
  public:
    edge_snapshot_t
    snapshot() override;

    edge_send_t
    send_create(const mutation_capability_t &capability, const head_guid_t &intent, const version_gate_t &gate) override;

    /** Not version-gated: removal must stay possible on any driver that could have created heads. */
    edge_send_t
    send_destroy_all(const mutation_capability_t &capability) override;

    std::optional<std::vector<edge_mode_t>>
    list_modes(const std::string &display_name) override;

    /** CCD through `set_display_modes`; a refusal is `uncertain`, since CCD may have applied part of it. */
    edge_send_t
    set_mode(const mutation_capability_t &capability, const os_display_id_t &display, const display_mode_t &mode) override;

    edge_send_t
    send_set_modes(const mutation_capability_t &capability, const std::vector<edge_mode_t> &modes, const version_gate_t &gate) override;

    /** CCD through `extend_topology`; a refusal is `uncertain`, since its revert may not have taken. */
    edge_send_t
    activate(const mutation_capability_t &capability, const os_display_id_t &display) override;

    /** One `SetDisplayConfig` of the active paths with moved source corners, not saved to the database. */
    edge_send_t
    arrange(const mutation_capability_t &capability, const std::vector<placement_t> &placements,
      const std::vector<os_display_id_t> &off) override;

    std::chrono::steady_clock::time_point
    now() override;

    void
    sleep(std::chrono::milliseconds duration) override;
  };
}  // namespace display_device::topology::win32_edge
