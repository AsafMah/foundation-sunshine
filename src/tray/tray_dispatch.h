/**
 * @file src/tray/tray_dispatch.h
 * @brief Runs blocking tray actions away from the tray thread.
 */
#pragma once

#include <exception>
#include <string_view>
#include <thread>
#include <utility>

#include "src/logging.h"

namespace system_tray {
  /**
   * Blocking topology-coordinator calls must not run on the tray thread: worker bodies
   * update the tray menu through tray_update(), which synchronously SendMessages the
   * tray window and would wait forever for a tray thread blocked on that worker.
   */
  template <class F>
  void
  run_off_tray_thread(const char *action, F &&work) {
    using namespace std::literals;
    try {
      std::thread([action, work = std::forward<F>(work)]() mutable {
        try {
          work();
        }
        catch (const std::exception &e) {
          BOOST_LOG(error) << action << " failed: "sv << e.what();
        }
        catch (...) {
          BOOST_LOG(error) << action << " failed with an unknown exception"sv;
        }
      }).detach();
    }
    catch (const std::exception &e) {
      BOOST_LOG(error) << action << " could not start: "sv << e.what();
    }
  }
}  // namespace system_tray
