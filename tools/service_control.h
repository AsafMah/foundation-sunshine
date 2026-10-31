#pragma once

#include <Windows.h>

#include <bit>
#include <charconv>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace sunshinesvc::service_control {
  enum class migration_exit { orderly,
    forced,
    force_failed,
    incomplete };

  template <class Signal, class Wait, class Force>
  migration_exit
  finish_migration(Signal &&signal, Wait &&wait, Force &&force) {
    if (signal() && wait(20000)) {
      return migration_exit::orderly;
    }
    if (!force()) {
      return migration_exit::force_failed;
    }
    return wait(5000) ? migration_exit::forced : migration_exit::incomplete;
  }

  inline DWORD
  set_handle_list(STARTUPINFOEXW &startup, std::span<const HANDLE> handles) {
    // The caller retains the backing list through CreateProcess.
    if (!UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
          const_cast<HANDLE *>(handles.data()), handles.size_bytes(), nullptr, nullptr)) {
      return GetLastError();
    }
    return ERROR_SUCCESS;
  }

  class handle {
  public:
    explicit handle(HANDLE value = nullptr): value_(value) {}
    handle(const handle &) = delete;
    handle &
    operator=(const handle &) = delete;
    ~handle() {
      if (value_) {
        CloseHandle(value_);
      }
    }
    HANDLE
    get() const { return value_; }
    void
    reset(HANDLE value) {
      if (value_) {
        CloseHandle(value_);
      }
      value_ = value;
    }

  private:
    HANDLE value_;
  };

  struct arguments {
    bool present = false;
    std::string error;
    HANDLE restart = nullptr;
    HANDLE stop = nullptr;
    std::vector<char *> filtered;
  };

  inline arguments
  parse_arguments(int argc, char **argv) {
    arguments result;
    bool version_seen = false, restart_seen = false, stop_seen = false;
    for (int i = 0; i < argc; ++i) {
      const std::string_view arg(argv[i]);
      if (i == 0 || !arg.starts_with("--service-")) {
        result.filtered.push_back(argv[i]);
        continue;
      }
      result.present = true;
      const auto parse_handle = [&](std::string_view text, HANDLE &destination, bool &seen) {
        std::uintptr_t value = 0;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
        if (seen || parsed.ec != std::errc() || parsed.ptr != text.data() + text.size() ||
            value == 0 || value == static_cast<std::uintptr_t>(-1)) {
          result.error = "Invalid or duplicate service event handle";
        }
        else {
          destination = reinterpret_cast<HANDLE>(value);
          seen = true;
        }
      };
      if (arg.starts_with("--service-control-version=")) {
        if (version_seen || arg != "--service-control-version=1") {
          result.error = "Invalid or duplicate service control version";
        }
        version_seen = true;
      }
      else if (arg.starts_with("--service-restart-event=")) {
        parse_handle(arg.substr(std::string_view("--service-restart-event=").size()), result.restart, restart_seen);
      }
      else if (arg.starts_with("--service-stop-event=")) {
        parse_handle(arg.substr(std::string_view("--service-stop-event=").size()), result.stop, stop_seen);
      }
      else {
        result.error = "Unknown service control argument";
      }
    }
    if (result.present && (!version_seen || !restart_seen || !stop_seen || result.restart == result.stop)) {
      result.error = "Incomplete or aliased service event bridge";
    }
    result.filtered.push_back(nullptr);
    return result;
  }

  // NtQueryEvent both verifies the object is an event and rejects auto-reset
  // events. Query-only child handles cannot signal a restart or service stop.
  inline bool
  is_manual_reset_event(HANDLE event) {
    struct event_basic_information {
      LONG type;
      LONG state;
    };
    using query_event_t = LONG(NTAPI *)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    const auto ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto query = ntdll ? std::bit_cast<query_event_t>(GetProcAddress(ntdll, "NtQueryEvent")) : nullptr;
    event_basic_information info {};
    return query && query(event, 0, &info, sizeof(info), nullptr) >= 0 && info.type == 0;
  }

  inline std::string
  validate(const arguments &args, bool system_identity) {
    if (!args.error.empty()) {
      return args.error;
    }
    if (!args.present) {
      return {};
    }
    if (!system_identity) {
      return "Service event bridge requires SYSTEM identity";
    }
    if (!is_manual_reset_event(args.restart) || !is_manual_reset_event(args.stop)) {
      return "Service event bridge requires valid manual-reset events";
    }
    if (WaitForSingleObject(args.restart, 0) == WAIT_FAILED ||
        WaitForSingleObject(args.stop, 0) == WAIT_FAILED) {
      return "Service event bridge requires synchronizable events";
    }
    return {};
  }

  class generation_events {
  public:
    DWORD
    create(HANDLE service_stop) {
      restart_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
      if (!restart_.get()) {
        return GetLastError();
      }
      HANDLE child_restart = nullptr, child_stop = nullptr;
      constexpr DWORD child_access = SYNCHRONIZE | 0x0001;  // EVENT_QUERY_STATE
      if (!DuplicateHandle(GetCurrentProcess(), restart_.get(), GetCurrentProcess(),
            &child_restart, child_access, TRUE, 0)) {
        return GetLastError();
      }
      child_restart_.reset(child_restart);
      if (!DuplicateHandle(GetCurrentProcess(), service_stop, GetCurrentProcess(),
            &child_stop, child_access, TRUE, 0)) {
        return GetLastError();
      }
      child_stop_.reset(child_stop);
      return ERROR_SUCCESS;
    }
    HANDLE
    restart() const { return restart_.get(); }
    HANDLE
    child_restart() const { return child_restart_.get(); }
    HANDLE
    child_stop() const { return child_stop_.get(); }
    std::wstring
    command() const {
      return L"Sunshine.exe --service-control-version=1 --service-restart-event=" +
             std::to_wstring(reinterpret_cast<std::uintptr_t>(child_restart())) +
             L" --service-stop-event=" +
             std::to_wstring(reinterpret_cast<std::uintptr_t>(child_stop()));
    }

  private:
    handle restart_, child_restart_, child_stop_;
  };

  class listener {
  public:
    listener(const listener &) = delete;
    listener &
    operator=(const listener &) = delete;
    listener() = default;
    ~listener() {
      if (thread_.joinable()) {
        SetEvent(cancel_.get());
        thread_.join();
      }
    }
    DWORD
    start(const arguments &args, std::function<void()> restart, std::function<void()> stop, std::function<void(DWORD)> error, std::function<void()> ready = [] {}) {
      if (!SetHandleInformation(args.restart, HANDLE_FLAG_INHERIT, 0) ||
          !SetHandleInformation(args.stop, HANDLE_FLAG_INHERIT, 0)) {
        return GetLastError();
      }
      cancel_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
      if (!cancel_.get()) {
        return GetLastError();
      }
      // Core owns its inherited copies, never the service's original handles.
      restart_.reset(args.restart);
      stop_.reset(args.stop);
      thread_ = std::thread([this, restart = std::move(restart), stop = std::move(stop),
                              error = std::move(error), ready = std::move(ready)] {
        ready();
        bool migration_seen = false;
        for (;;) {
          const HANDLE events[] = { stop_.get(), cancel_.get(), restart_.get() };
          const auto result = WaitForMultipleObjects(migration_seen ? 2 : 3, events, FALSE, INFINITE);
          if (result == WAIT_OBJECT_0) {
            stop();
            return;
          }
          if (result == WAIT_OBJECT_0 + 1) {
            return;
          }
          if (!migration_seen && result == WAIT_OBJECT_0 + 2) {
            // Recheck stop in case both became signaled between wait/dispatch.
            if (WaitForSingleObject(stop_.get(), 0) == WAIT_OBJECT_0) {
              stop();
              return;
            }
            restart();
            migration_seen = true;
            continue;
          }
          error(result == WAIT_FAILED ? GetLastError() : ERROR_INVALID_FUNCTION);
          return;
        }
      });
      return ERROR_SUCCESS;
    }

  private:
    handle cancel_, restart_, stop_;
    std::thread thread_;
  };
}  // namespace sunshinesvc::service_control
