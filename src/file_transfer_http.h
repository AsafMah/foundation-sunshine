/**
 * @file src/file_transfer_http.h
 * @brief HTTP helpers for host-to-client file-transfer offers.
 */
#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <sstream>
#include <string>

#include "src/http/server.h"

namespace file_transfer_http {
  using https_server_t = web::Server<web::HTTPS>;
  using resp_https_t = std::shared_ptr<typename web::ServerBase<web::HTTPS>::Response>;
  using req_https_t = std::shared_ptr<typename web::ServerBase<web::HTTPS>::Request>;
  using auth_fn = std::function<bool(resp_https_t, req_https_t)>;

  struct response_t {
    web::StatusCode status;
    std::string body;
    web::CaseInsensitiveMultimap headers;
  };

  struct download_response_t {
    web::StatusCode status;
    std::string body;
    web::CaseInsensitiveMultimap headers;
    std::filesystem::path path;
    bool stream_file;
  };

  response_t make_offer_response(const std::string &body);
  download_response_t make_download_response(const std::string &id);

  template <typename Request>
  inline response_t process_offer(const Request &req) {
    std::stringstream ss;
    ss << req->content.rdbuf();
    return make_offer_response(ss.str());
  }

  template <typename Request>
  inline download_response_t process_download(const Request &req) {
    const std::string id = req->path_match.size() >= 2 ? req->path_match[1].str() : std::string {};
    return make_download_response(id);
  }

  template <typename Response>
  inline void write_download_response(const Response &resp, download_response_t out) {
    if (!out.stream_file) {
      resp->write(out.status, out.body, out.headers);
      return;
    }

    // Offers can be up to 4 GiB: stream from disk instead of the buffered
    // istream overload, which caps bodies at the response buffer size.
    if (!resp->write_file(out.status, out.path, out.headers)) {
      web::CaseInsensitiveMultimap err_headers;
      err_headers.emplace("Content-Type", "application/json");
      err_headers.emplace("Cache-Control", "no-store");
      resp->write(web::StatusCode::not_found,
        R"({"error":"not_found"})", err_headers);
    }
  }

  void register_routes(https_server_t &server, auth_fn auth);
}  // namespace file_transfer_http
