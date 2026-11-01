#include "src/nvhttp/abr_request.h"
#include "src/nvhttp/abr_rejection_log_limiter.h"
#include "src/streaming/abr_session.h"
#include <boost/log/core.hpp>
#include <boost/log/sinks/sync_frontend.hpp>
#include <boost/log/sinks/text_ostream_backend.hpp>
#include <boost/make_shared.hpp>
#include <chrono>
#include <gtest/gtest.h>
#include <sstream>
#include <string>

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
  EXPECT_EQ(abr::select_session(sessions, "paired-a", "running").error, error::none);
  sessions[1].starting = false;
  EXPECT_EQ(abr::select_session(sessions, "paired-a", "running").error, error::none);
  sessions.push_back({ "paired-a", "another", true, false, false });
  EXPECT_EQ(abr::select_session(sessions, "paired-a").error, error::ambiguous_session);
  EXPECT_EQ(abr::select_session(sessions, "paired-a", "running").error, error::none);
  EXPECT_EQ(abr::select_session(sessions, "paired-a", "another").error, error::none);
  EXPECT_EQ(abr::select_session(sessions, "paired-a", "stale").error, error::stale_session);
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

TEST(AbrSessionIdentity, BoundTokenRequiresItsOwnRunningNonControlAllocation) {
  using error = abr::session_error_e;
  std::vector<abr::session_candidate_t> sessions {
    { "paired-a", "old-running", true, false, false },
    { "paired-a", "new-running", true, false, false },
    { "paired-a", "starting", false, true, false },
    { "paired-a", "control-only", true, false, true },
  };
  const auto selected = abr::select_session(sessions, "paired-a", "new-running");
  EXPECT_EQ(selected.error, error::none);
  EXPECT_EQ(selected.index, 1u);
  EXPECT_EQ(abr::select_session(sessions, "paired-a", "old-running").index, 0u);
  EXPECT_EQ(abr::select_session(sessions, "paired-a", "starting").error, error::stale_session);
  EXPECT_EQ(abr::select_session(sessions, "paired-a", "control-only").error, error::stale_session);
  EXPECT_EQ(abr::select_session(sessions, "paired-b", "new-running").error, error::stale_session);
}

TEST(AbrSessionIdentity, ProductionLifecycleProjectionExcludesNonVideoStates) {
  using lifecycle = stream::session::state_e;
  const auto stopped = abr::make_session_candidate("paired-a", "stopped", lifecycle::STOPPED, false);
  const auto stopping = abr::make_session_candidate("paired-a", "stopping", lifecycle::STOPPING, false);
  const auto starting = abr::make_session_candidate("paired-a", "starting", lifecycle::STARTING, false);
  const auto running = abr::make_session_candidate("paired-a", "running", lifecycle::RUNNING, false);
  const auto control = abr::make_session_candidate("paired-a", "control", lifecycle::RUNNING, true);
  EXPECT_FALSE(stopped.running);
  EXPECT_FALSE(stopped.starting);
  EXPECT_FALSE(stopping.running);
  EXPECT_FALSE(stopping.starting);
  EXPECT_FALSE(running.starting);
  EXPECT_TRUE(running.running);
  EXPECT_TRUE(starting.starting);
  EXPECT_FALSE(starting.running);
  EXPECT_TRUE(control.control_only);
  EXPECT_EQ(abr::select_session({ stopped, stopping, control }, "paired-a").error,
    abr::session_error_e::no_active_stream);
  EXPECT_EQ(abr::select_session({ starting }, "paired-a").error, abr::session_error_e::session_starting);
  EXPECT_EQ(abr::select_session({ running }, "paired-a").error, abr::session_error_e::none);
  for (const auto &candidate : { stopped, stopping, starting, control }) {
    EXPECT_EQ(abr::select_session({ candidate }, "paired-a", candidate.token).error,
      abr::session_error_e::stale_session);
  }
  EXPECT_EQ(abr::select_session({ running }, "paired-a", "running").error, abr::session_error_e::none);
}

TEST(AbrRejectionLogging, ClientChurnCannotEvictRateLimitStateOrFloodWarnings) {
  nvhttp::abr_api::rejection_log_limiter_t limiter;
  using clock = nvhttp::abr_api::rejection_log_limiter_t::clock_t;
  auto now = clock::time_point {} + std::chrono::seconds(1);
  std::uint64_t suppressed = 0;
  std::size_t logged = 0;
  for (std::size_t i = 0; i < 1000; ++i) {
    const auto uuid = "paired-client-" + std::to_string(i);
    if (limiter.should_log(uuid, "invalid_body", now, suppressed)) {
      ++logged;
    }
    now += std::chrono::milliseconds(50);
  }
  EXPECT_LE(logged, 11u);
  EXPECT_GT(suppressed, 0u);
}

TEST(AbrRejectionLogging, ReportsSuppressedCountOnce) {
  nvhttp::abr_api::rejection_log_limiter_t limiter;
  using clock = nvhttp::abr_api::rejection_log_limiter_t::clock_t;
  const auto now = clock::time_point {} + std::chrono::seconds(1);
  std::uint64_t suppressed = 0;
  EXPECT_TRUE(limiter.should_log("paired-a", "invalid_body", now, suppressed));
  EXPECT_EQ(suppressed, 0u);
  EXPECT_FALSE(limiter.should_log("paired-a", "invalid_body", now + std::chrono::seconds(1), suppressed));
  EXPECT_TRUE(limiter.should_log("paired-b", "stale_session", now + std::chrono::seconds(6), suppressed));
  EXPECT_EQ(suppressed, 1u);
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
         { { "enabled", true }, { "maxBitrate", 20000.0 } },
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
  EXPECT_TRUE(nvhttp::abr_api::error_contract(abr::session_error_e::ambiguous_session, true).retryable);
  EXPECT_FALSE(nvhttp::abr_api::error_contract(abr::session_error_e::ambiguous_session, false).retryable);
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

TEST(AbrAppliedLog, OneInfoLinePerActualTotalChange) {
  using sink_t = boost::log::sinks::synchronous_sink<boost::log::sinks::text_ostream_backend>;
  auto buffer = boost::make_shared<std::stringstream>();
  auto sink = boost::make_shared<sink_t>();
  sink->locked_backend()->add_stream(buffer);
  boost::log::core::get()->add_sink(sink);

  const std::string token = "0123456789abcdef-full-token";
  streaming::bitrate_budget_t budget(400000, 400000, 0, 0, 0, false);
  abr::session_request_t configure;
  configure.operation = abr::operation_e::configure;
  configure.config = { true, 120000, 400000, abr::mode_e::BALANCED };
  ASSERT_EQ(abr::apply_session_request(token, "app", budget, configure).error, abr::session_error_e::none);

  abr::session_request_t feedback;
  feedback.operation = abr::operation_e::feedback;
  feedback.token = token;
  feedback.feedback = { 6.0, 0, 0, 0, 400000 };
  const auto dropped = abr::apply_session_request(token, "app", budget, feedback);
  ASSERT_TRUE(dropped.bitrate_requested);
  const auto changed_to = dropped.requested_bitrate_kbps;
  ASSERT_LT(changed_to, 400000);
  abr::cleanup(token);

  boost::log::core::get()->remove_sink(sink);
  sink->flush();
  const auto text = buffer->str();
  const auto expected = "ABR applied: total 400000->" + std::to_string(changed_to) + " Kbps (" + dropped.action.reason + ") token_prefix=01234567";
  EXPECT_NE(text.find(expected), std::string::npos) << text;
  EXPECT_EQ(text.find("ABR applied:"), text.rfind("ABR applied:")) << text;
  EXPECT_EQ(text.find("0123456789abcdef"), std::string::npos) << text;
}

TEST(AbrRequestContract, LargestAcceptedIntegerCannotOverflowModePresets) {
  const auto request = nvhttp::abr_api::parse_request({ { "enabled", true }, { "maxBitrate", 0 } }, false);
  const auto effective = abr::enable("large-budget", request.config, std::numeric_limits<int>::max(), "app");
  EXPECT_EQ(effective.max_bitrate_kbps, 150000);
  EXPECT_GE(effective.min_bitrate_kbps, 0);
  EXPECT_LE(effective.min_bitrate_kbps, effective.max_bitrate_kbps);
  abr::cleanup("large-budget");
}
