#pragma once

#include "abr.h"
#include "bitrate.h"
#include <cstddef>
#include <string>
#include <vector>

namespace abr {
  enum class session_error_e {
    none,
    missing_client_identity,
    no_active_stream,
    session_starting,
    ambiguous_session,
    stale_session,
    abr_not_enabled,
  };

  struct session_candidate_t {
    std::string client_uuid;
    std::string token;
    bool running;
    bool starting;
    bool control_only;
  };

  struct session_selection_t {
    session_error_e error = session_error_e::no_active_stream;
    std::size_t index = 0;
  };

  inline session_selection_t
  select_session(const std::vector<session_candidate_t> &sessions, const std::string &client_uuid,
    const std::string &expected_token = {}) {
    if (client_uuid.empty()) {
      return { session_error_e::missing_client_identity };
    }
    session_selection_t selected;
    std::size_t count = 0;
    for (std::size_t i = 0; i < sessions.size(); ++i) {
      const auto &candidate = sessions[i];
      if (candidate.client_uuid == client_uuid && !candidate.control_only &&
          (candidate.running || candidate.starting)) {
        ++count;
        selected = { candidate.running ? session_error_e::none : session_error_e::session_starting, i };
      }
    }
    if (count > 1) {
      return { session_error_e::ambiguous_session };
    }
    if (!expected_token.empty() &&
        (count == 0 || sessions[selected.index].token != expected_token)) {
      return { session_error_e::stale_session };
    }
    return selected;
  }

  enum class operation_e { configure,
    disable,
    feedback };

  struct session_request_t {
    operation_e operation = operation_e::configure;
    std::string token;
    config_t config;
    network_feedback_t feedback {};
  };

  struct session_result_t {
    session_error_e error = session_error_e::no_active_stream;
    std::string token;
    config_t config;
    int initial_bitrate_kbps = 0;
    action_t action;
    bool bitrate_requested = false;
    int requested_bitrate_kbps = 0;
  };

  session_result_t
  apply_session_request(const std::string &token, const std::string &app_name,
    streaming::bitrate_budget_t &budget, const session_request_t &request, int host_max_bitrate_kbps = 0);

  // The selected reference is used only inside the list lock. Both callbacks
  // return values; neither may retain a session pointer past this operation.
  template <class SessionList, class Describe, class Apply>
  session_result_t
  with_authenticated_session(SessionList &session_list, const std::string &uuid,
    const session_request_t &request, Describe describe, Apply apply) {
    if (uuid.empty()) {
      return { .error = session_error_e::missing_client_identity };
    }
    if (request.operation != operation_e::configure && request.token.empty()) {
      return { .error = session_error_e::stale_session };
    }
    auto lock = session_list.lock();
    std::vector<session_candidate_t> candidates;
    for (const auto session : *session_list) {
      candidates.push_back(session ? describe(*session) : session_candidate_t { {}, {}, false, false, true });
    }
    const auto selected = select_session(candidates, uuid, request.token);
    if (selected.error != session_error_e::none) {
      return { .error = selected.error };
    }
    return apply(*(*session_list)[selected.index]);
  }
}  // namespace abr

namespace stream::session {
  // Selection, controller mutation and budget requests share the session-list
  // lock. ABR may take its own mutex inside this lock, never in reverse order.
  abr::session_result_t
  apply_abr_for_client(const std::string &client_cert_uuid, const abr::session_request_t &request);
}  // namespace stream::session
