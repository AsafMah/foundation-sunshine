#include "abr_api.h"
#include "abr_request.h"

#include "src/ai/proxy.h"
#include "src/config.h"
#include "src/http/server.h"
#include "src/logging.h"
#include <algorithm>

using json = nlohmann::json;

namespace nvhttp::abr_api {
  namespace {
    web::CaseInsensitiveMultimap
    json_headers() {
      return { { "Content-Type", "application/json" } };
    }

    void
    write_error(resp_https_t response, int status, const char *code, const char *message, bool retryable = false) {
      BOOST_LOG(warning) << "ABR request rejected: " << code << ": " << message;
      response->write(static_cast<web::StatusCode>(status),
        json { { "success", false }, { "error_code", code }, { "error", message }, { "retryable", retryable } }.dump(),
        json_headers());
    }

    int
    host_max_bitrate_kbps() {
      return std::max(config::video.max_bitrate, 0);
    }

    const char *
    mode_name(abr::mode_e mode) {
      switch (mode) {
        case abr::mode_e::QUALITY:
          return "quality";
        case abr::mode_e::LOW_LATENCY:
          return "lowLatency";
        default:
          return "balanced";
      }
    }

    void
    handle_request(resp_https_t response, req_https_t request, bool feedback) {
      const auto uuid = nvhttp::get_client_cert_uuid_from_request(request);
      if (uuid.empty()) {
        write_error(response, 403, "missing_client_identity", "Authenticated paired-client identity is required");
        return;
      }
      try {
        auto parsed = parse_request(json::parse(request->content.string()), feedback);
        const int requested_max = parsed.config.max_bitrate_kbps;
        const int host_max = host_max_bitrate_kbps();
        if (parsed.operation == abr::operation_e::configure && host_max > 0) {
          parsed.config.max_bitrate_kbps = requested_max > 0 ? std::min(requested_max, host_max) : host_max;
          parsed.config.min_bitrate_kbps = std::min(parsed.config.min_bitrate_kbps, parsed.config.max_bitrate_kbps);
        }
        const auto result = stream::session::apply_abr_for_client(uuid, parsed);
        if (result.error != abr::session_error_e::none) {
          const auto error = error_contract(result.error,
            parsed.operation == abr::operation_e::configure && parsed.token.empty());
          write_error(response, error.status, error.code, error.message, error.retryable);
          return;
        }
        json body { { "success", true }, { "sessionToken", result.token } };
        if (parsed.operation == abr::operation_e::feedback) {
          if (result.action.new_bitrate_kbps > 0 && result.bitrate_requested) {
            body["newBitrate"] = result.action.new_bitrate_kbps;
          }
          body["reason"] = result.action.reason;
        }
        else {
          body["enabled"] = parsed.operation == abr::operation_e::configure;
          if (parsed.operation == abr::operation_e::configure) {
            body["mode"] = mode_name(result.config.mode);
            body["minBitrate"] = result.config.min_bitrate_kbps;
            body["maxBitrate"] = result.config.max_bitrate_kbps;
            body["initialBitrate"] = result.initial_bitrate_kbps;
            body["requestedMaxBitrate"] = requested_max;
            body["hostMaxBitrate"] = host_max;
            body["maxBitrateCapped"] = host_max > 0 && requested_max > host_max;
            body["maxBitrateInheritedFromHost"] = host_max > 0 && requested_max == 0;
          }
        }
        if (parsed.operation != abr::operation_e::disable) {
          // Legacy field means accepted/no change, not confirmed encoder commit.
          body["bitrateApplied"] = true;
          body["bitrateRequested"] = result.bitrate_requested;
        }
        response->write(web::StatusCode::ok, body.dump(), json_headers());
      }
      catch (const json::exception &e) {
        BOOST_LOG(warning) << "ABR JSON validation: " << e.what();
        write_error(response, 400, "invalid_body", "Invalid JSON body or field type");
      }
      catch (const std::invalid_argument &e) {
        write_error(response, 400, "invalid_body", e.what());
      }
      catch (const std::exception &e) {
        BOOST_LOG(error) << "ABR request failed: " << e.what();
        write_error(response, 500, "internal_error", "ABR request failed");
      }
    }
  }  // namespace

  void
  capabilities(resp_https_t response, req_https_t request) {
    const auto caps = abr::get_capabilities();
    json body {
      { "supported", caps.supported },
      { "version", caps.version },
      { "features", json::array({ "llm_ai", "game_aware", "fallback_threshold", "bitrate_cap", "session_binding" }) },
      { "llmEnabled", ai::isAiEnabled() },
      { "hostMaxBitrate", host_max_bitrate_kbps() },
    };
    response->write(web::StatusCode::ok, body.dump(), json_headers());
  }

  void
  configure(resp_https_t response, req_https_t request) {
    handle_request(response, request, false);
  }

  void
  feedback(resp_https_t response, req_https_t request) {
    handle_request(response, request, true);
  }
}  // namespace nvhttp::abr_api
