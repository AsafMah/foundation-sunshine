#include "src/nvhttp/abr_request.h"
#include "src/streaming/abr_session.h"
#include <gtest/gtest.h>

TEST(AbrSessionIdentity, MappedIpv4FixtureUsesCertificateNotAddress) {
  // Reported HTTPS peer: 84.229.28.83; RTSP/ENet peer: ::ffff:84.229.28.83.
  std::vector<abr::session_candidate_t> sessions {
    { "paired-a", "generation-a", true, false, false },
  };
  EXPECT_EQ(abr::select_session(sessions, "84.229.28.83").error, abr::session_error_e::no_active_stream);
  EXPECT_EQ(abr::select_session(sessions, "paired-a").error, abr::session_error_e::none);
}

TEST(AbrSessionIdentity, SameCertificateOverAnotherRoute) {
  std::vector<abr::session_candidate_t> sessions {
    { "paired-a", "generation-a", true, false, false },
  };
  EXPECT_EQ(abr::select_session(sessions, "paired-a").error, abr::session_error_e::none);
  EXPECT_EQ(abr::select_session(sessions, "192.168.1.2").error, abr::session_error_e::no_active_stream);
}

TEST(AbrSessionIdentity, SharedNatDoesNotSelectAnotherClient) {
  std::vector<abr::session_candidate_t> sessions {
    { "paired-b", "generation-b", true, false, false },
    { "paired-a", "generation-a", true, false, false },
  };
  const auto selected = abr::select_session(sessions, "paired-a");
  EXPECT_EQ(selected.error, abr::session_error_e::none);
  EXPECT_EQ(selected.index, 1u);
}

TEST(AbrSessionIdentity, MissingStartingControlOnlyAndAmbiguityFailClosed) {
  using error = abr::session_error_e;
  std::vector<abr::session_candidate_t> sessions {
    { "paired-a", "control", true, false, true },
    { "paired-a", "starting", false, true, false },
  };
  EXPECT_EQ(abr::select_session(sessions, "").error, error::missing_client_identity);
  EXPECT_EQ(abr::select_session(sessions, "paired-b").error, error::no_active_stream);
  EXPECT_EQ(abr::select_session(sessions, "paired-a").error, error::session_starting);
  sessions.push_back({ "paired-a", "running", true, false, false });
  EXPECT_EQ(abr::select_session(sessions, "paired-a").error, error::ambiguous_session);
  EXPECT_EQ(abr::select_session(sessions, "paired-a", "running").error, error::ambiguous_session);
  sessions[1].starting = false;
  EXPECT_EQ(abr::select_session(sessions, "paired-a", "running").error, error::none);
  sessions.push_back({ "paired-a", "another", true, false, false });
  EXPECT_EQ(abr::select_session(sessions, "paired-a").error, error::ambiguous_session);
}

TEST(AbrSessionIdentity, ReplacementRejectsOldTokenAndAnotherCertificatesToken) {
  std::vector<abr::session_candidate_t> sessions {
    { "paired-a", "new-generation", true, false, false },
    { "paired-b", "old-generation", true, false, false },
  };
  EXPECT_EQ(abr::select_session(sessions, "paired-a", "old-generation").error, abr::session_error_e::stale_session);
  EXPECT_EQ(abr::select_session(sessions, "paired-a", "new-generation").error, abr::session_error_e::none);
  EXPECT_EQ(abr::select_session({}, "paired-a", "new-generation").error, abr::session_error_e::stale_session);
}

TEST(AbrRequestContract, InvalidBodiesCannotDisableOrSilentlyCoerceMetrics) {
  using nlohmann::json;
  using nvhttp::abr_api::parse_request;
  for (const auto &body : std::vector<json> {
         nullptr,
         json::array(),
         json::object(),
         { { "enabled", 1 } },
         { { "enabled", false } },
         { { "enabled", true }, { "minBitrate", -1 } },
         { { "enabled", true }, { "maxBitrate", 1.5 } },
         { { "enabled", true }, { "maxBitrate", 2147483648ULL } },
         { { "enabled", true }, { "minBitrate", 20000 }, { "maxBitrate", 10000 } },
         { { "enabled", true }, { "mode", "unknown" } },
       }) {
    SCOPED_TRACE(body.dump());
    EXPECT_THROW(parse_request(body, false), std::exception);
  }
  for (const auto &body : std::vector<json> {
         json::object(),
         { { "sessionToken", "" } },
         { { "sessionToken", 1 } },
         { { "sessionToken", "s" }, { "packetLoss", 101 } },
         { { "sessionToken", "s" }, { "rttMs", -1 } },
         { { "sessionToken", "s" }, { "decodeFps", "NaN" } },
         { { "sessionToken", "s" }, { "droppedFrames", 0.5 } },
         { { "sessionToken", "s" }, { "packetLoss", std::numeric_limits<double>::infinity() } },
       }) {
    SCOPED_TRACE(body.dump());
    EXPECT_THROW(parse_request(body, true), std::exception);
  }
}

TEST(AbrRequestContract, SuccessAndRetryContractIsTypedAndBound) {
  using nvhttp::abr_api::parse_request;
  const auto enable = parse_request({ { "enabled", true }, { "minBitrate", 0 }, { "maxBitrate", 80000 }, { "mode", "balanced" } }, false);
  EXPECT_TRUE(enable.token.empty());
  EXPECT_EQ(enable.config.max_bitrate_kbps, 80000);
  const auto disable = parse_request({ { "enabled", false }, { "sessionToken", "s" } }, false);
  EXPECT_EQ(disable.operation, abr::operation_e::disable);
  const auto feedback = parse_request({ { "sessionToken", "s" }, { "packetLoss", 6.0 }, { "currentBitrate", 20000 } }, true);
  EXPECT_EQ(feedback.token, "s");
  EXPECT_EQ(feedback.feedback.current_bitrate_kbps, 20000);
  for (auto error : { abr::session_error_e::no_active_stream, abr::session_error_e::session_starting }) {
    EXPECT_TRUE(nvhttp::abr_api::error_contract(error, true).retryable);
    EXPECT_FALSE(nvhttp::abr_api::error_contract(error, false).retryable);
  }
  EXPECT_FALSE(nvhttp::abr_api::error_contract(abr::session_error_e::ambiguous_session, true).retryable);
  EXPECT_EQ(nvhttp::abr_api::error_contract(abr::session_error_e::missing_client_identity, true).status, 403);
}

TEST(AbrControllerOwnership, SameNameReplacementAndOldCleanupStayIsolated) {
  abr::config_t config { true, 2000, 50000, abr::mode_e::BALANCED };
  // Both devices may have display name "asaf"; it is never a controller key.
  abr::enable("old-allocation", config, 20000, "app");
  abr::enable("new-allocation", config, 40000, "app");
  abr::network_feedback_t feedback { 6.0, 0, 0, 0, 20000 };
  EXPECT_EQ(abr::process_feedback("old-allocation", feedback).new_bitrate_kbps, 14000);
  abr::cleanup("old-allocation");
  EXPECT_FALSE(abr::is_enabled("old-allocation"));
  EXPECT_TRUE(abr::is_enabled("new-allocation"));
  feedback.current_bitrate_kbps = 40000;
  EXPECT_EQ(abr::process_feedback("new-allocation", feedback).new_bitrate_kbps, 28000);
  abr::cleanup("new-allocation");
}

TEST(AbrRequestContract, LargestAcceptedIntegerCannotOverflowModePresets) {
  const auto request = nvhttp::abr_api::parse_request({ { "enabled", true }, { "maxBitrate", 0 } }, false);
  const auto effective = abr::enable("large-budget", request.config, std::numeric_limits<int>::max(), "app");
  EXPECT_EQ(effective.max_bitrate_kbps, 150000);
  EXPECT_GE(effective.min_bitrate_kbps, 0);
  EXPECT_LE(effective.min_bitrate_kbps, effective.max_bitrate_kbps);
  abr::cleanup("large-budget");
}
