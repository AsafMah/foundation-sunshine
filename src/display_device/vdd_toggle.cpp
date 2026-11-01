#include "vdd_utils.h"

#include <atomic>
#include <stdexcept>

#include "src/globals.h"
#include "src/logging.h"
#include "src/tray/system_tray_i18n.h"

namespace display_device::vdd_utils {
  struct toggle_debounce_t {
    std::chrono::steady_clock::time_point when;
  };

  namespace {
    using toggle_clock = std::chrono::steady_clock;
    std::atomic<std::shared_ptr<const toggle_debounce_t>> last_toggle_reservation {
      std::make_shared<const toggle_debounce_t>(toggle_clock::now())
    };

    void CALLBACK
    expire_confirmation(HWND, UINT, UINT_PTR, DWORD) {
      EnumThreadWindows(GetCurrentThreadId(), [](HWND window, LPARAM) -> BOOL {
        wchar_t name[32] {};
        if (GetClassNameW(window, name, static_cast<int>(std::size(name))) &&
            std::wstring_view { name } == L"#32770") {
          SendMessageW(window, WM_COMMAND, MAKEWPARAM(IDNO, BN_CLICKED), 0);
          return FALSE;
        }
        return TRUE; }, 0);
    }

    bool
    confirm_keep() {
      const auto title = system_tray_i18n::utf8_to_wstring(system_tray_i18n::get_localized_string(system_tray_i18n::KEY_VDD_CONFIRM_KEEP_TITLE));
      const auto message = system_tray_i18n::utf8_to_wstring(system_tray_i18n::get_localized_string(system_tray_i18n::KEY_VDD_CONFIRM_KEEP_MSG));
      const auto timer = SetTimer(nullptr, 0, 20000, expire_confirmation);
      if (!timer) {
        BOOST_LOG(error) << "Unable to start the VDD confirmation timeout: " << GetLastError();
        return false;
      }
      const auto result = MessageBoxW(nullptr, message.c_str(), title.c_str(), MB_YESNO | MB_ICONQUESTION);
      KillTimer(nullptr, timer);
      if (!result) {
        BOOST_LOG(error) << "Unable to show VDD confirmation: " << GetLastError();
      }
      return result == IDYES;
    }
  }  // namespace

  std::optional<toggle_request_t>
  prepare_display_toggle() {
    auto previous = last_toggle_reservation.load();
    const auto now = toggle_clock::now();
    if (now - previous->when < kDefaultDebounceInterval) {
      BOOST_LOG(debug) << "Ignoring a repeated VDD toggle during its cooldown";
      return {};
    }
    const auto reservation = std::make_shared<const toggle_debounce_t>(now);
    if (!last_toggle_reservation.compare_exchange_strong(previous, reservation)) {
      BOOST_LOG(debug) << "Ignoring a concurrent VDD toggle";
      return {};
    }

    toggle_request_t request { false, vdd_generation(), reservation, previous };
    request.create = !is_display_on();
    if (request.create) {
      const auto title = system_tray_i18n::utf8_to_wstring(system_tray_i18n::get_localized_string(system_tray_i18n::KEY_VDD_CONFIRM_CREATE_TITLE));
      const auto message = system_tray_i18n::utf8_to_wstring(system_tray_i18n::get_localized_string(system_tray_i18n::KEY_VDD_CONFIRM_CREATE_MSG));
      const auto result = MessageBoxW(nullptr, message.c_str(), title.c_str(), MB_OKCANCEL | MB_ICONQUESTION);
      if (result != IDOK) {
        if (!result) {
          BOOST_LOG(error) << "Unable to show VDD creation confirmation: " << GetLastError();
        }
        else {
          BOOST_LOG(info) << system_tray_i18n::get_localized_string(system_tray_i18n::KEY_VDD_CANCEL_CREATE_LOG);
        }
        return {};
      }
    }
    return request;
  }

  void
  reject_display_toggle(const toggle_request_t &request) {
    if (!request.reservation || !request.previous) {
      throw std::invalid_argument("Missing VDD debounce reservation");
    }
    auto expected = request.reservation;
    last_toggle_reservation.compare_exchange_strong(expected, request.previous);
  }

  void
  confirm_display_toggle(const toggle_result_t &created, toggle_callbacks_t callbacks) {
    if (created.outcome != toggle_outcome_e::created || !callbacks.extend || !callbacks.destroy) {
      throw std::invalid_argument("Incomplete VDD confirmation handoff");
    }
    const auto current = [&] { return created.generation == vdd_generation(); };
    if (!current()) return;
    try {
      auto device = find_device_by_friendlyname(ZAKO_NAME);
      if (device.empty()) {
        std::this_thread::sleep_for(std::chrono::seconds { 2 });
        if (!current()) return;
        device = find_device_by_friendlyname(ZAKO_NAME);
      }
      if (device.empty()) {
        BOOST_LOG(warning) << "Created VDD was not found; skipping extended topology";
      }
      else if (!callbacks.extend(created.generation, device, created.physical_devices)) {
        BOOST_LOG(warning) << "VDD extended topology did not complete";
      }
      if (!current()) return;
      if (confirm_keep()) {
        BOOST_LOG(info) << "User confirmed keeping the virtual display";
        return;
      }
      BOOST_LOG(info) << "VDD was not confirmed; requesting generation-checked cleanup";
    }
    catch (const std::exception &e) {
      BOOST_LOG(error) << "VDD confirmation failed: " << e.what();
    }
    catch (...) {
      BOOST_LOG(error) << "VDD confirmation failed with an unknown exception";
    }
    callbacks.destroy(created.generation);
  }
}  // namespace display_device::vdd_utils
