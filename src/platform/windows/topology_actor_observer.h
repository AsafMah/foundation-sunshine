/**
 * @file src/platform/windows/topology_actor_observer.h
 * @brief Recovery observation of a recorded external topology actor.
 */
#pragma once

#include <windows.h>

#include "src/display_device/topology_actor_record.h"

namespace platf {

  /** Kernel entry points used by recovery observation; replaceable in tests. */
  struct process_query_api_t {
    HANDLE(WINAPI *open)(DWORD, BOOL, DWORD);
    BOOL(WINAPI *times)(HANDLE, LPFILETIME, LPFILETIME, LPFILETIME, LPFILETIME);
    DWORD(WINAPI *wait)(HANDLE, DWORD);
    DWORD(WINAPI *last_error)();
    BOOL(WINAPI *close)(HANDLE);
  };

  const process_query_api_t &
  system_process_query_api() noexcept;

  /**
   * Open the receipt's PID once and use that same handle for both identity
   * (creation time) and liveness. Anything that is not positive evidence of the
   * same process running or exited, or of no process with that PID, is
   * `inaccessible`.
   */
  display_device::topology::process_observation_e
  observe_actor_process(const display_device::topology::actor_receipt_t &receipt,
    const process_query_api_t &api = system_process_query_api()) noexcept;

}  // namespace platf
