/**
 * @file src/video_packet_queue.h
 * @brief Shared enqueue path for encoded video packets.
 */
#pragma once

#include "src/logging.h"
#include "src/video.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace video {
  /// Maximum encoded frames waiting for the broadcast thread. Older frames are
  /// superseded rather than allowed to build a standing latency backlog.
  inline constexpr std::size_t max_queued_packets = 2;

  /**
   * @brief Queue an encoded packet for broadcast. If max_queued_packets are
   * already waiting, the broadcast thread is behind: every waiting packet is
   * dropped and only the newest one is queued.
   *
   * Drops look like network loss to the client, so they are always reported,
   * at most once per second with the count accumulated since the last report.
   */
  inline void
  raise_packet(safe::mail_raw_t::queue_t<packet_t> &packets, packet_t &&packet) {
    static std::atomic<std::uint64_t> pending_drops { 0 };
    static std::atomic<std::int64_t> last_report_ms { std::numeric_limits<std::int64_t>::min() };

    const auto dropped = packets->raise_superseding(max_queued_packets, std::move(packet));
    if (dropped == 0) {
      return;
    }
    pending_drops += dropped;

    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch())
                          .count();
    auto last = last_report_ms.load();
    if (last != std::numeric_limits<std::int64_t>::min() && now_ms - last < 1000) {
      return;
    }
    if (!last_report_ms.compare_exchange_strong(last, now_ms)) {
      return;
    }
    BOOST_LOG(warning) << "Video packet queue full: dropped " << pending_drops.exchange(0)
                       << " superseded encoded frame(s) since last report; broadcast is behind the encoder"
                       << " and the client will see the gap as frame loss";
  }
}  // namespace video
