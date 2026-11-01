#pragma once

#include "src/display_device/vdd_command_buffer.h"

#include <optional>
#include <string>

namespace vdd_transport_test {
  enum class outcome_e { success,
    missing,
    open_failed,
    write_failed };

  struct state_t {
    outcome_e outcome { outcome_e::success };
    int enumerations {};
    int opens {};
    int writes {};
    int closes {};
    int list_destroys {};
    std::wstring payload;
    std::uint32_t payload_bytes {};
    /** DEVPKEY_Device_DriverVersion of the control interface's device; nullopt = unreadable. */
    std::optional<std::wstring> driver_version { L"100.0.18.2" };
    int version_reads {};
  };

  extern state_t state;

  display_device::vdd_ioctl::result
  send(const display_device::vdd_ioctl::detail::encoded_command_t &command);

  display_device::vdd_ioctl::gated_send_t
  send_gated(const display_device::vdd_ioctl::detail::encoded_command_t &command, const display_device::vdd_ioctl::driver_gate_t &gate);
}  // namespace vdd_transport_test
