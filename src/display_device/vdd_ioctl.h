/**
 * @file vdd_ioctl.h
 * @brief Self-contained IOCTL transport for the ZakoVDD control channel.
 *
 * This is the sole transport for VDD control commands.
 */

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "vdd_mode_list.h"

namespace display_device::topology {
  class mutation_capability_t;
}

namespace display_device::vdd_ioctl {

  /**
   * @brief Outcome of an IOCTL transport attempt.
   *
   * Three-state so callers can distinguish an unavailable device interface
   * from a command rejected by a reachable driver.
   */
  enum class result {
    success,           ///< IOCTL completed with STATUS_SUCCESS.
    interface_missing, ///< No registered device interface (driver too old / not installed).
    failed,            ///< Invalid request, rejected IOCTL, or transport error.
  };

  /**
   * @brief Host-side view of the sealed frame-channel capability probe.
   *
   * Kept separate from `result` because a driver can fully support the command
   * IOCTL transport while still being an older legacy named-texture frame
   * producer.
   */
  enum class frame_channel_status {
    supported,
    unsupported,
    interface_missing,
    failed,
  };

  struct frame_channel_caps {
    std::uint32_t version = 0;
    std::uint32_t flags = 0;
    std::uint32_t max_shared_slots = 0;
    std::uint32_t metadata_size = 0;
  };

  enum class frame_channel_open_status {
    opened,
    not_ready,       ///< Producer is transitioning modes; retry within the caller's deadline.
    unsupported,
    interface_missing,
    failed,
  };

  inline const char *
  frame_channel_open_status_name(frame_channel_open_status status) {
    switch (status) {
      case frame_channel_open_status::opened:
        return "opened";
      case frame_channel_open_status::not_ready:
        return "not_ready";
      case frame_channel_open_status::unsupported:
        return "unsupported";
      case frame_channel_open_status::interface_missing:
        return "interface_missing";
      case frame_channel_open_status::failed:
        return "failed";
    }
    return "unknown";
  }

  struct frame_channel_open_request {
    std::uint32_t monitor_index = 0;
    std::uint32_t required_flags = 0;
    std::uint32_t desired_slots = 0;
    std::uint32_t adapter_luid_low_part = 0;
    std::int32_t adapter_luid_high_part = 0;
  };

  struct frame_channel_slot_handle {
    std::uint64_t texture_handle = 0;
  };

  struct frame_channel_open_response {
    std::uint32_t version = 0;
    std::uint32_t flags = 0;
    std::uint32_t slot_count = 0;
    std::uint32_t metadata_size = 0;
    std::uint64_t metadata_handle = 0;
    std::uint64_t frame_ready_event_handle = 0;
    std::vector<frame_channel_slot_handle> slots;
  };

  std::uint32_t required_sealed_frame_channel_flags();

  struct adapter_status_t {
    bool present = false;
    bool problem_code_valid = false;
    std::uint32_t problem_code = 0;
  };

  /**
   * @brief Query the ZakoVDD Plug and Play node without opening its control interface.
   */
  adapter_status_t query_adapter_status();

  /**
   * @brief Check whether the ZakoVDD display adapter is present in Plug and Play.
   *
   * Unlike `ping()`, this also recognizes an installed adapter whose control
   * IOCTL interface is unavailable.
   */
  bool adapter_present();

  struct create_monitor_t {
    std::string guid;
    float max_nits { 1000.0f };
    float min_nits { 0.001f };
    float max_full_nits { 1000.0f };
    float width_cm {};
    float height_cm {};
  };

  struct destroy_monitors_t {};

  struct set_modes_t {
    std::vector<vdd_mode_list::vdd_mode_t> modes;
  };

  struct hardware_cursor_t {
    bool enabled {};
  };

  using command_t = std::variant<std::monostate, create_monitor_t, destroy_monitors_t, set_modes_t, hardware_cursor_t>;

  /**
   * @brief Authorize, validate and encode a closed mutation request before opening the driver.
   *
   * Performs `SetupDiGetClassDevsW` on `GUID_DEVINTERFACE_ZAKO_VDD_CONTROL`,
   * opens the first interface instance with `CreateFileW`, and issues
   * `IOCTL_VDD_COMMAND`.
   *
   * @param capability The original coordinator-issued worker authority.
   * @param command A supported typed request. Arbitrary command strings are not accepted.
   * Authority failures throw before payload validation or device access.
   * Invalid payloads are logged and fail without opening or writing the device.
   * @return tri-state result; see `enum class result`.
   */
  result
  send_command(const topology::mutation_capability_t &capability, const command_t &command);

  /** What a gated send established about the driver receiving the command. */
  enum class delivery_e {
    delivered,  ///< The driver completed the command.
    not_sent,   ///< Nothing reached the driver: invalid request, no interface, gate refusal or open failure.
    uncertain,  ///< The IOCTL was issued and failed; the driver may have acted on it.
  };

  struct gated_send_t {
    delivery_e delivery { delivery_e::not_sent };
    std::string detail;
  };

  /**
   * Refusal reason for the driver version of the control interface about to be opened, or
   * nullopt to send. The version is `DEVPKEY_Device_DriverVersion` of the interface's own
   * device, and nullopt when it cannot be read.
   */
  using driver_gate_t = std::function<std::optional<std::string>(const std::optional<std::string> &version)>;

  /**
   * @brief `send_command`, but only to a driver `gate` accepts, reporting delivery rather than a result.
   *
   * Authority failures throw exactly as in `send_command`. Nothing is retried.
   */
  gated_send_t
  send_command_gated(const topology::mutation_capability_t &capability, const command_t &command, const driver_gate_t &gate);

  /**
   * @brief Cheap liveness probe used to decide whether the IOCTL transport
   *        is available at all without paying the cost of a full command.
   *
   * @return `true` if `IOCTL_VDD_PING` round-trips successfully.
   */
  bool ping();

  /**
   * @brief Probe whether the installed driver supports the v2 sealed frame
   *        channel negotiation IOCTL.
   *
   * `unsupported` means the control device is present but the driver does not
   * implement `IOCTL_VDD_QUERY_FRAME_CHANNEL_CAPS`; callers should keep using
   * the hardened legacy named shared-texture path.
   */
  frame_channel_status query_frame_channel_caps(frame_channel_caps &caps);

  /**
   * @brief Ask a v2 driver to duplicate an unnamed producer-owned frame
   *        channel into this Sunshine process.
   *
   * This is the host side of sealed borrow. Older drivers return
   * `unsupported`, and callers must not treat `query_frame_channel_caps()`
   * support as proof that this open path is usable until this call succeeds.
   */
  frame_channel_open_status open_frame_channel(const frame_channel_open_request &request,
                                               frame_channel_open_response &response,
                                               bool log_failures = true);

}  // namespace display_device::vdd_ioctl
