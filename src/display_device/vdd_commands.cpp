#include "vdd_command_buffer.h"

#include <cmath>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <type_traits>

#include "src/logging_severity.h"

namespace display_device::vdd_ioctl {
  namespace {
    topology::operation_e
    operation_for(const command_t &request) {
      return std::visit([](const auto &value) -> topology::operation_e {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, create_monitor_t>)
          return topology::operation_e::vdd_create;
        else if constexpr (std::is_same_v<T, destroy_monitors_t>)
          return topology::operation_e::vdd_destroy;
        else if constexpr (std::is_same_v<T, set_modes_t>)
          return topology::operation_e::vdd_set_modes;
        else if constexpr (std::is_same_v<T, hardware_cursor_t>)
          return topology::operation_e::vdd_cursor;
        else
          throw std::invalid_argument { "Missing VDD mutation request" };
      },
        request);
    }

    bool
    valid_guid(const std::string &guid) {
      if (guid.size() != 38 || guid.front() != '{' || guid.back() != '}') return false;
      for (std::size_t i = 1; i < 37; ++i) {
        const auto c = guid[i];
        if (i == 9 || i == 14 || i == 19 || i == 24) {
          if (c != '-') return false;
        }
        else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
          return false;
        }
      }
      return true;
    }

    std::wstring
    encode_create(const create_monitor_t &request) {
      if (request.guid.empty()) return L"CREATEMONITOR";
      if (!valid_guid(request.guid)) throw std::invalid_argument { "Invalid VDD client GUID" };
      for (const auto value : { request.max_nits, request.min_nits, request.max_full_nits, request.width_cm, request.height_cm }) {
        if (!std::isfinite(value)) throw std::invalid_argument { "Non-finite VDD display parameter" };
      }
      std::wostringstream command;
      command.imbue(std::locale::classic());
      command << L"CREATEMONITOR " << std::wstring(request.guid.begin(), request.guid.end())
              << L":[" << request.max_nits << L"," << request.min_nits << L"," << request.max_full_nits << L"]";
      if (request.width_cm > 0.0f && request.height_cm > 0.0f) {
        command << L"[" << request.width_cm << L"," << request.height_cm << L"]";
      }
      return command.str();
    }

    std::wstring
    encode_modes(const set_modes_t &request) {
      if (request.modes.empty() || request.modes.size() > vdd_mode_list::kMaxTotalModes) {
        throw std::invalid_argument { "VDD mode count is outside the driver limit" };
      }
      for (const auto &mode : request.modes) {
        if (!mode.resolution.width || !mode.resolution.height || !mode.refresh_hz) {
          throw std::invalid_argument { "VDD modes require nonzero dimensions and refresh rates" };
        }
      }
      auto command = vdd_mode_list::format_setmodes_command(request.modes);
      if (command.size() > vdd_mode_list::kMaxCommandChars) {
        throw std::invalid_argument { "VDD mode command exceeds the driver buffer" };
      }
      return command;
    }

    std::wstring
    encode(const command_t &request) {
      return std::visit([](const auto &value) -> std::wstring {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, create_monitor_t>)
          return encode_create(value);
        else if constexpr (std::is_same_v<T, destroy_monitors_t>)
          return L"DESTROYMONITOR";
        else if constexpr (std::is_same_v<T, set_modes_t>)
          return encode_modes(value);
        else if constexpr (std::is_same_v<T, hardware_cursor_t>)
          return value.enabled ? L"HARDWARECURSOR true" : L"HARDWARECURSOR false";
        else
          throw std::invalid_argument { "Missing VDD mutation request" };
      },
        request);
    }
  }  // namespace

  result
  send_command(const topology::mutation_capability_t &capability, const command_t &request) {
    std::wstring command;
    topology::operation_e operation;
    try {
      operation = operation_for(request);
      capability.require(operation);
      command = encode(request);
    }
    catch (const std::invalid_argument &error) {
      BOOST_LOG(warning) << "vdd_ioctl: rejecting mutation request: " << error.what();
      return result::failed;
    }
    const detail::encoded_command_t encoded { capability, operation, std::move(command) };
    return detail::send_command_buffer(encoded);
  }
}  // namespace display_device::vdd_ioctl
