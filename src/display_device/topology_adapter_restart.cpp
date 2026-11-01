/**
 * @file src/display_device/topology_adapter_restart.cpp
 * @brief Last-resort adapter restart run as one supervised external actor.
 */
#include "topology_adapter_restart.h"

#include <utility>

namespace display_device::topology {

  namespace {

    /**
     * Forwards the settled hand-off to the relay and keeps its supervisor alive
     * until then. The supervisor owns this sink and releases it only when it
     * delivers the hand-off, so a supervisor that never settles (blocked) keeps
     * itself, and with it the exclusion, for the life of the process, while a
     * settled one is freed.
     */
    class retaining_sink_t final: public actor_cleanup_sink_t {
    public:
      explicit retaining_sink_t(std::shared_ptr<actor_cleanup_relay_t> relay):
          relay_ { std::move(relay) } {}

      /** Called by publish, which start runs before the body is spawned. */
      void
      keep(std::shared_ptr<actor_supervisor_t> supervisor) noexcept {
        supervisor_ = std::move(supervisor);
      }

      void
      accept(actor_cleanup_handoff_t handoff) noexcept override {
        relay_->accept(std::move(handoff));
      }

    private:
      std::shared_ptr<actor_cleanup_relay_t> relay_;
      std::shared_ptr<actor_supervisor_t> supervisor_;
    };

    adapter_restart_result_t
    restore_without_actor(external_cleanup_t &obligation, external_cleanup_body_t restore, std::string detail) {
      const bool queued = obligation.queue_first(std::move(restore));
      return { queued ? adapter_restart_e::cleanup_owned : adapter_restart_e::wedged, {}, std::move(detail) };
    }

  }  // namespace

  adapter_restart_result_t
  restart_adapter(coordinator_t &coordinator, const mutation_capability_t &capability, adapter_restart_plan_t plan,
    external_cleanup_body_t restore) {
    if (!plan.store || !plan.process || !restore) {
      capability.require(operation_e::adapter_restart);
      return { adapter_restart_e::not_started, {}, "The adapter restart needs a record store, a process and a restoration" };
    }

    // Refusals (no authority, foreign worker, draining, an undischarged earlier obligation) propagate.
    const auto obligation = coordinator.begin_external_cleanup(capability);

    auto intent = begin_actor_intent(plan.operation, std::move(plan.boot), actor_obligation_e::restore_legacy_display, "adapter restart");
    if (!intent) {
      return restore_without_actor(*obligation, std::move(restore), intent.error().detail);
    }

    auto relay = std::make_shared<actor_cleanup_relay_t>(obligation, plan.store, plan.operation);
    auto sink = std::make_shared<retaining_sink_t>(relay);
    // Publishing cannot fail, so a start error means nothing was persisted or launched.
    const actor_supervisor_t::publish_t publish = [sink](const std::shared_ptr<actor_supervisor_t> &self) -> result_t<void> {
      sink->keep(self);
      return {};
    };
    auto options = std::move(plan.options);
    options.cleanup_sink = sink;
    auto started = actor_supervisor_t::start(plan.store, std::move(*intent), relay->exclusion(), publish,
      make_process_actor_body(std::move(plan.process)), std::move(options));
    if (!started) {
      return restore_without_actor(*obligation, std::move(restore), started.error().detail);
    }

    const auto supervisor = std::move(*started);
    if (!relay->submit(std::move(restore))) {
      return { adapter_restart_e::wedged, supervisor->wait_launch(), "The adapter restart's cleanup was refused", supervisor };
    }
    auto launch = supervisor->wait_launch();
    auto detail = launch.detail;
    return { adapter_restart_e::cleanup_owned, std::move(launch), std::move(detail), supervisor };
  }

}  // namespace display_device::topology
