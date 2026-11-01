#include "topology_actor_process.h"

#include "owned_process.h"

namespace platf {

  namespace {
    using display_device::topology::actor_receipt_t;
    using display_device::topology::actor_wait_e;
    using display_device::topology::actor_wait_t;

    class owned_actor_process_t final: public display_device::topology::actor_process_t {
    public:
      explicit owned_actor_process_t(actor_launch_spec_t spec):
          spec_(std::move(spec)) {}

      void
      launch(std::error_code &ec) override {
        process_.launch(spec_.elevated, spec_.interactive, spec_.cmd, spec_.working_dir, spec_.env, nullptr, ec, nullptr);
      }

      bool
      started() const override {
        return process_.started();
      }

      std::optional<actor_receipt_t>
      receipt() override {
        if (!process_.started()) return std::nullopt;
        const auto created = process_.creation_time();
        if (!created) return std::nullopt;
        return actor_receipt_t { process_.id(), *created };
      }

      actor_wait_t
      wait() override {
        const auto observed = process_.observe(INFINITE);
        switch (observed.state) {
          case process_observation_e::running:
            return { actor_wait_e::running, {}, {} };
          case process_observation_e::exited:
            return { actor_wait_e::exited, observed.exit_code,
              observed.error ? observed.error.message() : std::string {} };
          case process_observation_e::failed:
            return { actor_wait_e::failed, {}, observed.error ? observed.error.message() : "wait failed" };
          case process_observation_e::not_started:
          default:
            return { actor_wait_e::failed, {}, "not started" };
        }
      }

    private:
      actor_launch_spec_t spec_;
      owned_process_t process_;
    };
  }  // namespace

  std::shared_ptr<display_device::topology::actor_process_t>
  make_owned_actor_process(actor_launch_spec_t spec) {
    return std::make_shared<owned_actor_process_t>(std::move(spec));
  }

}  // namespace platf
