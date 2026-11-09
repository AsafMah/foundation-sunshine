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

#include <functional>
#include <optional>
#include <string>
#include <vector>

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

  /**
   * What SetDisplayConfig(SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_VIRTUAL_MODE_AWARE) needs of the arrays, or nullopt:
   * each active path's source and target mode indices (the virtual-mode-aware 16-bit fields) in range and of the
   * right type, and no two different active sources overlapping on the desktop.
   */
  std::optional<std::string>
  ccd_problem(const std::vector<DISPLAYCONFIG_PATH_INFO> &paths, const std::vector<DISPLAYCONFIG_MODE_INFO> &modes);

  /** One line per path: target, adapter, source, active, corner and size, and the id `identify` gives it. */
  std::string
  describe_ccd(const std::vector<DISPLAYCONFIG_PATH_INFO> &paths, const std::vector<DISPLAYCONFIG_MODE_INFO> &modes,
    const std::function<std::string(const DISPLAYCONFIG_PATH_INFO &)> &identify);

  /**
   * Plans `arrange` on the active paths and modes of the desktop, in place: drops the `off` displays' paths and moves
   * each placed display's source corner. `identify` gives the id the inventory knows a path by, or "" when it cannot
   * identify it. Returns why nothing may be sent, or nullopt when the arrays are ready for SetDisplayConfig.
   */
  std::optional<std::string>
  plan_arrange(std::vector<DISPLAYCONFIG_PATH_INFO> &paths, std::vector<DISPLAYCONFIG_MODE_INFO> &modes,
    const std::function<std::string(const DISPLAYCONFIG_PATH_INFO &)> &identify, const std::vector<placement_t> &placements,
    const std::vector<os_display_id_t> &off);

  /**
   * The display count ZakoVDD reads: `parse_vdd_display_count` of `<VDDPATH>\vdd_settings.xml`, where VDDPATH is the
   * driver's own registry value (HKLM\SOFTWARE\ZakoTech\ZakoDisplayAdapter). nullopt when either cannot be read.
   */
  std::optional<std::uint32_t>
  read_zako_display_count();

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

    /** Plans `arrange` on the live desktop and asks Windows to validate it (SDC_VALIDATE): "valid", or why not. Writes nothing. */
    std::string
    validate_arrange(const std::vector<placement_t> &placements);

    /** SetupAPI over every monitor devnode (present or not) and the raw CCD paths; reads only. */
    missing_head_report_t
    describe_missing_head(const head_guid_t &intent) override;

    void
    sleep(std::chrono::milliseconds duration) override;
  };
}  // namespace display_device::topology::win32_edge
