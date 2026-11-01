#pragma once

#include "src/display_device/topology_coordinator.h"

#include <gtest/gtest.h>

namespace topology_test {
  inline void
  with_capability(display_device::topology::command_kind_e kind, bool managed,
    const std::function<void(const display_device::topology::mutation_capability_t &)> &body) {
    namespace topology = display_device::topology;
    topology::coordinator_options_t options;
    options.managed_mode = managed;
    options.probes.startup_restore_quiescent = [] { return true; };
    if (managed) {
      options.probes.active_streams = [] { return std::size_t {}; };
      options.native_restore = [](const auto &, const auto &) {};
    }
    topology::coordinator_t coordinator { std::move(options) };
    int calls {};
    const topology::command_body_t wrapped = [&](const auto &capability, const auto &) {
      ++calls;
      body(capability);
    };
    topology::command_result_t result;
    if (kind == topology::command_kind_e::startup_recovery) {
      result = coordinator.start(wrapped).get();
    }
    else if (kind == topology::command_kind_e::legacy_startup_restore) {
      ASSERT_TRUE(coordinator.start([&](const auto &capability, const auto &) {
                               coordinator.defer_startup_restore(capability, wrapped);
                             })
          .get()
          .completed());
      result = coordinator.retry_startup_restore().completion.get();
    }
    else {
      ASSERT_TRUE(coordinator.start({}).get().completed());
      if (kind == topology::command_kind_e::launch_prepare) {
        const auto launch = coordinator.admit_launch({ 1, "mutation-boundary" });
        ASSERT_TRUE(launch);
        result = coordinator.run_launch_prepare(*launch.token, wrapped);
      }
      else {
        result = coordinator.run(kind, wrapped);
      }
    }
    EXPECT_TRUE(result.completed());
    EXPECT_EQ(calls, 1);
    coordinator.stop();
  }
}  // namespace topology_test
