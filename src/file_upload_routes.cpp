/**
 * @file src/file_upload_routes.cpp
 * @brief See file_upload_routes.h.
 */
#include "file_upload_routes.h"

#include <charconv>

#include "config.h"
#include "file_upload_api.h"
#include "file_upload_store.h"
#include "logging.h"
#include "platform/common.h"
#include "stream.h"

namespace file_upload_routes {
  namespace {
    std::vector<std::uint32_t>
    running_sessions(const std::string &client_uuid) {
      std::vector<std::uint32_t> out;
      for (const auto &info : stream::session::get_all_sessions_info()) {
        if (info.client_uuid == client_uuid && info.state == "RUNNING") {
          out.push_back(info.session_id);
        }
      }
      return out;
    }

    file_upload_api::request_t
    to_request(const web::Request &request, std::string &body_storage) {
      file_upload_api::request_t out;
      out.method = request.method;
      out.path = request.path;
      out.query = request.query_string;
      if (const auto it = request.header.find("Content-Length"); it != request.header.end()) {
        std::uint64_t length = 0;
        const auto &v = it->second;
        const auto [end, ec] = std::from_chars(v.data(), v.data() + v.size(), length);
        if (ec == std::errc {} && end == v.data() + v.size()) {
          out.content_length = length;
        }
      }
      body_storage = request.content.string();
      out.body = body_storage;
      return out;
    }

    void
    write_reply(web::detail::ResponseBuffer &response, const file_upload_api::reply_t &reply) {
      web::CaseInsensitiveMultimap headers;
      for (const auto &[k, v] : reply.headers) {
        headers.emplace(k, v);
      }
      const auto status = static_cast<web::StatusCode>(reply.status);
      if (!reply.file.empty()) {
        if (!response.write_file(status, reply.file, headers)) {
          web::CaseInsensitiveMultimap err_headers;
          err_headers.emplace("Content-Type", "application/json");
          response.write(web::StatusCode::not_found, R"({"error":"not_found"})", err_headers);
        }
        return;
      }
      response.write(status, reply.body, headers);
    }
  }  // namespace

  void
  init() {
    file_upload_store::options_t opt;
    opt.staging_dir = platf::appdata() / "file_transfer" / "uploads";
    opt.sessions = running_sessions;
    opt.free_space = [](const std::filesystem::path &path) {
      std::error_code ec;
      const auto info = std::filesystem::space(path, ec);
      return ec ? std::uint64_t { 0 } : static_cast<std::uint64_t>(info.available);
    };
    file_upload_store::init(std::move(opt));
    BOOST_LOG(info) << "FileUpload: staging ready (client uploads " << (config::input.file_upload ? "enabled" : "disabled") << ')';
  }

  void
  handle_client(web::detail::ResponseBuffer &response, const web::Request &request) {
    std::string body;
    const auto reply = file_upload_api::handle_client(file_upload_store::instance(), config::input.file_upload, request.client_uuid, to_request(request, body));
    if (reply.status >= 400 && reply.status != 409) {
      BOOST_LOG(debug) << "FileUpload: " << request.method << ' ' << request.path << " -> " << reply.status << ' ' << reply.body;
    }
    write_reply(response, reply);
  }

  void
  handle_agent(web::detail::ResponseBuffer &response, const web::Request &request) {
    std::string body;
    write_reply(response, file_upload_api::handle_agent(file_upload_store::instance(), config::input.file_upload, to_request(request, body)));
  }
}  // namespace file_upload_routes
