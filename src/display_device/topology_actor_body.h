/**
 * @file src/display_device/topology_actor_body.h
 * @brief Supervisor body that launches and observes one external actor process.
 *
 * The process object is created, launched, queried and observed only on the
 * supervisor's body thread. It is retained by the supervisor before launch, so
 * the original process handles outlive any later error, exception or timeout.
 * Its final destruction only releases handles and may run on any thread. The
 * body never terminates the process.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <system_error>

#include "topology_actor_supervisor.h"

namespace display_device::topology {

  enum class actor_wait_e : std::uint8_t {
    running,
    exited,
    /** The original handle can no longer be observed. */
    failed,
  };

  struct actor_wait_t {
    actor_wait_e state { actor_wait_e::failed };
    std::optional<std::uint32_t> exit_code;
    std::string detail;
  };

  /** One external process owned through its original creation handles. */
  class actor_process_t {
  public:
    virtual ~actor_process_t() = default;

    /**
     * Create the process once. Errors and exceptions may follow a successful
     * creation; `started()` tells the two apart.
     */
    virtual void
    launch(std::error_code &ec) = 0;

    /**
     * True once creation succeeded and the original handles are owned. It must
     * become true before anything after the OS creation call can fail or throw,
     * so after `launch` returns or throws, false proves no process was created.
     */
    virtual bool
    started() const = 0;

    /** PID and creation time read from the original handle, if available. */
    virtual std::optional<actor_receipt_t>
    receipt() = 0;

    /** Block until the process exits or can no longer be observed. */
    virtual actor_wait_t
    wait() = 0;
  };

  using actor_process_factory_t = std::function<std::shared_ptr<actor_process_t>()>;

  /** `make` runs on the body thread; a null result is a definite non-launch. */
  actor_body_t
  make_process_actor_body(actor_process_factory_t make);

}  // namespace display_device::topology
