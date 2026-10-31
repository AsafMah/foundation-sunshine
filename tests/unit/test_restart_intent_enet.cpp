#include "src/streaming/restart_intent_enet.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <thread>

TEST(RestartIntentEnet, RealLoopbackDrainDeliversReliableBytesAndServicesAcknowledgements) {
  ASSERT_EQ(enet_initialize(), 0);
  ENetAddress address {};
  ASSERT_EQ(enet_address_set_host(&address, "127.0.0.1"), 0);
  enet_address_set_port(&address, 0);
  auto server = enet_host_create(AF_INET, &address, 1, 1, 0, 0);
  auto client = enet_host_create(AF_INET, nullptr, 1, 1, 0, 0);
  ASSERT_NE(server, nullptr);
  ASSERT_NE(client, nullptr);
  address = server->address;
  auto peer = enet_host_connect(client, &address, 1, 0);
  ASSERT_NE(peer, nullptr);
  ENetPeer *server_peer = nullptr;
  const auto connect_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!server_peer && std::chrono::steady_clock::now() < connect_deadline) {
    ENetEvent event {};
    if (enet_host_service(client, &event, 1) > 0 && event.type == ENET_EVENT_TYPE_RECEIVE) {
      enet_packet_destroy(event.packet);
    }
    if (enet_host_service(server, &event, 1) > 0 && event.type == ENET_EVENT_TYPE_CONNECT) {
      server_peer = event.peer;
    }
  }
  ASSERT_NE(server_peer, nullptr);
  const auto bytes = streaming::restart_intent::termination_packet(0x0109, streaming::restart_intent::SS_TERMINATION_HOST_RESTART_V1);
  ASSERT_EQ(enet_peer_send(server_peer, 0, enet_packet_create(bytes.data(), bytes.size(), ENET_PACKET_FLAG_RELIABLE)), 0);
  std::atomic<bool> finished { false }, received { false };
  std::thread receiver([&] {
    while (!finished.load()) {
      ENetEvent event {};
      if (enet_host_service(client, &event, 2) > 0 && event.type == ENET_EVENT_TYPE_RECEIVE) {
        EXPECT_EQ(event.packet->dataLength, bytes.size());
        EXPECT_EQ(std::memcmp(event.packet->data, bytes.data(), bytes.size()), 0);
        received.store(true);
        enet_packet_destroy(event.packet);
      }
    }
  });
  EXPECT_TRUE(streaming::restart_intent::drain_enet(server));
  EXPECT_TRUE(received.load());
  EXPECT_EQ(server_peer->reliableDataInTransit, 0u);
  finished.store(true);
  receiver.join();
  enet_host_destroy(client);
  enet_host_destroy(server);
  enet_deinitialize();
}
