/**
 * @file src/file_upload_routes.h
 * @brief Wires file_upload_api into the nvhttp (paired client) and confighttp (local agent) servers.
 */
#pragma once

#include <functional>
#include <memory>

#include "src/http/server.h"

namespace file_upload_routes {
  /// Creates the process-wide upload store; its staging directory under appdata is wiped.
  void
  init();

  /// Paired-client request (identity = TLS certificate client UUID).
  void
  handle_client(web::detail::ResponseBuffer &response, const web::Request &request);
  /// Local GUI agent request; the caller has already authenticated it.
  void
  handle_agent(web::detail::ResponseBuffer &response, const web::Request &request);

  inline constexpr const char *kClientPattern = "^/api/v1/file-transfer/(batches|uploads)(/[a-f0-9]{64})?(/commit)?$";
  inline constexpr const char *kAgentPattern = "^/api/v1/file-transfer/manifests/[a-f0-9]{64}(/items/[0-9]{1,4})?$";

  template <class Server>
  void
  register_client_routes(Server &server) {
    for (const char *method : { "GET", "POST", "PUT", "DELETE" }) {
      server.resource[kClientPattern][method] = [](auto response, auto request) {
        handle_client(*response, *request);
      };
    }
  }

  template <class Server, class Auth>
  void
  register_agent_routes(Server &server, Auth auth) {
    auto shared_auth = std::make_shared<Auth>(std::move(auth));
    for (const char *method : { "GET", "DELETE" }) {
      server.resource[kAgentPattern][method] = [shared_auth](auto response, auto request) {
        if ((*shared_auth)(response, request)) {
          handle_agent(*response, *request);
        }
      };
    }
  }
}  // namespace file_upload_routes
