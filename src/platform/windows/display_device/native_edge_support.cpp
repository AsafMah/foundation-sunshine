/**
 * @file src/platform/windows/display_device/native_edge_support.cpp
 * @brief Pure conversions used by the Win32 native edge.
 */
#include "native_edge.h"

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <functional>

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

namespace display_device::topology::win32_edge {

  namespace {
    std::string
    adapter_text(const LUID &luid) {
      char text[24];
      std::snprintf(text, sizeof(text), "%lX:%lX", static_cast<unsigned long>(luid.HighPart), static_cast<unsigned long>(luid.LowPart));
      return text;
    }

    bool
    same_luid(const LUID &a, const LUID &b) {
      return a.HighPart == b.HighPart && a.LowPart == b.LowPart;
    }

    constexpr UINT32 invalid_index16 = 0xffff;
  }  // namespace

  std::optional<std::string>
  ccd_problem(const std::vector<DISPLAYCONFIG_PATH_INFO> &paths, const std::vector<DISPLAYCONFIG_MODE_INFO> &modes) {
    struct rect_t {
      LONG left, top, right, bottom;
      std::size_t path;
      UINT32 source_index;
    };
    std::vector<rect_t> sources;
    for (std::size_t i = 0; i < paths.size(); ++i) {
      const auto &path = paths[i];
      if (!(path.flags & DISPLAYCONFIG_PATH_ACTIVE)) continue;
      const UINT32 source_index = path.sourceInfo.sourceModeInfoIdx;
      if (source_index == invalid_index16) return "active path " + std::to_string(i) + " has no source mode";
      if (source_index >= modes.size()) return "active path " + std::to_string(i) + " names source mode " + std::to_string(source_index) + " of " + std::to_string(modes.size());
      const auto &source = modes[source_index];
      if (source.infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE || source.id != path.sourceInfo.id || !same_luid(source.adapterId, path.sourceInfo.adapterId)) {
        return "active path " + std::to_string(i) + "'s source mode " + std::to_string(source_index) + " is not its source";
      }
      const UINT32 target_index = path.targetInfo.targetModeInfoIdx;
      if (target_index != invalid_index16 &&
          (target_index >= modes.size() || modes[target_index].infoType != DISPLAYCONFIG_MODE_INFO_TYPE_TARGET || modes[target_index].id != path.targetInfo.id)) {
        return "active path " + std::to_string(i) + "'s target mode " + std::to_string(target_index) + " is not its target";
      }
      const UINT32 desktop_index = path.targetInfo.desktopModeInfoIdx;
      if (desktop_index != invalid_index16 && (desktop_index >= modes.size() || modes[desktop_index].infoType != DISPLAYCONFIG_MODE_INFO_TYPE_DESKTOP_IMAGE)) {
        return "active path " + std::to_string(i) + "'s desktop image mode " + std::to_string(desktop_index) + " is not a desktop image";
      }
      const auto &mode = source.sourceMode;
      sources.push_back({ mode.position.x, mode.position.y, mode.position.x + static_cast<LONG>(mode.width), mode.position.y + static_cast<LONG>(mode.height), i, source_index });
    }
    for (std::size_t a = 0; a < sources.size(); ++a) {
      for (std::size_t b = a + 1; b < sources.size(); ++b) {
        // A clone shares its source mode; that is one desktop region, not an overlap.
        if (sources[a].source_index == sources[b].source_index) continue;
        if (sources[a].left < sources[b].right && sources[b].left < sources[a].right && sources[a].top < sources[b].bottom && sources[b].top < sources[a].bottom) {
          return "active paths " + std::to_string(sources[a].path) + " and " + std::to_string(sources[b].path) + " overlap on the desktop";
        }
      }
    }
    return std::nullopt;
  }

  std::string
  describe_ccd(const std::vector<DISPLAYCONFIG_PATH_INFO> &paths, const std::vector<DISPLAYCONFIG_MODE_INFO> &modes,
    const std::function<std::string(const DISPLAYCONFIG_PATH_INFO &)> &identify) {
    std::string text;
    for (std::size_t i = 0; i < paths.size(); ++i) {
      const auto &path = paths[i];
      text += "[path " + std::to_string(i) + ": target " + std::to_string(path.targetInfo.id) + " adapter " + adapter_text(path.targetInfo.adapterId) +
              " source " + std::to_string(path.sourceInfo.id) + (path.flags & DISPLAYCONFIG_PATH_ACTIVE ? " active" : " inactive");
      const UINT32 index = path.sourceInfo.sourceModeInfoIdx;
      if (index != invalid_index16 && index < modes.size() && modes[index].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
        const auto &mode = modes[index].sourceMode;
        text += " at " + std::to_string(mode.position.x) + "," + std::to_string(mode.position.y) + " " + std::to_string(mode.width) + "x" + std::to_string(mode.height);
      }
      const auto id = identify(path);
      text += id.empty() ? std::string { " unidentified]" } : " id " + id + "]";
    }
    return text;
  }

  std::optional<std::string>
  plan_arrange(std::vector<DISPLAYCONFIG_PATH_INFO> &paths, std::vector<DISPLAYCONFIG_MODE_INFO> &modes,
    const std::function<std::string(const DISPLAYCONFIG_PATH_INFO &)> &identify, const std::vector<placement_t> &placements,
    const std::vector<os_display_id_t> &off) {
    // A display taken off the desktop is left out of the supplied active paths, in the same write.
    std::erase_if(paths, [&](const DISPLAYCONFIG_PATH_INFO &path) {
      const auto id = identify(path);
      return !id.empty() && std::ranges::any_of(off, [&](const os_display_id_t &display) { return display.value == id; });
    });
    if (paths.empty()) return "no display would be left on the desktop";
    // The arrays are supplied whole, so a path the inventory cannot see would be sent unchanged, wherever it is
    // (gh-all run 20261109-174516: a head whose monitor devnode was disabled stayed active at the origin).
    for (const auto &path : paths) {
      if ((path.flags & DISPLAYCONFIG_PATH_ACTIVE) && identify(path).empty()) {
        return "an active display path the inventory cannot identify is on the desktop (target " + std::to_string(path.targetInfo.id) +
               " on adapter " + adapter_text(path.targetInfo.adapterId) + "; a monitor whose device is disabled has no device path), so nothing was written: " +
               describe_ccd(paths, modes, identify);
      }
    }

    std::vector<bool> placed(placements.size(), false);
    std::vector<UINT32> moved;
    for (auto &path : paths) {
      const auto id = identify(path);
      if (id.empty()) continue;
      const auto it = std::ranges::find_if(placements, [&](const placement_t &placement) { return placement.display.value == id; });
      if (it == placements.end()) continue;
      const UINT32 index = path.sourceInfo.sourceModeInfoIdx;
      if (index == invalid_index16 || index >= modes.size() || modes[index].infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
        return "display " + id + " has no source mode";
      }
      // A duplicated display shares its source; moving it would move its clone too.
      if (std::ranges::find(moved, index) != moved.end()) return "display " + id + " shares its source with another display";
      moved.push_back(index);
      modes[index].sourceMode.position = POINTL { it->position.x, it->position.y };
      placed[static_cast<std::size_t>(it - placements.begin())] = true;
    }
    for (std::size_t i = 0; i < placements.size(); ++i) {
      if (!placed[i]) return "display " + placements[i].display.value + " is not on the desktop";
    }
    if (const auto problem = ccd_problem(paths, modes)) {
      return "the planned layout is not valid (" + *problem + "), so nothing was written: " + describe_ccd(paths, modes, identify);
    }
    return std::nullopt;
  }
}  // namespace display_device::topology::win32_edge
