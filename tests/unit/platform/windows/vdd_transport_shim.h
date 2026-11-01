#pragma once

#include "src/display_device/vdd_command_buffer.h"

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
  };

  extern state_t state;

  display_device::vdd_ioctl::result
  send(const display_device::vdd_ioctl::detail::encoded_command_t &command);
}  // namespace vdd_transport_test
