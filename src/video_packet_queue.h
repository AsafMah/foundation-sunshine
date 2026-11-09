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
  inline auto
  pop_packet(safe::mail_raw_t::queue_t<packet_t> &packets) {
    return packets->pop();
  }

  inline std::chrono::nanoseconds
  packet_queue_age(double framerate) {
    return std::max(std::chrono::nanoseconds(std::chrono::milliseconds(3)),
      std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::duration<double>(1.5 / std::max(framerate, 1.0))));
  }

  /**
   * @brief Prune only this channel's stale output under the queue lock.
   * Reference recovery remains client-driven; enqueue has no encoder effects.
   */
  inline std::size_t
  raise_packet(safe::mail_raw_t::queue_t<packet_t> &packets, packet_t &&packet,
    std::chrono::nanoseconds max_age = std::chrono::milliseconds(25),
    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now(),
    bool prune = true) {
    static std::atomic<std::uint64_t> pending_drops { 0 };
    static std::atomic<std::int64_t> last_report_ms { std::numeric_limits<std::int64_t>::min() };

    const auto channel = packet->channel_data;
    packet->queued_at = now;
    const auto dropped = packets->raise_pruning([&](const packet_t &queued) {
      return prune && queued->channel_data == channel && now - queued->queued_at > max_age;
    }, std::move(packet));
    if (dropped == 0) {
      return 0;
    }
    pending_drops += dropped;

    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch())
                          .count();
    auto last = last_report_ms.load();
    if (last != std::numeric_limits<std::int64_t>::min() && now_ms - last < 1000) {
      return dropped;
    }
    if (!last_report_ms.compare_exchange_strong(last, now_ms)) {
      return dropped;
    }
    BOOST_LOG(warning) << "Video packet queue age exceeded: dropped " << pending_drops.exchange(0)
                       << " superseded encoded frame(s) since last report; broadcast is behind the encoder"
                       << "; reference recovery remains client-driven";
    return dropped;
  }

  /**
   * @brief Outputs drained from the encoder in one pass (e.g. an AMF
   * pre-analysis/multi-HW batch). Prune once before the first output, so a pass
   * never supersedes its own outputs.
   */
  class packet_batch_t {
  public:
    void
    raise(safe::mail_raw_t::queue_t<packet_t> &packets, packet_t &&packet,
      std::chrono::nanoseconds max_age = std::chrono::milliseconds(25),
      std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) {
      raise_packet(packets, std::move(packet), max_age, now, !emitted_);
      emitted_ = true;
    }

  private:
    bool emitted_ = false;
  };
}  // namespace video
