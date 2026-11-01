/**
 * @file src/display_device/topology_actor_supervisor.cpp
 * @brief Bounded, strongly owned supervision of one external topology actor.
 */
#include "topology_actor_supervisor.h"

#include <algorithm>

namespace display_device::topology {

  namespace {
    error_t
    unavailable(std::string detail) {
      return error_t { error_code_e::resource_unavailable, std::move(detail), {} };
    }
  }  // namespace

  bool
  actor_supervisor_t::context_t::report_launched(actor_receipt_t receipt) {
    return owner_.persist_receipt(receipt);
  }

  void
  actor_supervisor_t::context_t::retain(std::shared_ptr<void> resource) {
    std::lock_guard lock { owner_.mutex_ };
    if (!owner_.body_open_ || std::this_thread::get_id() != owner_.body_thread_) return;
    owner_.retained_.push_back(std::move(resource));
  }

  actor_supervisor_t::actor_supervisor_t(private_t, std::shared_ptr<actor_record_store_t> store, actor_record_t intent,
    std::shared_ptr<void> exclusion, actor_body_t body, std::chrono::steady_clock::time_point deadline):
      store_(std::move(store)),
      body_(std::move(body)),
      deadline_(deadline),
      exclusion_(std::move(exclusion)),
      record_(std::move(intent)) {}

  result_t<std::shared_ptr<actor_supervisor_t>>
  actor_supervisor_t::start(std::shared_ptr<actor_record_store_t> store, actor_record_t intent, std::shared_ptr<void> exclusion,
    const publish_t &publish, actor_body_t body, actor_supervisor_options_t options) {
    const auto started = std::chrono::steady_clock::now();
    if (options.deadline <= std::chrono::milliseconds::zero()) {
      return std::unexpected(error_t { error_code_e::invalid_deadline, "The external actor launch deadline must be positive", {} });
    }
    if (intent.stage != actor_stage_e::intent || intent.receipt) {
      return std::unexpected(error_t { error_code_e::invalid_transition, "An external actor can only start from a fresh intent", {} });
    }
    if (!store || !publish || !body) {
      return std::unexpected(unavailable("The external actor supervisor needs a store, a publisher and a body"));
    }

    try {
      if (auto created = store->create(intent); !created) return std::unexpected(created.error());
    }
    catch (...) {
      return std::unexpected(unavailable("Persisting the external actor intent threw"));
    }

    auto self = std::make_shared<actor_supervisor_t>(private_t {}, std::move(store), std::move(intent), std::move(exclusion),
      std::move(body), started + std::min(options.deadline, actor_launch_deadline_limit));
    if (options.cleanup_sink) {
      self->sink_ = std::move(options.cleanup_sink);
      self->automatic_ = true;
    }

    // Settles a supervisor whose body can no longer run. Only the claimant may call it.
    const auto abandon = [&self](std::string detail) {
      const bool settled = self->settle_record();
      {
        std::lock_guard lock { self->mutex_ };
        self->decide_launch_locked({ actor_launch_e::not_launched, {}, detail });
      }
      if (settled) {
        self->finish_settled(std::move(detail));
      }
      else {
        self->finish(actor_phase_e::blocked, std::move(detail));
      }
    };

    result_t<void> published;
    try {
      published = publish(self);
    }
    catch (...) {
      published = std::unexpected(unavailable("Publishing the external actor supervisor threw"));
    }
    if (!published) {
      // Nothing was launched; the intent becomes cleanup that a later owner must discharge.
      self->claim_launch();
      abandon("The external actor supervisor was not published");
      return std::unexpected(published.error());
    }

    auto spawn = options.spawn ? std::move(options.spawn) : actor_spawn_t { [](std::function<void()> work) {
      std::thread { std::move(work) }.detach();
    } };
    try {
      spawn([self] {
        if (self->claim_launch()) self->run();
      });
    }
    catch (...) {
      if (self->claim_launch()) abandon("The external actor thread could not be started");
    }
    return self;
  }

  bool
  actor_supervisor_t::claim_launch() noexcept {
    return !claimed_.exchange(true);
  }

  void
  actor_supervisor_t::decide_launch_locked(actor_launch_result_t result) {
    if (launch_) return;
    if (std::chrono::steady_clock::now() >= deadline_) {
      result = { actor_launch_e::uncertain, {}, "The external actor's start was decided after the caller's deadline" };
    }
    launch_ = std::move(result);
    changed_.notify_all();
  }

  bool
  actor_supervisor_t::write_record(const actor_record_t &current, const actor_record_t &next) {
    try {
      if (store_->replace(current, next)) {
        std::lock_guard lock { mutex_ };
        record_ = next;
        return true;
      }
    }
    catch (...) {
    }

    // The write may have reached the disk before failing: trust only an exact match.
    actor_load_t loaded;
    try {
      loaded = store_->load();
    }
    catch (...) {
      loaded.status = actor_load_status_e::unreadable;
    }
    std::lock_guard lock { mutex_ };
    if (loaded.status == actor_load_status_e::loaded && loaded.record == next) {
      record_ = next;
      return true;
    }
    if (!(loaded.status == actor_load_status_e::loaded && loaded.record == current)) record_unknown_ = true;
    return false;
  }

  bool
  actor_supervisor_t::persist_receipt(actor_receipt_t receipt) {
    actor_record_t current;
    {
      std::lock_guard lock { mutex_ };
      if (!body_open_ || std::this_thread::get_id() != body_thread_) return false;
      if (reported_) {
        report_violation_ = true;
        return false;
      }
      reported_ = true;
      current = record_;
    }

    auto next = record_actor_receipt(current, current.operation, receipt);
    const bool durable = next && write_record(current, *next);

    std::lock_guard lock { mutex_ };
    phase_ = actor_phase_e::running;
    if (!durable) {
      decide_launch_locked({ actor_launch_e::uncertain, {}, "The external actor's receipt is not durable" });
      return false;
    }
    decide_launch_locked({ actor_launch_e::launched, receipt, {} });
    return true;
  }

  bool
  actor_supervisor_t::settle_record() {
    actor_record_t current;
    {
      std::lock_guard lock { mutex_ };
      if (record_unknown_) return false;
      current = record_;
    }
    auto next = settle_actor(current, current.operation);
    return next && write_record(current, *next);
  }

  void
  actor_supervisor_t::finish(actor_phase_e phase, std::string detail) {
    std::lock_guard lock { mutex_ };
    decide_launch_locked({ actor_launch_e::uncertain, {}, detail });
    phase_ = phase;
    detail_ = std::move(detail);
    changed_.notify_all();
  }

  void
  actor_supervisor_t::finish_settled(std::string detail) {
    std::shared_ptr<actor_cleanup_sink_t> sink;
    actor_cleanup_handoff_t handoff;
    {
      std::lock_guard lock { mutex_ };
      if (automatic_ && !handed_off_) {
        handed_off_ = true;
        sink = std::move(sink_);
        handoff = { record_, std::move(exclusion_) };
      }
    }
    // Outside the lock, so the sink may inspect the supervisor; it is noexcept.
    if (sink) sink->accept(std::move(handoff));
    finish(actor_phase_e::settled, std::move(detail));
  }

  void
  actor_supervisor_t::run() noexcept {
    {
      std::lock_guard lock { mutex_ };
      body_thread_ = std::this_thread::get_id();
      body_open_ = true;
    }
    actor_body_result_t result;
    bool threw = false;
    try {
      result = body_(context_);
    }
    catch (...) {
      threw = true;
    }

    bool reported;
    bool violation;
    {
      std::lock_guard lock { mutex_ };
      body_open_ = false;
      reported = reported_;
      violation = report_violation_;
    }
    // The body's captures belong to this thread; release them here, outside the lock.
    body_ = nullptr;

    if (threw) {
      finish(actor_phase_e::blocked, "The external actor body threw");
      return;
    }
    if (violation) {
      finish(actor_phase_e::blocked, "The external actor reported more than one launch");
      return;
    }

    switch (result.exit) {
      case actor_body_exit_e::not_launched:
        if (reported) {
          finish(actor_phase_e::blocked, "The external actor reported no launch after its receipt");
          return;
        }
        {
          std::lock_guard lock { mutex_ };
          decide_launch_locked({ actor_launch_e::not_launched, {}, result.detail });
        }
        break;
      case actor_body_exit_e::exited:
        if (!reported) {
          finish(actor_phase_e::blocked, "The external actor exited without a launch receipt");
          return;
        }
        break;
      case actor_body_exit_e::unobservable:
      default:
        finish(actor_phase_e::blocked, "The external actor can no longer be observed: " + result.detail);
        return;
    }

    if (!settle_record()) {
      finish(actor_phase_e::blocked, "The external actor's settlement is not durable");
      return;
    }
    finish_settled(result.detail);
  }

  actor_launch_result_t
  actor_supervisor_t::wait_launch() {
    std::unique_lock lock { mutex_ };
    changed_.wait_until(lock, deadline_, [&] { return launch_.has_value(); });
    decide_launch_locked({ actor_launch_e::uncertain, {}, "The external actor did not start before the caller's deadline" });
    return *launch_;
  }

  actor_supervisor_snapshot_t
  actor_supervisor_t::snapshot() const {
    std::lock_guard lock { mutex_ };
    return { phase_, record_, launch_, detail_ };
  }

  bool
  actor_supervisor_t::wait_finished(std::chrono::milliseconds timeout) const {
    std::unique_lock lock { mutex_ };
    return changed_.wait_for(lock, timeout, [&] {
      return phase_ == actor_phase_e::settled || phase_ == actor_phase_e::blocked;
    });
  }

  result_t<actor_cleanup_handoff_t>
  actor_supervisor_t::hand_off_cleanup() {
    std::lock_guard lock { mutex_ };
    if (handed_off_) {
      return std::unexpected(error_t { error_code_e::already_finalized, "The external actor's cleanup was already handed off", {} });
    }
    if (automatic_) {
      return std::unexpected(error_t { error_code_e::invalid_transition, "The external actor's cleanup is delivered to its sink", {} });
    }
    if (phase_ != actor_phase_e::settled) {
      return std::unexpected(error_t { error_code_e::invalid_transition, "Only a settled external actor can hand off its cleanup", {} });
    }
    handed_off_ = true;
    return actor_cleanup_handoff_t { record_, std::move(exclusion_) };
  }

}  // namespace display_device::topology
