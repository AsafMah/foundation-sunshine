#pragma once

#include "src/streaming/abr_session.h"
#include <cmath>
#include <limits>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace nvhttp::abr_api {
  inline void
  require_body(bool condition, const char *message) {
    if (!condition) {
      throw std::invalid_argument(message);
    }
  }

  inline int
  nonnegative_integer(const nlohmann::json &body, const char *field) {
    if (!body.contains(field)) {
      return 0;
    }
    const auto &value = body.at(field);
    require_body(value.is_number_integer(), "Expected an integer metric or bitrate");
    require_body(value >= 0 && value <= std::numeric_limits<int>::max(), "Integer is outside the supported range");
    return value.get<int>();
  }

  inline double
  nonnegative_number(const nlohmann::json &body, const char *field) {
    if (!body.contains(field)) {
      return 0;
    }
    const auto &value = body.at(field);
    require_body(value.is_number(), "Expected a numeric feedback metric");
    const auto number = value.get<double>();
    require_body(std::isfinite(number) && number >= 0, "Feedback metrics must be finite and non-negative");
    return number;
  }

  inline abr::session_request_t
  parse_request(const nlohmann::json &body, bool feedback) {
    require_body(body.is_object(), "Expected a JSON object");
    abr::session_request_t request;
    if (body.contains("sessionToken")) {
      require_body(body.at("sessionToken").is_string(), "sessionToken must be a string");
      request.token = body.at("sessionToken").get<std::string>();
      require_body(!request.token.empty() && request.token.size() <= 128, "Invalid sessionToken");
    }
    if (feedback) {
      request.operation = abr::operation_e::feedback;
      require_body(!request.token.empty(), "Feedback requires sessionToken");
      request.feedback = {
        nonnegative_number(body, "packetLoss"),
        nonnegative_number(body, "rttMs"),
        nonnegative_number(body, "decodeFps"),
        nonnegative_integer(body, "droppedFrames"),
        nonnegative_integer(body, "currentBitrate"),
      };
      require_body(request.feedback.packet_loss <= 100, "packetLoss must not exceed 100");
      return request;
    }
    require_body(body.contains("enabled") && body.at("enabled").is_boolean(), "enabled must be a boolean");
    if (!body.at("enabled").get<bool>()) {
      request.operation = abr::operation_e::disable;
      require_body(!request.token.empty(), "Disable requires sessionToken");
      return request;
    }
    request.config.enabled = true;
    request.config.min_bitrate_kbps = nonnegative_integer(body, "minBitrate");
    request.config.max_bitrate_kbps = nonnegative_integer(body, "maxBitrate");
    require_body(request.config.max_bitrate_kbps == 0 ||
                   request.config.min_bitrate_kbps <= request.config.max_bitrate_kbps,
      "minBitrate must not exceed maxBitrate");
    const auto mode = body.value("mode", std::string { "balanced" });
    if (mode == "balanced") {
      request.config.mode = abr::mode_e::BALANCED;
    }
    else if (mode == "quality") {
      request.config.mode = abr::mode_e::QUALITY;
    }
    else if (mode == "lowLatency") {
      request.config.mode = abr::mode_e::LOW_LATENCY;
    }
    else {
      throw std::invalid_argument("Invalid ABR mode");
    }
    return request;
  }

  struct session_error_contract_t {
    int status;
    const char *code;
    const char *message;
    bool retryable;
  };

  inline session_error_contract_t
  error_contract(abr::session_error_e error, bool unbound_configure) {
    switch (error) {
      case abr::session_error_e::missing_client_identity:
        return { 403, "missing_client_identity", "Authenticated paired-client identity is required", false };
      case abr::session_error_e::session_starting:
        return { 409, "session_starting", "Video session is starting", unbound_configure };
      case abr::session_error_e::ambiguous_session:
        return { 409, "ambiguous_session", "Multiple video sessions belong to this client", false };
      case abr::session_error_e::stale_session:
        return { 409, "stale_session", "Session token no longer identifies this client's active stream", false };
      case abr::session_error_e::abr_not_enabled:
        return { 409, "abr_not_enabled", "ABR is not enabled for this session", false };
      case abr::session_error_e::no_active_stream:
        return { 409, "no_active_stream", "No active video stream for this client", unbound_configure };
      case abr::session_error_e::none:
        throw std::logic_error("Successful ABR selection has no error contract");
    }
    throw std::logic_error("Unknown ABR session error");
  }
}  // namespace nvhttp::abr_api
