/**
 * @file src/platform/windows/topology_actor_process.h
 * @brief Windows external topology actor launched through its original handles.
 */
#pragma once

#include <memory>
#include <string>

#include "src/display_device/topology_actor_body.h"
#include "src/platform/run_command.h"

namespace platf {

  struct actor_launch_spec_t {
    std::string cmd;
    boost::filesystem::path working_dir;
    boost::process::v1::environment env;
    bool elevated {};
    bool interactive {};
  };

  /**
   * An actor process backed by `owned_process_t`. Its operations must stay on one
   * thread (the supervisor's body thread); destruction only closes the original
   * handles and may happen on any thread. It never terminates the child.
   */
  std::shared_ptr<display_device::topology::actor_process_t>
  make_owned_actor_process(actor_launch_spec_t spec);

}  // namespace platf
