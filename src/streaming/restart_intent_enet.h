#pragma once

#include "restart_intent.h"

#include <enet/enet.h>

namespace streaming::restart_intent {
  inline bool
  drain_enet(ENetHost *host) {
    enet_host_flush(host);
    return drain([] { return std::chrono::steady_clock::now(); }, [host](std::chrono::milliseconds wait) {
      ENetEvent event {};
      const auto result = enet_host_service(host, &event, static_cast<enet_uint32>(wait.count()));
      if (result < 0) {
        return false;
      }
      if (result > 0 && event.type == ENET_EVENT_TYPE_RECEIVE) {
        // Service ENet protocol work without dispatching input during shutdown.
        enet_packet_destroy(event.packet);
      }
      return true; });
  }
}  // namespace streaming::restart_intent
