/**
 * @file src/display_device/topology_windows_native.h
 * @brief The P1 Windows `native_platform_t`, written against a narrow OS/driver edge.
 *
 * The edge (`native_edge_t`) is the only part that touches CCD, SetupAPI or the VDD
 * control IOCTL; everything here is portable so it can be tested with a fake edge.
 *
 * Head identity: ZakoVDD (pinned tags v0.15.10 and v0.18.2) sets
 * `IDDCX_MONITOR_INFO.MonitorContainerId` to the client GUID of `CREATEMONITOR`, so the
 * monitor devnode's ContainerId is read back as the head GUID. The devnode's last
 * arrival time is its incarnation: a head recreated with the same GUID arrives again.
 * A driver head that lacks either cannot be proven, and a classification the edge cannot
 * establish makes the whole inventory unavailable.
 *
 * `DESTROYMONITOR` removes every head. Inside Sunshine, managed mode denies every legacy
 * VDD command kind (see `table_allows`), so only external tools can race it; the executor
 * proves a sole matching head before removing anything.
 */
#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "topology_native.h"

namespace display_device::topology {

  /** A four-part numeric driver version (`DriverVer`), compared part by part. */
  struct driver_version_t {
    std::array<std::uint32_t, 4> parts {};

    auto
    operator<=>(const driver_version_t &) const = default;
  };

  /** Exactly four dot-separated decimal parts, each at most 65535; anything else is nullopt. */
  std::optional<driver_version_t>
  parse_driver_version(std::string_view text);

  /** `{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}`, bytes in textual order, upper case. */
  std::string
  format_guid(const uuid_bytes_t &bytes);

  /** The braced textual form, case-insensitive; bytes are kept in textual order. */
  std::optional<uuid_bytes_t>
  parse_guid(std::string_view text);

  enum class edge_delivery_e : std::uint8_t {
    /** The driver completed the command. */
    delivered,
    /** Nothing reached the driver (bad request, gate refusal, no interface, open failure). */
    not_sent,
    /** The command may have reached the driver; its effect is unknown. */
    uncertain,
  };

  struct edge_send_t {
    edge_delivery_e delivery { edge_delivery_e::not_sent };
    std::string detail;
  };

  enum class head_class_e : std::uint8_t {
    /** The edge could not tell whether this display belongs to the virtual display driver. */
    unknown,
    physical,
    driver_head,
  };

  struct edge_display_t {
    os_display_id_t id;
    device_info_t info {};
    std::optional<layout_t> layout;
    std::optional<adapter_luid_t> adapter;
    head_class_e kind { head_class_e::unknown };
    /** The monitor devnode's ContainerId, bytes in textual GUID order. */
    std::optional<uuid_bytes_t> container;
    /** The monitor devnode's last arrival time; 0 when unknown. */
    std::uint64_t arrival {};
  };

  struct edge_snapshot_t {
    bool available { false };
    std::vector<edge_display_t> displays;
  };

  /**
   * Refusal reason for the driver version read from the control interface the command is
   * about to be sent to, or nullopt to send. The version is nullopt when it cannot be read.
   */
  using version_gate_t = std::function<std::optional<std::string>(const std::optional<std::string> &version)>;

  struct edge_mode_t {
    unsigned width {};
    unsigned height {};
    unsigned refresh_hz {};

    bool
    operator==(const edge_mode_t &) const = default;
  };

  /** The driver-wide mode list keeps at most this many modes (`vdd_mode_list::kMaxTotalModes`). */
  inline constexpr std::size_t max_driver_modes = 96;

  class native_edge_t {
  public:
    virtual ~native_edge_t() = default;

    virtual edge_snapshot_t
    snapshot() = 0;

    /** `CREATEMONITOR` with `intent` as the client GUID, only if `gate` accepts the driver. */
    virtual edge_send_t
    send_create(const mutation_capability_t &capability, const head_guid_t &intent, const version_gate_t &gate) = 0;

    /** `DESTROYMONITOR`: the driver removes every head. */
    virtual edge_send_t
    send_destroy_all(const mutation_capability_t &capability) = 0;

    /** The modes the OS lists for this display, or nullopt when they cannot be read. */
    virtual std::optional<std::vector<edge_mode_t>>
    list_modes(const std::string &display_name) = 0;

    /** Applies `mode` to one display through the OS. */
    virtual edge_send_t
    set_mode(const mutation_capability_t &capability, const os_display_id_t &display, const display_mode_t &mode) = 0;

    /**
     * `SETMODES`: replaces the driver-wide mode list (the first mode is preferred), only if
     * `gate` accepts the driver. The driver may recreate its heads with the same GUIDs.
     */
    virtual edge_send_t
    send_set_modes(const mutation_capability_t &capability, const std::vector<edge_mode_t> &modes, const version_gate_t &gate) = 0;

    /**
     * Adds one display that is listed but not on the desktop as its own extended display,
     * keeping every other display's topology group. Delivered means the OS accepted and
     * verified the new topology; uncertain means it may have been applied.
     */
    virtual edge_send_t
    activate(const mutation_capability_t &capability, const os_display_id_t &display) = 0;

    virtual std::chrono::steady_clock::time_point
    now() = 0;

    virtual void
    sleep(std::chrono::milliseconds duration) = 0;
  };

  /**
   * Refusal reason when a ZakoVDD control interface with this DriverVer may not carry the
   * CREATEMONITOR client GUID as the monitor's ContainerId, or nullopt to accept it.
   * Every release tag from v0.1.4 sets MonitorContainerId to the client GUID. Accepted:
   *  - main line, tag vA.B.C = DriverVer 100.A.B.C, from 100.0.1.4;
   *  - Win10 line, tag v0.15.P = DriverVer 15.0.15.P.
   * Branch/PR artifacts (15.0.0.N), CI dev builds (99.x) and unknown schemes are refused.
   */
  std::optional<std::string>
  container_driver_refusal(const driver_version_t &version);

  /**
   * The identity journaled for ZakoVDD heads. It names the driver, not a device instance,
   * so it must never change: a journal written by one run is matched by the next.
   */
  inline constexpr driver_instance_id_t zako_vdd_driver {
    uuid_bytes_t { 0x5A, 0x4B, 0x0D, 0x1E, 0x7C, 0x3F, 0x4E, 0x2A, 0x9B, 0x61, 0x2D, 0x8F, 0x0C, 0x4A, 0x7E, 0x93 }
  };

  struct windows_native_options_t {
    driver_instance_id_t driver;
    /** Friendly name of the driver's heads (`ZAKO_NAME`). */
    std::string virtual_name;
    std::chrono::milliseconds wait { 5000 };
    std::chrono::milliseconds poll { 100 };
  };

  class windows_native_platform_t final: public native_platform_t {
  public:
    windows_native_platform_t(native_edge_t &edge, windows_native_options_t options);

    inventory_t
    inventory() override;

    /** By friendly name only, so a journaled inventory classifies the same way it did live. */
    bool
    is_virtual_head(const observed_display_t &display) const override;

  private:
    result_t<creation_delivery_e>
    do_create_head(const mutation_capability_t &capability, const head_guid_t &intent) override;

    result_t<void>
    do_remove_all_heads(const mutation_capability_t &capability) override;

    result_t<void>
    do_apply_fields(const mutation_capability_t &capability, const os_display_id_t &display, const managed_values_t &values) override;

    /**
     * Lists the mode with the driver first (`SETMODES`) when the OS does not offer it yet,
     * then applies it to the head with the same GUID, which may have been recreated. A
     * recreated head that comes back off the desktop is added to it again first.
     */
    result_t<void>
    do_change_head_mode(const mutation_capability_t &capability, const observed_display_t &head, const display_mode_t &mode) override;

    result_t<void>
    do_activate_head(const mutation_capability_t &capability, const observed_display_t &head) override;

    /** Adds the head with `guid` to the desktop unless it is already on it, and waits for it there. */
    result_t<void>
    ensure_active(const mutation_capability_t &capability, const head_guid_t &guid);

    /** Polls until `done` holds for an available inventory or the wait ends; returns whether it held. */
    bool
    wait_for(const std::function<bool(const inventory_t &)> &done);

    native_edge_t &edge_;
    windows_native_options_t options_;
    std::uint64_t revision_ {};
  };
}  // namespace display_device::topology
