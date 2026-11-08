/**
 * @file src/platform/windows/display_device/native_edge.cpp
 * @brief CCD/SetupAPI/VDD-IOCTL glue for the Win32 native edge.
 */
#include "native_edge.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <thread>
#include <vector>

#include <SetupApi.h>

#include "src/display_device/display_device.h"
#include "src/display_device/vdd_ioctl.h"
#include "src/logging.h"
#include "windows_utils.h"

namespace display_device::topology::win32_edge {

  namespace {

    // From devpkey.h, defined here so no translation unit needs INITGUID storage for them.
    constexpr DEVPROPKEY container_id_key { { 0x8c7ed206, 0x3f8a, 0x4827, { 0xb3, 0xab, 0xae, 0x9e, 0x1f, 0xae, 0xfc, 0x6c } }, 2 };
    constexpr DEVPROPKEY last_arrival_key { { 0x83da6326, 0x97a6, 0x4088, { 0x94, 0x53, 0xa1, 0x92, 0x3f, 0x57, 0x3b, 0x29 } }, 102 };
    constexpr DEVPROPKEY parent_key { { 0x4340a6c5, 0x93fa, 0x4706, { 0x97, 0x2c, 0x7b, 0x64, 0x80, 0x08, 0xa5, 0xa7 } }, 8 };
    constexpr DEVPROPKEY hardware_ids_key { { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 3 };
    constexpr DEVPROPKEY devnode_status_key { { 0x4340a6c5, 0x93fa, 0x4706, { 0x97, 0x2c, 0x7b, 0x64, 0x80, 0x08, 0xa5, 0xa7 } }, 2 };
    constexpr DEVPROPKEY problem_code_key { { 0x4340a6c5, 0x93fa, 0x4706, { 0x97, 0x2c, 0x7b, 0x64, 0x80, 0x08, 0xa5, 0xa7 } }, 3 };
    constexpr DEVPROPKEY is_present_key { { 0x540b947e, 0x8b40, 0x45bc, { 0xa8, 0xa2, 0x6a, 0x0b, 0x89, 0x4c, 0xbd, 0xa2 } }, 5 };
    // GUID_DEVCLASS_MONITOR (devguid.h).
    constexpr GUID monitor_class { 0x4d36e96e, 0xe325, 0x11ce, { 0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18 } };

    class device_list_t {
    public:
      device_list_t():
          handle_ { SetupDiCreateDeviceInfoList(nullptr, nullptr) } {}

      ~device_list_t() {
        if (handle_ != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(handle_);
      }

      device_list_t(const device_list_t &) = delete;
      device_list_t &operator=(const device_list_t &) = delete;

      HDEVINFO
      get() const {
        return handle_;
      }

    private:
      HDEVINFO handle_;
    };

    std::wstring
    widen(const std::string &text) {
      if (text.empty()) return {};
      const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
      if (size <= 0) return {};
      std::wstring wide(static_cast<std::size_t>(size), L'\0');
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), wide.data(), size);
      return wide;
    }

    template <class T>
    std::optional<T>
    fixed_property(HDEVINFO list, SP_DEVINFO_DATA &device, const DEVPROPKEY &key, DEVPROPTYPE expected) {
      T value {};
      DEVPROPTYPE type {};
      DWORD size {};
      if (!SetupDiGetDevicePropertyW(list, &device, &key, &type, reinterpret_cast<PBYTE>(&value), sizeof(value), &size, 0) ||
          type != expected || size != sizeof(value)) {
        return std::nullopt;
      }
      return value;
    }

    /** A string or string-list property as raw wide characters; a missing property is an empty list. */
    std::optional<std::wstring>
    text_property(HDEVINFO list, SP_DEVINFO_DATA &device, const DEVPROPKEY &key, DEVPROPTYPE expected, bool missing_is_empty) {
      DEVPROPTYPE type {};
      DWORD size {};
      if (SetupDiGetDevicePropertyW(list, &device, &key, &type, nullptr, 0, &size, 0) || GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        if (missing_is_empty && GetLastError() == ERROR_NOT_FOUND) return std::wstring {};
        return std::nullopt;
      }
      std::vector<wchar_t> buffer(size / sizeof(wchar_t) + 1, L'\0');
      if (!SetupDiGetDevicePropertyW(list, &device, &key, &type, reinterpret_cast<PBYTE>(buffer.data()), size, &size, 0) || type != expected) {
        return std::nullopt;
      }
      return std::wstring(buffer.data(), size / sizeof(wchar_t));
    }

    /** Fills container, arrival and kind from the monitor devnode behind `monitor_path`. */
    void
    read_devnode(HDEVINFO list, const std::string &monitor_path, edge_display_t &display) {
      const auto path = widen(monitor_path);
      if (path.empty()) return;
      SP_DEVICE_INTERFACE_DATA interface_data {};
      interface_data.cbSize = sizeof(interface_data);
      if (!SetupDiOpenDeviceInterfaceW(list, path.c_str(), 0, &interface_data)) {
        BOOST_LOG(debug) << "native edge: monitor interface " << monitor_path << " could not be opened (err=" << GetLastError() << ")";
        return;
      }
      DWORD required {};
      SetupDiGetDeviceInterfaceDetailW(list, &interface_data, nullptr, 0, &required, nullptr);
      if (required < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) return;
      std::vector<BYTE> detail_buffer(required);
      auto *detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(detail_buffer.data());
      detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
      SP_DEVINFO_DATA monitor {};
      monitor.cbSize = sizeof(monitor);
      if (!SetupDiGetDeviceInterfaceDetailW(list, &interface_data, detail, required, nullptr, &monitor)) return;

      if (const auto container = fixed_property<GUID>(list, monitor, container_id_key, DEVPROP_TYPE_GUID)) {
        display.container = container_bytes(*container);
      }
      if (const auto arrival = fixed_property<FILETIME>(list, monitor, last_arrival_key, DEVPROP_TYPE_FILETIME)) {
        display.arrival = filetime_ticks(*arrival);
      }

      const auto parent = text_property(list, monitor, parent_key, DEVPROP_TYPE_STRING, false);
      if (!parent || parent->empty()) return;
      SP_DEVINFO_DATA parent_device {};
      parent_device.cbSize = sizeof(parent_device);
      if (!SetupDiOpenDeviceInfoW(list, parent->c_str(), nullptr, 0, &parent_device)) return;
      display.kind = classify_parent_hardware_ids(
        text_property(list, parent_device, hardware_ids_key, DEVPROP_TYPE_STRING_LIST, true));
    }

    std::string
    narrow(const std::wstring &text) {
      if (text.empty()) return {};
      const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
      if (size <= 0) return {};
      std::string out(static_cast<std::size_t>(size), '\0');
      WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
      return out;
    }

    std::string
    hex32(std::uint32_t value) {
      char text[11];
      std::snprintf(text, sizeof(text), "0x%08X", value);
      return text;
    }

    bool
    contains_ignoring_case(std::string text, std::string needle) {
      const auto lower = [](std::string &s) { std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); }); };
      lower(text);
      lower(needle);
      return text.find(needle) != std::string::npos;
    }

    edge_send_t
    to_edge(const vdd_ioctl::gated_send_t &sent) {
      switch (sent.delivery) {
        case vdd_ioctl::delivery_e::delivered:
          return { edge_delivery_e::delivered, sent.detail };
        case vdd_ioctl::delivery_e::not_sent:
          return { edge_delivery_e::not_sent, sent.detail };
        case vdd_ioctl::delivery_e::uncertain:
          break;
      }
      return { edge_delivery_e::uncertain, sent.detail };
    }
  }  // namespace

  edge_snapshot_t
  win32_native_edge_t::snapshot() {
    const auto data = w_utils::query_display_config(w_utils::ALL_DEVICES);
    if (!data) return {};
    edge_snapshot_t snapshot { true, {} };
    if (data->paths.empty()) return snapshot;

    // Active paths come first, so the first valid path of a device is its active one if any.
    std::map<std::string, std::size_t> chosen;
    for (std::size_t index = 0; index < data->paths.size(); ++index) {
      if (const auto info = w_utils::get_device_info_for_valid_path(data->paths[index], w_utils::ALL_DEVICES)) {
        chosen.try_emplace(info->device_id, index);
      }
    }
    if (chosen.empty()) return {};

    const device_list_t list;
    if (list.get() == INVALID_HANDLE_VALUE) return {};
    for (const auto &[device_id, index] : chosen) {
      const auto &path = data->paths[index];
      edge_display_t display { .id = os_display_id_t { device_id } };
      if (w_utils::is_active(path)) {
        const auto *mode = w_utils::get_source_mode(w_utils::get_source_index(path, data->modes), data->modes);
        display.info = {
          w_utils::get_display_name(path),
          w_utils::get_friendly_name(path),
          mode && w_utils::is_primary(*mode) ? device_state_e::primary : device_state_e::active,
          w_utils::get_hdr_state(path),
        };
        const auto rotation = rotation_from(path.targetInfo.rotation);
        if (mode && rotation) {
          display.layout = layout_t {
            .position = { mode->position.x, mode->position.y },
            .mode = { { mode->width, mode->height }, { path.targetInfo.refreshRate.Numerator, path.targetInfo.refreshRate.Denominator } },
            .rotation = *rotation,
            .hdr = display.info.hdr_state,
          };
        }
      }
      else {
        display.info = { {}, w_utils::get_friendly_name(path), device_state_e::inactive, hdr_state_e::unknown };
      }
      const auto &adapter = path.targetInfo.adapterId;
      display.adapter = adapter_luid_t { (static_cast<std::uint64_t>(static_cast<std::uint32_t>(adapter.HighPart)) << 32) | adapter.LowPart };
      read_devnode(list.get(), w_utils::get_monitor_device_path(path), display);
      snapshot.displays.push_back(std::move(display));
    }
    return snapshot;
  }

  std::optional<std::uint32_t>
  read_zako_display_count() {
    DWORD size = 0;
    constexpr auto key = L"SOFTWARE\\ZakoTech\\ZakoDisplayAdapter";
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key, L"VDDPATH", RRF_RT_REG_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS || size < sizeof(wchar_t)) {
      return std::nullopt;
    }
    std::wstring folder(size / sizeof(wchar_t), L'\0');
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key, L"VDDPATH", RRF_RT_REG_SZ, nullptr, folder.data(), &size) != ERROR_SUCCESS) return std::nullopt;
    folder.resize(wcsnlen(folder.c_str(), folder.size()));
    std::ifstream file { std::filesystem::path { folder } / L"vdd_settings.xml", std::ios::binary };
    if (!file) return std::nullopt;
    const std::string xml { std::istreambuf_iterator<char> { file }, std::istreambuf_iterator<char> {} };
    return parse_vdd_display_count(xml);
  }

  missing_head_report_t
  win32_native_edge_t::describe_missing_head(const head_guid_t &intent) {
    missing_head_report_t report;
    std::string detail;
    // Every monitor devnode, present or not, whose ContainerId is the request GUID (ZakoVDD sets it from the GUID).
    const HDEVINFO monitors = SetupDiGetClassDevsW(&monitor_class, nullptr, nullptr, 0);
    if (monitors == INVALID_HANDLE_VALUE) {
      detail += "monitor devices unreadable (err=" + std::to_string(GetLastError()) + "); ";
    }
    else {
      bool present = false;
      int matches = 0;
      SP_DEVINFO_DATA device {};
      device.cbSize = sizeof(device);
      for (DWORD index = 0; SetupDiEnumDeviceInfo(monitors, index, &device); ++index) {
        const auto container = fixed_property<GUID>(monitors, device, container_id_key, DEVPROP_TYPE_GUID);
        if (!container || container_bytes(*container) != intent.value) continue;
        ++matches;
        wchar_t instance[512] {};
        SetupDiGetDeviceInstanceIdW(monitors, &device, instance, static_cast<DWORD>(std::size(instance)), nullptr);
        const auto here = fixed_property<DEVPROP_BOOLEAN>(monitors, device, is_present_key, DEVPROP_TYPE_BOOLEAN);
        const bool is_here = here && *here == DEVPROP_TRUE;
        present = present || is_here;
        detail += "monitor " + narrow(instance) + (is_here ? " present" : here ? " not present" : " presence unknown");
        if (const auto status = fixed_property<std::uint32_t>(monitors, device, devnode_status_key, DEVPROP_TYPE_UINT32)) detail += " status=" + hex32(*status);
        if (const auto problem = fixed_property<std::uint32_t>(monitors, device, problem_code_key, DEVPROP_TYPE_UINT32)) detail += " problem=" + std::to_string(*problem);
        detail += "; ";
      }
      SetupDiDestroyDeviceInfoList(monitors);
      if (matches == 0) detail += "no monitor device carries its ContainerId; ";
      report.monitor_present = present;
    }
    // The driver's raw CCD paths, available or not, which the snapshot filters out when they are not valid.
    if (const auto data = w_utils::query_display_config(w_utils::ALL_DEVICES)) {
      std::size_t named = 0;
      for (const auto &path : data->paths) {
        const auto friendly = w_utils::get_friendly_name(path);
        const auto monitor = w_utils::get_monitor_device_path(path);
        if (!contains_ignoring_case(monitor, "zak") && !contains_ignoring_case(friendly, "zako")) continue;
        if (++named > 8) continue;
        const auto &adapter = path.targetInfo.adapterId;
        detail += "path target=" + std::to_string(path.targetInfo.id) + " adapter=" + hex32(static_cast<std::uint32_t>(adapter.HighPart)) + ":" +
                  hex32(adapter.LowPart) + " available=" + std::to_string(path.targetInfo.targetAvailable ? 1 : 0) +
                  " active=" + std::to_string(w_utils::is_active(path) ? 1 : 0) + " name='" + w_utils::get_display_name(path) + "' friendly='" +
                  friendly + "' monitor='" + monitor + "'; ";
      }
      detail += std::to_string(named) + " of " + std::to_string(data->paths.size()) + " display paths name a ZakoVDD target";
    }
    else {
      detail += "display paths unreadable";
    }
    report.detail = std::move(detail);
    return report;
  }

  edge_send_t
  win32_native_edge_t::send_create(const mutation_capability_t &capability, const head_guid_t &intent, const version_gate_t &gate) {
    return to_edge(vdd_ioctl::send_command_gated(capability, vdd_ioctl::create_monitor_t { .guid = format_guid(intent.value) }, gate));
  }

  edge_send_t
  win32_native_edge_t::send_destroy_all(const mutation_capability_t &capability) {
    return to_edge(vdd_ioctl::send_command_gated(capability, vdd_ioctl::destroy_monitors_t {},
      [](const std::optional<std::string> &) -> std::optional<std::string> { return std::nullopt; }));
  }

  std::optional<std::vector<edge_mode_t>>
  win32_native_edge_t::list_modes(const std::string &display_name) {
    const auto name = widen(display_name);
    if (name.empty()) return std::nullopt;
    std::vector<edge_mode_t> modes;
    DEVMODEW mode {};
    mode.dmSize = sizeof(mode);
    for (DWORD index = 0; EnumDisplaySettingsW(name.c_str(), index, &mode); ++index) {
      const edge_mode_t listed { mode.dmPelsWidth, mode.dmPelsHeight, mode.dmDisplayFrequency };
      if (std::ranges::find(modes, listed) == modes.end()) modes.push_back(listed);
      mode = {};
      mode.dmSize = sizeof(mode);
    }
    if (modes.empty()) return std::nullopt;
    return modes;
  }

  edge_send_t
  win32_native_edge_t::set_mode(const mutation_capability_t &capability, const os_display_id_t &display, const display_mode_t &mode) {
    if (display_device::set_display_modes(capability, { { display.value, mode } })) return { edge_delivery_e::delivered, {} };
    return { edge_delivery_e::uncertain, "the display mode could not be applied" };
  }

  edge_send_t
  win32_native_edge_t::send_set_modes(const mutation_capability_t &capability, const std::vector<edge_mode_t> &modes, const version_gate_t &gate) {
    vdd_ioctl::set_modes_t command;
    command.modes.reserve(modes.size());
    for (const auto &mode : modes) {
      command.modes.push_back({ { mode.width, mode.height }, mode.refresh_hz });
    }
    return to_edge(vdd_ioctl::send_command_gated(capability, command, gate));
  }

  edge_send_t
  win32_native_edge_t::activate(const mutation_capability_t &capability, const os_display_id_t &display) {
    if (display_device::extend_topology(capability, display.value)) return { edge_delivery_e::delivered, {} };
    return { edge_delivery_e::uncertain, "the display could not be added to the desktop" };
  }

  edge_send_t
  win32_native_edge_t::arrange(const mutation_capability_t &capability, const std::vector<placement_t> &placements,
    const std::vector<os_display_id_t> &off) {
    capability.require(arrange_operation(capability.kind()));
    auto data = w_utils::query_display_config(w_utils::ACTIVE_ONLY_DEVICES);
    if (!data) return { edge_delivery_e::not_sent, "the desktop could not be read" };

    // A display taken off the desktop is left out of the supplied active paths, in the same write.
    // One already off the desktop is not listed here, which is what was asked.
    std::erase_if(data->paths, [&](const DISPLAYCONFIG_PATH_INFO &path) {
      const auto info = w_utils::get_device_info_for_valid_path(path, w_utils::ACTIVE_ONLY_DEVICES);
      return info && std::ranges::any_of(off, [&](const os_display_id_t &id) { return id.value == info->device_id; });
    });
    if (data->paths.empty()) return { edge_delivery_e::not_sent, "no display would be left on the desktop" };

    std::vector<bool> placed(placements.size(), false);
    std::vector<UINT32> moved;
    for (auto &path : data->paths) {
      const auto info = w_utils::get_device_info_for_valid_path(path, w_utils::ACTIVE_ONLY_DEVICES);
      if (!info) continue;
      const auto it = std::ranges::find_if(placements, [&](const placement_t &placement) { return placement.display.value == info->device_id; });
      if (it == placements.end()) continue;
      const auto index = w_utils::get_source_index(path, data->modes);
      auto *mode = w_utils::get_source_mode(index, data->modes);
      if (!index || !mode) return { edge_delivery_e::not_sent, "display " + info->device_id + " has no source mode" };
      // A duplicated display shares its source; moving it would move its clone too.
      if (std::ranges::find(moved, *index) != moved.end()) {
        return { edge_delivery_e::not_sent, "display " + info->device_id + " shares its source with another display" };
      }
      moved.push_back(*index);
      mode->position = POINTL { it->position.x, it->position.y };
      placed[static_cast<std::size_t>(it - placements.begin())] = true;
    }
    for (std::size_t i = 0; i < placements.size(); ++i) {
      if (!placed[i]) return { edge_delivery_e::not_sent, "display " + placements[i].display.value + " is not on the desktop" };
    }

    // Not SDC_SAVE_TO_DATABASE: the user's saved layout for this display set stays as it was.
    const UINT32 flags { SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_VIRTUAL_MODE_AWARE };
    const LONG result { SetDisplayConfig(static_cast<UINT32>(data->paths.size()), data->paths.data(),
      static_cast<UINT32>(data->modes.size()), data->modes.data(), flags) };
    if (result != ERROR_SUCCESS) {
      return { edge_delivery_e::uncertain, w_utils::get_error_string(result) + " while placing the displays" };
    }
    return { edge_delivery_e::delivered, {} };
  }

  std::chrono::steady_clock::time_point
  win32_native_edge_t::now() {
    return std::chrono::steady_clock::now();
  }

  void
  win32_native_edge_t::sleep(std::chrono::milliseconds duration) {
    std::this_thread::sleep_for(duration);
  }
}  // namespace display_device::topology::win32_edge
