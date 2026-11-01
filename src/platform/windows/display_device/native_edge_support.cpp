/**
 * @file src/platform/windows/display_device/native_edge_support.cpp
 * @brief Pure conversions used by the Win32 native edge.
 */
#include "native_edge.h"

#include <algorithm>
#include <cwchar>

namespace display_device::topology::win32_edge {

  uuid_bytes_t
  container_bytes(const GUID &guid) {
    uuid_bytes_t bytes {};
    for (int i = 0; i < 4; ++i) bytes[i] = static_cast<std::uint8_t>(guid.Data1 >> (24 - 8 * i));
    bytes[4] = static_cast<std::uint8_t>(guid.Data2 >> 8);
    bytes[5] = static_cast<std::uint8_t>(guid.Data2);
    bytes[6] = static_cast<std::uint8_t>(guid.Data3 >> 8);
    bytes[7] = static_cast<std::uint8_t>(guid.Data3);
    for (int i = 0; i < 8; ++i) bytes[8 + i] = guid.Data4[i];
    return bytes;
  }

  std::uint64_t
  filetime_ticks(const FILETIME &time) {
    return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
  }

  std::optional<rotation_e>
  rotation_from(DISPLAYCONFIG_ROTATION rotation) {
    switch (rotation) {
      case DISPLAYCONFIG_ROTATION_IDENTITY:
        return rotation_e::identity;
      case DISPLAYCONFIG_ROTATION_ROTATE90:
        return rotation_e::clockwise90;
      case DISPLAYCONFIG_ROTATION_ROTATE180:
        return rotation_e::clockwise180;
      case DISPLAYCONFIG_ROTATION_ROTATE270:
        return rotation_e::clockwise270;
      default:
        return std::nullopt;
    }
  }

  head_class_e
  classify_parent_hardware_ids(const std::optional<std::wstring> &multi_sz) {
    if (!multi_sz) return head_class_e::unknown;
    const std::wstring &list = *multi_sz;
    for (std::size_t offset = 0; offset < list.size() && list[offset] != L'\0';) {
      const auto end = std::min(list.find(L'\0', offset), list.size());
      const std::wstring entry = list.substr(offset, end - offset);
      if (_wcsicmp(entry.c_str(), L"ROOT\\ZAKOVDD") == 0) return head_class_e::driver_head;
      offset = end + 1;
    }
    return head_class_e::physical;
  }
}  // namespace display_device::topology::win32_edge
