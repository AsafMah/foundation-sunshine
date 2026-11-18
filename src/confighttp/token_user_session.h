/**
 * @file src/confighttp/token_user_session.h
 * @brief Which Windows session's user may read the local host credential (local-auth.token).
 *
 * The signed-in console user is preferred. Without one (the console is at the sign-in screen because the person is connected
 * over Remote Desktop), the user of the single active interactive session gets it. Two or more active users is ambiguous and
 * grants nobody: the file is never readable by more than one user besides SYSTEM and Administrators. Session 0 (services) and
 * disconnected sessions never count, and a session whose user cannot be read makes the whole choice fail closed. Pure logic over
 * a WTS view, so it is tested with a fake one.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace confighttp::token_user_session {
  /** WTSGetActiveConsoleSessionId when no session is attached to the console. */
  inline constexpr std::uint32_t no_session = 0xFFFFFFFF;

  struct session_t {
    std::uint32_t id = 0;
    bool active = false;  ///< WTSActive: attached to the console or a connected Remote Desktop client
    bool has_user = false;  ///< a user is signed in to the session
    bool known = true;  ///< false when the session's user could not be read
  };

  struct wts_view_t {
    std::function<std::uint32_t()> console_session;
    std::function<std::vector<session_t>()> sessions;
  };

  inline std::optional<std::uint32_t>
  pick(const wts_view_t &wts) {
    const auto console = wts.console_session();
    const auto sessions = wts.sessions();
    for (const auto &session : sessions) {
      if (!session.known) return std::nullopt;
    }
    if (console != no_session) {
      for (const auto &session : sessions) {
        if (session.id == console && session.has_user) return console;
      }
    }
    std::optional<std::uint32_t> found;
    for (const auto &session : sessions) {
      if (session.id == 0 || !session.active || !session.has_user) continue;
      if (found) return std::nullopt;
      found = session.id;
    }
    return found;
  }
}  // namespace confighttp::token_user_session
