#include "src/config.h"
#include "src/nvhttp/abr_api.h"
#include "src/nvhttp/abr_request.h"
#include "src/sync.h"
#include <future>
#include <gtest/gtest.h>
#include <mutex>

namespace {
  struct fake_session_t {
    abr::session_candidate_t candidate;
    std::string name = "asaf";
    std::string address = "::ffff:84.229.28.83";
    unsigned launch_id = 42;
    streaming::bitrate_budget_t budget { 20000, 100000, 0, 0, 0, false };
  };
  sync_util::sync_t<std::vector<fake_session_t *>> session_list;
  std::vector<std::unique_ptr<fake_session_t>> sessions;
  bool fail_operation = false;

  class AbrHttp: public testing::Test {
  protected:
    void
    SetUp() override {
      config::video.max_bitrate = 0;
      fail_operation = false;
      add("paired-a", "allocation-a");
    }
    void
    TearDown() override {
      clear();
    }
    void
    clear() {
      auto lock = session_list.lock();
      session_list->clear();
      for (const auto &session : sessions) {
        abr::cleanup(session->candidate.token);
      }
      sessions.clear();
    }
    fake_session_t &
    add(std::string uuid, std::string token) {
      auto session = std::make_unique<fake_session_t>();
      session->candidate = { std::move(uuid), std::move(token), true, false, false };
      auto lock = session_list.lock();
      session_list->push_back(session.get());
      sessions.push_back(std::move(session));
      return *sessions.back();
    }
    struct reply_t {
      int status;
      nlohmann::json body;
    };
    reply_t
    post(const std::string &body, bool feedback = false, std::string uuid = "paired-a",
      std::string address = "84.229.28.83", bool capability = false) {
      auto request = std::make_shared<web::Request>();
      request->method = capability ? "GET" : "POST";
      request->path = capability ? "/api/abr/capabilities" : feedback ? "/api/abr/feedback" :
                                                                        "/api/abr";
      request->client_uuid = std::move(uuid);
      request->remote = { boost::asio::ip::make_address(address), 1234 };
      request->query_string = "uniqueid=paired-b&clientname=asaf";
      request->header.emplace("X-Client-UUID", "paired-b");
      request->content.str(body);
      std::string output;
      web::detail::ResponseChannel channel;
      channel.send = [&](std::string bytes, auto completion, bool, bool) {
        output += bytes;
        if (completion) completion({});
      };
      channel.begin_stream = [] {};
      channel.closed = [] { return false; };
      channel.abort = [] {};
      auto response = std::make_shared<nvhttp::abr_api::resp_https_t::element_type>(*request, std::move(channel));
      if (capability) {
        nvhttp::abr_api::capabilities(response, request);
      }
      else if (feedback) {
        nvhttp::abr_api::feedback(response, request);
      }
      else {
        nvhttp::abr_api::configure(response, request);
      }
      response->send();
      return { std::stoi(output.substr(9, 3)), nlohmann::json::parse(output.substr(output.find("\r\n\r\n") + 4)) };
    }
    void
    expect_error(const reply_t &reply, int status, const char *code, bool retryable) {
      EXPECT_EQ(reply.status, status);
      EXPECT_EQ(reply.body.at("success"), false);
      EXPECT_EQ(reply.body.at("error_code"), code);
      EXPECT_TRUE(reply.body.at("error").is_string());
      EXPECT_EQ(reply.body.at("retryable"), retryable);
      EXPECT_EQ(reply.body.size(), 4u);
    }
  };
}  // namespace

// Believable stream shim: same selector, controller and real budget implementation
// as production, serialized ownership, but no media threads, TLS listener or device.
namespace stream::session {
  abr::session_result_t
  apply_abr_for_client(const std::string &uuid, const abr::session_request_t &request) {
    if (fail_operation) throw std::runtime_error("private internal detail");
    return abr::with_authenticated_session(session_list, uuid, request, [](const fake_session_t &session) { return session.candidate; }, [&](fake_session_t &session) { return abr::apply_session_request(session.candidate.token, "app", session.budget, request, config::video.max_bitrate); });
  }
}  // namespace stream::session

namespace nvhttp {
  std::string
  get_client_cert_uuid_from_request(req_https_t request) {
    return request->client_uuid;
  }
}  // namespace nvhttp

TEST_F(AbrHttp, ReportedIpv4MappedFixtureAndChangedHttpsRouteConfigureSuccessfully) {
  ASSERT_EQ(sessions[0]->address, "::ffff:84.229.28.83");
  auto reply = post(R"({"enabled":true,"minBitrate":0,"maxBitrate":80000,"mode":"balanced","uniqueid":"paired-b"})");
  ASSERT_EQ(reply.status, 200);
  EXPECT_EQ(reply.body.at("sessionToken"), "allocation-a");
  EXPECT_EQ(reply.body.at("minBitrate"), 6000);
  EXPECT_EQ(reply.body.at("maxBitrate"), 80000);
  EXPECT_EQ(reply.body.at("initialBitrate"), 20000);
  EXPECT_EQ(reply.body.at("bitrateRequested"), false);
  EXPECT_EQ(post(R"({"enabled":true})", false, "paired-a", "192.168.1.200").status, 200);
}

TEST_F(AbrHttp, SharedIpAndDisplayNameCannotCrossTargetBudgetOrDisable) {
  add("paired-b", "allocation-b");
  ASSERT_EQ(sessions[0]->name, sessions[1]->name);
  ASSERT_EQ(sessions[0]->address, sessions[1]->address);
  ASSERT_EQ(post(R"({"enabled":true,"maxBitrate":10000})").status, 200);
  EXPECT_EQ(sessions[0]->budget.total(), 10000);
  EXPECT_EQ(sessions[1]->budget.total(), 20000);
  ASSERT_EQ(post(R"({"enabled":true})", false, "paired-b").status, 200);
  const auto feedback = post(R"({"sessionToken":"allocation-a","packetLoss":6,"currentBitrate":10000})", true);
  ASSERT_EQ(feedback.status, 200);
  EXPECT_EQ(feedback.body.at("newBitrate"), 7000);
  EXPECT_EQ(feedback.body.at("bitrateRequested"), true);
  EXPECT_EQ(sessions[0]->budget.total(), 7000);
  EXPECT_EQ(sessions[1]->budget.total(), 20000);
  expect_error(post(R"({"enabled":false,"sessionToken":"allocation-b"})"), 409, "stale_session", false);
  EXPECT_TRUE(abr::is_enabled("allocation-b"));
  const auto disabled = post(R"({"enabled":false,"sessionToken":"allocation-a"})");
  ASSERT_EQ(disabled.status, 200);
  EXPECT_EQ(disabled.body, (nlohmann::json { { "success", true }, { "enabled", false }, { "sessionToken", "allocation-a" } }));
  EXPECT_FALSE(abr::is_enabled("allocation-a"));
  EXPECT_TRUE(abr::is_enabled("allocation-b"));
}

TEST_F(AbrHttp, ReusedLaunchIdAndNameRejectOldFeedbackDisableAndConfigureTokens) {
  ASSERT_EQ(post(R"({"enabled":true})").status, 200);
  abr::cleanup("allocation-a");
  clear();
  add("paired-a", "replacement");
  ASSERT_EQ(post(R"({"enabled":true})").status, 200);
  expect_error(post(R"({"sessionToken":"allocation-a","packetLoss":6})", true), 409, "stale_session", false);
  expect_error(post(R"({"enabled":false,"sessionToken":"allocation-a"})"), 409, "stale_session", false);
  expect_error(post(R"({"enabled":true,"sessionToken":"allocation-a"})"), 409, "stale_session", false);
  abr::cleanup("allocation-a");
  EXPECT_TRUE(abr::is_enabled("replacement"));
  EXPECT_EQ(sessions[0]->budget.total(), 20000);
}

TEST_F(AbrHttp, MissingStartingAmbiguousAndDisabledHaveExactErrorContract) {
  expect_error(post(R"({"enabled":true})", false, ""), 403, "missing_client_identity", false);
  expect_error(post(R"({"enabled":true})", false, "unpaired"), 409, "no_active_stream", true);
  sessions[0]->candidate.running = false;
  sessions[0]->candidate.starting = true;
  expect_error(post(R"({"enabled":true})"), 409, "session_starting", true);
  expect_error(post(R"({"enabled":true,"sessionToken":"allocation-a"})"), 409, "session_starting", false);
  add("paired-a", "second");
  expect_error(post(R"({"enabled":true})"), 409, "ambiguous_session", false);
  sessions[0]->candidate.starting = false;
  sessions[0]->candidate.control_only = true;
  expect_error(post(R"({"sessionToken":"second"})", true), 409, "abr_not_enabled", false);
  clear();
  expect_error(post(R"({"sessionToken":"second"})", true), 409, "stale_session", false);
}

TEST_F(AbrHttp, MalformedBodiesCannotDisableAndInternalErrorsAreNotLeaked) {
  ASSERT_EQ(post(R"({"enabled":true})").status, 200);
  for (const std::string body : { "{", "[]", "{}", R"({"enabled":false})",
         R"({"enabled":true,"maxBitrate":1.5})", R"({"enabled":true,"maxBitrate":2147483648})" }) {
    expect_error(post(body), 400, "invalid_body", false);
  }
  expect_error(post(R"({"packetLoss":6})", true), 400, "invalid_body", false);
  EXPECT_TRUE(abr::is_enabled("allocation-a"));
  fail_operation = true;
  const auto reply = post(R"({"enabled":true})");
  expect_error(reply, 500, "internal_error", false);
  EXPECT_EQ(reply.body.dump().find("private internal detail"), std::string::npos);
}

TEST_F(AbrHttp, HostCapAndEffectiveBoundsPreserveBudgetRequestMeaning) {
  config::video.max_bitrate = 15000;
  const auto reply = post(R"({"enabled":true,"maxBitrate":80000})");
  ASSERT_EQ(reply.status, 200);
  EXPECT_EQ(reply.body.at("maxBitrate"), 15000);
  EXPECT_EQ(reply.body.at("initialBitrate"), 15000);
  EXPECT_EQ(reply.body.at("maxBitrateCapped"), true);
  EXPECT_EQ(reply.body.at("requestedMaxBitrate"), 80000);
  EXPECT_EQ(reply.body.at("bitrateRequested"), true);
  EXPECT_EQ(reply.body.at("bitrateApplied"), true);
  EXPECT_EQ(sessions[0]->budget.total(), 15000);
  EXPECT_EQ(sessions[0]->budget.applied().total_kbps, 20000);
}

TEST_F(AbrHttp, CapabilitiesAdvertiseVersionTwoAndSessionBinding) {
  const auto reply = post("", false, "paired-a", "84.229.28.83", true);
  ASSERT_EQ(reply.status, 200);
  EXPECT_EQ(reply.body.at("supported"), true);
  EXPECT_EQ(reply.body.at("version"), 2);
  const auto features = reply.body.at("features").get<std::vector<std::string>>();
  EXPECT_NE(std::find(features.begin(), features.end(), "session_binding"), features.end());
}

TEST_F(AbrHttp, FeedbackPreservesAnUpdatedHostCapAndReportsActualQueuedBudget) {
  ASSERT_EQ(post(R"({"enabled":true,"maxBitrate":80000})").status, 200);
  config::video.max_bitrate = 5000;
  const auto reply = post(R"({"sessionToken":"allocation-a","packetLoss":6,"currentBitrate":20000})", true);
  ASSERT_EQ(reply.status, 200);
  EXPECT_EQ(reply.body.at("newBitrate"), 5000);
  EXPECT_EQ(sessions[0]->budget.total(), 5000);
  EXPECT_EQ(sessions[0]->budget.applied().total_kbps, 20000);
}

TEST_F(AbrHttp, RemovalAndReplacementCannotInterleaveSelectionAndBudgetMutation) {
  std::promise<void> selected, release, removing;
  auto release_future = release.get_future();
  abr::session_request_t request;
  request.config = { true, 2000, 10000, abr::mode_e::BALANCED };
  auto operation = std::async(std::launch::async, [&] {
    return abr::with_authenticated_session(session_list, "paired-a", request, [](const fake_session_t &session) { return session.candidate; }, [&](fake_session_t &session) {
        selected.set_value();
        release_future.wait();
        return abr::apply_session_request(session.candidate.token, "app", session.budget, request); });
  });
  selected.get_future().wait();
  auto replacement = std::async(std::launch::async, [&] {
    removing.set_value();
    clear();
    add("paired-a", "replacement");
  });
  removing.get_future().wait();
  EXPECT_EQ(replacement.wait_for(std::chrono::milliseconds(50)), std::future_status::timeout);
  release.set_value();
  const auto result = operation.get();
  replacement.get();
  EXPECT_EQ(result.token, "allocation-a");
  EXPECT_EQ(result.requested_bitrate_kbps, 10000);
  EXPECT_EQ(sessions[0]->budget.total(), 20000);
  expect_error(post(R"({"sessionToken":"allocation-a","packetLoss":6})", true), 409, "stale_session", false);
}
