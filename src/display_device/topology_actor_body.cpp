#include "topology_actor_body.h"

#include <algorithm>
#include <exception>
#include <string>
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
  make_process_actor_body(actor_process_factory_t make, actor_launch_retry_t retry) {
    return [make = std::move(make), retry = std::move(retry)](actor_body_context_t &context) {
      const unsigned attempts = std::max(retry.attempts, 1u);
      if (attempts == 1) return run_process(context, make);
      std::string history;
      for (unsigned attempt = 0;; ++attempt) {
        auto result = run_process(context, make);
        if (result.exit != actor_body_exit_e::not_launched) {
          if (!history.empty()) result.detail = history + "; " + result.detail;
          return result;
        }
        if (!history.empty()) history += "; ";
        history += "attempt " + std::to_string(attempt + 1) + '/' + std::to_string(attempts) + ": " + result.detail;
        if (attempt + 1 == attempts) return actor_body_result_t { actor_body_exit_e::not_launched, history };
        try {
          if (retry.wait) retry.wait(attempt);
        }
        catch (...) {
          return actor_body_result_t { actor_body_exit_e::not_launched, history + "; backoff: " + exception_detail(std::current_exception()) };
        }
      }
    };
  }

}  // namespace display_device::topology
