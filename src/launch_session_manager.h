/**
 * @file src/launch_session_manager.h
 * @brief Bounded lifecycle manager for pending GameStream launch tickets.
 */
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "crypto.h"

namespace rtsp_stream {
  struct launch_session_t;

  constexpr std::size_t DEFAULT_PENDING_LAUNCH_LIMIT = 16;
  constexpr std::size_t DEFAULT_PENDING_LAUNCH_LIMIT_PER_CLIENT = 2;

  enum class launch_ticket_state_e {
    pending,
    claimed,
  };

  enum class launch_ticket_register_e {
    accepted,
    replaced,
    global_limit,
    client_limit,
    client_busy,
    denied,
    binding_rejected,
  };

  struct encrypted_launch_claim_t {
    std::shared_ptr<launch_session_t> session;
    std::vector<std::uint8_t> plaintext;

    explicit operator bool() const noexcept {
      return session != nullptr;
    }
  };

  class launch_session_manager_t {
  public:
    using clock_t = std::chrono::steady_clock;

    explicit launch_session_manager_t(
      std::size_t global_limit = DEFAULT_PENDING_LAUNCH_LIMIT,
      std::size_t per_client_limit = DEFAULT_PENDING_LAUNCH_LIMIT_PER_CLIENT);
    ~launch_session_manager_t();

    launch_session_manager_t(const launch_session_manager_t &) = delete;
    launch_session_manager_t &
    operator=(const launch_session_manager_t &) = delete;

    /**
     * @brief Publish a ticket only after its optional binding check succeeds.
     * @details before_publish runs once, under the ticket lock, after capacity checks
     *          and before publication/replacement. It must be a short leaf operation:
     *          no ticket-manager reentry, blocking work or platform calls. Exceptions
     *          propagate without changing the ticket being replaced.
     */
    launch_ticket_register_e
    register_session(std::shared_ptr<launch_session_t> session,
                     std::chrono::milliseconds timeout,
                     clock_t::time_point now = clock_t::now(),
                     std::function<bool()> before_publish = {});

    std::shared_ptr<launch_session_t>
    claim_plaintext(std::string_view remote_address,
                    clock_t::time_point now = clock_t::now());

    encrypted_launch_claim_t
    claim_encrypted(std::string_view remote_address,
                    std::string_view tagged_cipher,
                    crypto::aes_t iv,
                    clock_t::time_point now = clock_t::now());

    /**
     * Consume an existing ticket only after the optional activation hook succeeds
     * under the ticket lock. Like before_publish, the hook must be a short leaf.
     * A rejected/throwing hook leaves the ticket and its claims unchanged.
     */
    bool
    activate(std::uint32_t launch_session_id, std::function<bool()> before_consume = {});

    bool
    release(std::uint32_t launch_session_id,
            std::chrono::milliseconds timeout,
            clock_t::time_point now = clock_t::now());

    bool
    erase(std::uint32_t launch_session_id);

    std::size_t erase_client(std::string_view client_uuid);

    std::size_t
    prune(clock_t::time_point now = clock_t::now());

    void
    clear();

    std::size_t
    size(clock_t::time_point now = clock_t::now());

    /**
     * @brief Whether the ticket of this launch is still pending or claimed.
     * @details Expired pending tickets are pruned first, so an expired ticket is never reported.
     */
    bool
    contains(std::uint32_t launch_session_id, clock_t::time_point now = clock_t::now());

  private:
    struct impl_t;
    std::unique_ptr<impl_t> _impl;
  };
}  // namespace rtsp_stream
