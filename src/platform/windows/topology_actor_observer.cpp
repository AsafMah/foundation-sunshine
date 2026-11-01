#include "topology_actor_observer.h"

namespace platf {

  using display_device::topology::actor_receipt_t;
  using display_device::topology::process_observation_e;

  const process_query_api_t &
  system_process_query_api() noexcept {
    static const process_query_api_t api { OpenProcess, GetProcessTimes, WaitForSingleObject, GetLastError, CloseHandle };
    return api;
  }

  process_observation_e
  observe_actor_process(const actor_receipt_t &receipt, const process_query_api_t &api) noexcept {
    if (!receipt.pid || !receipt.creation_time) return process_observation_e::inaccessible;

    const HANDLE process = api.open(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, receipt.pid);
    if (!process) {
      return api.last_error() == ERROR_INVALID_PARAMETER ? process_observation_e::absent : process_observation_e::inaccessible;
    }

    const auto observed = [&]() {
      FILETIME creation {}, exit {}, kernel {}, user {};
      if (!api.times(process, &creation, &exit, &kernel, &user)) return process_observation_e::inaccessible;
      const auto created = (static_cast<std::uint64_t>(creation.dwHighDateTime) << 32) | creation.dwLowDateTime;
      if (created != receipt.creation_time) return process_observation_e::identity_mismatch;
      switch (api.wait(process, 0)) {
        case WAIT_TIMEOUT:
          return process_observation_e::running;
        case WAIT_OBJECT_0:
          return process_observation_e::exited;
        default:
          return process_observation_e::inaccessible;
      }
    }();
    api.close(process);
    return observed;
  }

}  // namespace platf
