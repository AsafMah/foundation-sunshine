// Compile the actual transport with every OS edge redirected; no native API runs.
#include "vdd_transport_shim.h"
#include "src/display_device/vdd_control_ioctl.h"
#include "src/logging.h"

#include <SetupAPI.h>
#include <Windows.h>
#include <algorithm>
#include <cwchar>
#include <devguid.h>
#include <gtest/gtest.h>
#include <iomanip>

namespace vdd_transport_test {
  state_t state;

  HDEVINFO
  get_class_devs(const GUID *, PCWSTR, HWND, DWORD) {
    ++state.enumerations;
    return state.outcome == outcome_e::missing ? INVALID_HANDLE_VALUE : reinterpret_cast<HDEVINFO>(1);
  }

  BOOL
  destroy_list(HDEVINFO) {
    ++state.list_destroys;
    return TRUE;
  }

  BOOL
  enum_interfaces(HDEVINFO, PSP_DEVINFO_DATA, const GUID *, DWORD, PSP_DEVICE_INTERFACE_DATA) {
    return TRUE;
  }

  BOOL
  get_detail(HDEVINFO, PSP_DEVICE_INTERFACE_DATA, PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail, DWORD,
    PDWORD required, PSP_DEVINFO_DATA) {
    constexpr wchar_t path[] = L"fake-vdd";
    if (required) *required = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W) + sizeof(path);
    if (detail) std::copy(std::begin(path), std::end(path), detail->DevicePath);
    return detail != nullptr;
  }

  HANDLE
  create_file(LPCWSTR path, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE) {
    EXPECT_STREQ(path, L"fake-vdd");
    ++state.opens;
    return state.outcome == outcome_e::open_failed ? INVALID_HANDLE_VALUE : reinterpret_cast<HANDLE>(2);
  }

  BOOL
  close_handle(HANDLE) {
    ++state.closes;
    return TRUE;
  }

  BOOL
  device_io(HANDLE, DWORD code, LPVOID input, DWORD length, LPVOID, DWORD, LPDWORD, LPOVERLAPPED) {
    EXPECT_EQ(code, IOCTL_VDD_COMMAND);
    ++state.writes;
    state.payload_bytes = length;
    if (input && length >= sizeof(wchar_t)) {
      const auto *text = static_cast<const wchar_t *>(input);
      EXPECT_EQ(text[length / sizeof(wchar_t) - 1], L'\0');
      state.payload.assign(text, length / sizeof(wchar_t) - 1);
    }
    return state.outcome != outcome_e::write_failed;
  }

  DWORD
  get_error() { return ERROR_GEN_FAILURE; }
  DWORD
  get_process_id() {
    ADD_FAILURE() << "frame-channel path is outside this fixture";
    return 0;
  }
  BOOL
  enum_info(HDEVINFO, DWORD, PSP_DEVINFO_DATA) {
    ADD_FAILURE() << "adapter probe is outside this fixture";
    return FALSE;
  }
  BOOL
  get_registry_property(HDEVINFO, PSP_DEVINFO_DATA, DWORD, PDWORD, PBYTE, DWORD, PDWORD) {
    ADD_FAILURE() << "adapter probe is outside this fixture";
    return FALSE;
  }
  BOOL
  get_property(HDEVINFO, PSP_DEVINFO_DATA, const DEVPROPKEY *, DEVPROPTYPE *, PBYTE, DWORD, PDWORD, DWORD) {
    ADD_FAILURE() << "adapter probe is outside this fixture";
    return FALSE;
  }
}  // namespace vdd_transport_test

#define SetupDiGetClassDevsW vdd_transport_test::get_class_devs
#define SetupDiDestroyDeviceInfoList vdd_transport_test::destroy_list
#define SetupDiEnumDeviceInterfaces vdd_transport_test::enum_interfaces
#define SetupDiGetDeviceInterfaceDetailW vdd_transport_test::get_detail
#define SetupDiEnumDeviceInfo vdd_transport_test::enum_info
#define SetupDiGetDeviceRegistryPropertyW vdd_transport_test::get_registry_property
#define SetupDiGetDevicePropertyW vdd_transport_test::get_property
#define CreateFileW vdd_transport_test::create_file
#define CloseHandle vdd_transport_test::close_handle
#define DeviceIoControl vdd_transport_test::device_io
#define GetLastError vdd_transport_test::get_error
#define GetCurrentProcessId vdd_transport_test::get_process_id
#define send_command_buffer native_send_command_buffer
#include "src/display_device/vdd_ioctl.cpp"
#undef send_command_buffer
#undef GetCurrentProcessId
#undef GetLastError
#undef DeviceIoControl
#undef CloseHandle
#undef CreateFileW
#undef SetupDiGetDevicePropertyW
#undef SetupDiGetDeviceRegistryPropertyW
#undef SetupDiEnumDeviceInfo
#undef SetupDiGetDeviceInterfaceDetailW
#undef SetupDiEnumDeviceInterfaces
#undef SetupDiDestroyDeviceInfoList
#undef SetupDiGetClassDevsW

namespace vdd_transport_test {
  display_device::vdd_ioctl::result
  send(const display_device::vdd_ioctl::detail::encoded_command_t &command) {
    return display_device::vdd_ioctl::detail::native_send_command_buffer(command);
  }
}  // namespace vdd_transport_test
