#pragma once

namespace stream::session {
  enum class state_e : int {
    STOPPED,  ///< The session is stopped
    STOPPING,  ///< The session is stopping
    STARTING,  ///< The session is starting
    RUNNING,  ///< The session is running
  };
}  // namespace stream::session
