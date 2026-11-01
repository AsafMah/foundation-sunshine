/**
 * @file src/display_device/topology_process_lifecycle.cpp
 * @brief Process lifecycle of the topology coordinator.
 */
#include "topology_process_lifecycle.h"

#include <exception>
#include <stdexcept>

namespace display_device::topology {

  namespace {
    std::string
    describe(const std::exception_ptr &exception) {
      if (!exception) return {};
      try {
        std::rethrow_exception(exception);
      }
      catch (const std::exception &error) {
        return error.what();
      }
      catch (...) {
        return "unknown exception";
      }
    }

    std::string
    describe(const command_result_t &result) {
      std::string line { to_string(result.outcome) };
      if (result.reason != reject_reason_e::none) {
        line += " / ";
        line += to_string(result.reason);
      }
      if (const auto detail = describe(result.exception); !detail.empty()) {
        line += ": " + detail;
      }
      return line;
    }

    lifecycle_hooks_t
    validated(lifecycle_hooks_t hooks) {
      if (!hooks.startup_restore || !hooks.shutdown_restore || !hooks.quiet_vdd_destroy || !hooks.keep_vdd) {
        throw std::invalid_argument { "Topology lifecycle needs startup, shutdown and VDD cleanup hooks" };
      }
      if (!hooks.actor.store || !hooks.actor.observe) {
        throw std::invalid_argument { "Topology lifecycle needs the actor record store and process observer" };
      }
      return hooks;
    }
  }  // namespace

  const char *
  to_string(lifecycle_start_e value) noexcept {
    switch (value) {
      case lifecycle_start_e::completed:
        return "completed";
      case lifecycle_start_e::failed:
        return "failed";
      case lifecycle_start_e::suspended:
        return "suspended";
      case lifecycle_start_e::timed_out:
        return "timed_out";
    }
    return "unknown";
  }

  const char *
  to_string(lifecycle_cleanup_e value) noexcept {
    switch (value) {
      case lifecycle_cleanup_e::completed:
        return "completed";
      case lifecycle_cleanup_e::skipped:
        return "skipped";
      case lifecycle_cleanup_e::rejected:
        return "rejected";
      case lifecycle_cleanup_e::failed:
        return "failed";
      case lifecycle_cleanup_e::timed_out:
        return "timed_out";
    }
    return "unknown";
  }

  process_lifecycle_t::process_lifecycle_t(coordinator_options_t options, lifecycle_hooks_t hooks):
      hooks_(validated(std::move(hooks))),
      coordinator_(std::move(options)) {
    if (!hooks_.actor.restore_pending) {
      hooks_.actor.restore_pending = [this]() { return coordinator_.status().startup_restore_pending; };
    }
    if (!hooks_.actor.report) {
      hooks_.actor.report = [this](const std::string &line) { report(line); };
    }
  }

  bool
  process_lifecycle_t::suppress_automatic_vdd() const {
    const auto status = coordinator_.status();
    return status.native.journal_present || status.native.in_conflict;
  }

  lifecycle_start_e
  process_lifecycle_t::start() {
    auto started = coordinator_.start([this](const mutation_capability_t &capability, const command_context_t &) {
      const auto classified = recover_startup_actor(coordinator_, capability, hooks_.actor, hooks_.startup_restore);
      report(std::string { "Startup actor record: " } + std::string { to_string(classified) });
    });
    if (started.wait_for(hooks_.startup_wait) != std::future_status::ready) {
      if (coordinator_.status().external_recovery_pending) {
        report("Startup is suspended behind a recorded external actor; topology stays fenced until it exits");
        return lifecycle_start_e::suspended;
      }
      report("Startup recovery is still running; topology work waits behind it");
      return lifecycle_start_e::timed_out;
    }
    const auto &result = started.get();
    if (result.completed()) return lifecycle_start_e::completed;
    report("Startup recovery failed: " + describe(result));
    return lifecycle_start_e::failed;
  }

  lifecycle_cleanup_e
  process_lifecycle_t::shutdown_restore() {
    std::lock_guard lock { shutdown_mutex_ };
    if (shut_down_ || coordinator_.status().phase == coordinator_phase_e::not_started) {
      return lifecycle_cleanup_e::skipped;
    }
    coordinator_.begin_draining();
    return run_bounded(command_kind_e::shutdown_restore, hooks_.shutdown_restore, hooks_.shutdown_restore_wait);
  }

  lifecycle_cleanup_e
  process_lifecycle_t::shutdown_vdd_destroy() {
    std::lock_guard lock { shutdown_mutex_ };
    if (shut_down_) return lifecycle_cleanup_e::skipped;
    shut_down_ = true;
    auto result = lifecycle_cleanup_e::skipped;
    if (coordinator_.status().phase != coordinator_phase_e::not_started && !hooks_.keep_vdd()) {
      coordinator_.begin_draining();
      result = run_bounded(command_kind_e::shutdown_vdd_destroy, hooks_.quiet_vdd_destroy, hooks_.vdd_destroy_wait);
    }
    coordinator_.stop();
    return result;
  }

  lifecycle_cleanup_e
  process_lifecycle_t::run_bounded(command_kind_e kind, command_body_t body, std::chrono::milliseconds wait) {
    const auto posted = coordinator_.post_tracked(kind, std::move(body));
    if (posted.reason != reject_reason_e::none) {
      report(std::string { to_string(kind) } + " rejected: " + std::string { to_string(posted.reason) });
      return lifecycle_cleanup_e::rejected;
    }
    if (posted.completion.wait_for(wait) != std::future_status::ready) {
      report(std::string { to_string(kind) } + " did not finish in time; it is left to the next startup");
      return lifecycle_cleanup_e::timed_out;
    }
    const auto &result = posted.completion.get();
    if (result.completed()) return lifecycle_cleanup_e::completed;
    report(std::string { to_string(kind) } + " failed: " + describe(result));
    return lifecycle_cleanup_e::failed;
  }

  void
  process_lifecycle_t::report(const std::string &line) const {
    if (hooks_.report) hooks_.report(line);
  }

}  // namespace display_device::topology