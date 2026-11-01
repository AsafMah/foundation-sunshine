#pragma once

#include <Windows.h>
#include <atomic>
#include <bit>
#include <stdexcept>
#include <string>

inline bool
excluded_object_is_present(HANDLE candidate, HANDLE reference) {
  using compare_t = BOOL(WINAPI *)(HANDLE, HANDLE);
  static const auto compare = [] {
    // MinGW declares this Windows 10 API but its kernel32 import library omits it.
    const auto address = GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "CompareObjectHandles");
    if (!address) {
      throw std::runtime_error("CompareObjectHandles unavailable in whitelist test");
    }
    return std::bit_cast<compare_t>(address);
  }();
  return compare(candidate, reference) != FALSE;
}

inline std::string
excluded_event_name() {
  static std::atomic<unsigned> sequence { 0 };
  return "Local\\FoundationWhitelistTest-" + std::to_string(GetCurrentProcessId()) +
         "-" + std::to_string(GetTickCount64()) + "-" + std::to_string(sequence.fetch_add(1));
}
