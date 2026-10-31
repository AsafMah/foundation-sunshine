#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string_view>
#include <utility>

namespace streaming::restart_intent {
  // v1 allocations match moonlight-common-c 080e04dd3c26bede976a8434ddc3defe8fc4b6e8.
  inline constexpr std::uint32_t SS_TERMINATION_HOST_RESTART_V1 = 0x53535201;
  inline constexpr std::uint32_t LI_FF_HOST_RESTART = 0x800;
  inline constexpr std::uint32_t ML_FF_HOST_RESTART = 0x100;
  inline constexpr std::uint32_t GRACEFUL_TERMINATION = 0x80030023;
  inline constexpr std::uint32_t LI_HOST_RESTART_PROTOCOL_VERSION = 1;
  inline constexpr std::string_view HOST_VERSION_ATTRIBUTE = "x-ss-restart.protocolVersion";
  inline constexpr std::string_view CLIENT_VERSION_ATTRIBUTE = "x-ml-restart.protocolVersion";
  inline constexpr auto DRAIN_BUDGET = std::chrono::milliseconds(250);

  enum class console_action { keep,
    requery,
    migrate,
    stop };

  inline console_action
  classify_console_change(std::uint32_t original, std::uint32_t current, bool stopping) {
    if (stopping) {
      return console_action::stop;
    }
    if (current == 0xffffffff) {
      return console_action::requery;
    }
    return original == current ? console_action::keep : console_action::migrate;
  }

  inline bool
  accepts_restart(bool bridge, bool described, std::uint32_t client_flags,
    std::string_view version, bool encrypted) {
    return bridge && described && (client_flags & ML_FF_HOST_RESTART) != 0 &&
           version == "1" && encrypted;
  }

  inline std::uint32_t
  termination_reason(bool migration, bool accepted, bool stopping) {
    return migration && accepted && !stopping ? SS_TERMINATION_HOST_RESTART_V1 : GRACEFUL_TERMINATION;
  }

  inline std::array<std::uint8_t, 4>
  reason_bytes(std::uint32_t reason) {
    return { static_cast<std::uint8_t>(reason >> 24), static_cast<std::uint8_t>(reason >> 16),
      static_cast<std::uint8_t>(reason >> 8), static_cast<std::uint8_t>(reason) };
  }

  inline std::array<std::uint8_t, 8>
  termination_packet(std::uint16_t type, std::uint32_t reason) {
    const auto bytes = reason_bytes(reason);
    return { static_cast<std::uint8_t>(type), static_cast<std::uint8_t>(type >> 8),
      4, 0, bytes[0], bytes[1], bytes[2], bytes[3] };
  }

  class shutdown_state {
  public:
    enum class intent { none,
      migration,
      terminal };

    template <class Publish>
    void
    request(intent requested, Publish &&publish) {
      std::lock_guard lock(mutex_);
      if (requested == intent::terminal || intent_ == intent::none) {
        intent_ = requested;
      }
      // Publish the reason before the global shutdown flag, under the same lock
      // used by the sender. A terminal request can never be downgraded.
      publish();
    }

    template <class Publish>
    void
    request_terminal_from_signal(Publish &&publish) {
      // Do not add a mutex to the repository's signal callback path.
      signal_terminal_.store(true, std::memory_order_release);
      publish();
    }

    void
    set_terminal_probe(std::function<bool()> probe) {
      std::lock_guard lock(mutex_);
      terminal_probe_ = std::move(probe);
    }

    template <class Send>
    void
    send_reason(bool accepted, Send &&send) {
      std::lock_guard lock(mutex_);
      const bool stopping = signal_terminal_.load(std::memory_order_acquire) ||
                            intent_ == intent::terminal || (terminal_probe_ && terminal_probe_());
      send(termination_reason(intent_ == intent::migration, accepted, stopping));
    }

    intent
    current() {
      std::lock_guard lock(mutex_);
      return signal_terminal_.load(std::memory_order_acquire) ? intent::terminal : intent_;
    }

  private:
    std::mutex mutex_;
    std::atomic<bool> signal_terminal_ { false };
    intent intent_ = intent::none;
    std::function<bool()> terminal_probe_;
  };

  inline shutdown_state shutdown;
  inline std::atomic<bool> bridge_available { false };

  // Service work must process ACKs/retransmissions, not dispatch normal control
  // messages. Inject the clock/service operation for bounded fake tests.
  template <class Now, class Service>
  bool
  drain(Now &&now, Service &&service) {
    const auto deadline = now() + DRAIN_BUDGET;
    while (now() < deadline) {
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now());
      if (remaining.count() <= 0) {
        break;
      }
      if (!service(std::min(remaining, std::chrono::milliseconds(5)))) {
        return false;
      }
    }
    return true;
  }
}  // namespace streaming::restart_intent
