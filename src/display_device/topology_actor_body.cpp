#include "topology_actor_body.h"

#include <exception>
#include <utility>

namespace display_device::topology {

  namespace {
    std::string
    exception_detail(std::exception_ptr error) {
      try {
        std::rethrow_exception(error);
      }
      catch (const std::exception &e) {
        return e.what();
      }
      catch (...) {
        return "unknown exception";
      }
    }

    actor_body_result_t
    run_process(actor_body_context_t &context, const actor_process_factory_t &make) {
      std::shared_ptr<actor_process_t> process;
      try {
        if (make) process = make();
        if (!process) return { actor_body_exit_e::not_launched, "no actor process" };
        context.retain(process);
      }
      catch (...) {
        return { actor_body_exit_e::not_launched, "before launch: " + exception_detail(std::current_exception()) };
      }

      std::error_code ec;
      std::string launch_detail;
      try {
        process->launch(ec);
      }
      catch (...) {
        launch_detail = exception_detail(std::current_exception());
      }
      if (!process->started()) {
        if (launch_detail.empty()) launch_detail = ec ? ec.message() : "creation refused";
        return { actor_body_exit_e::not_launched, launch_detail };
      }
      if (launch_detail.empty() && ec) launch_detail = ec.message();
      const auto note = launch_detail.empty() ? std::string {} : "; after creation: " + launch_detail;

      const auto receipt = process->receipt();
      if (!receipt) return { actor_body_exit_e::unobservable, "creation time unavailable" + note };
      context.report_launched(*receipt);

      while (true) {
        const auto observed = process->wait();
        switch (observed.state) {
          case actor_wait_e::running:
            continue;
          case actor_wait_e::exited:
            return { actor_body_exit_e::exited,
              (observed.exit_code ? "exit code " + std::to_string(*observed.exit_code) : "exit code unavailable: " + observed.detail) + note };
          case actor_wait_e::failed:
          default:
            return { actor_body_exit_e::unobservable, "observation failed: " + observed.detail + note };
        }
      }
    }
  }  // namespace

  actor_body_t
  make_process_actor_body(actor_process_factory_t make) {
    return [make = std::move(make)](actor_body_context_t &context) {
      return run_process(context, make);
    };
  }

}  // namespace display_device::topology
