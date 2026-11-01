#pragma once

#include "topology_capability.h"
#include "vdd_ioctl.h"

#include <utility>

namespace display_device::vdd_ioctl::detail {
  /** Synchronous transport envelope, bound to the original worker authority. */
  class encoded_command_t {
  public:
    encoded_command_t(const encoded_command_t &) = delete;
    encoded_command_t &
    operator=(const encoded_command_t &) = delete;
    encoded_command_t(encoded_command_t &&) = delete;
    encoded_command_t &
    operator=(encoded_command_t &&) = delete;

    const std::wstring &
    authorized_bytes() const {
      capability_.require(operation_);
      return bytes_;
    }

  private:
    friend result
    vdd_ioctl::send_command(const topology::mutation_capability_t &, const command_t &);

    encoded_command_t(const topology::mutation_capability_t &capability,
      topology::operation_e operation, std::wstring bytes):
        capability_ { capability }, operation_ { operation }, bytes_ { std::move(bytes) } {}

    const topology::mutation_capability_t &capability_;
    const topology::operation_e operation_;
    const std::wstring bytes_;
  };

  result
  send_command_buffer(const encoded_command_t &command);
}  // namespace display_device::vdd_ioctl::detail
