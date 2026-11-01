/**
 * Exercise the real startup/fallback policy with link-time platform fakes.
 * This target must not link display, encoder, registry or driver implementations.
 */
#include <algorithm>
#include <deque>
#include <gtest/gtest.h>
#include <stdexcept>
#include <vector>

#include "src/config.h"
#include "src/display_device/session.h"
#include "src/display_device/vdd_capability.h"
#include "src/hdr/session_target.h"
#include "src/logging_severity.h"
#include "src/nvhttp_stream_start.h"
#include "src/touch_keyboard_session.h"
#include "src/video.h"

namespace {
  using configure_result_t = display_device::session_t::configure_result_t;
  using result_e = configure_result_t::result_e;
  using intent_t = display_device::display_intent_t;
  using prep_e = display_device::parsed_config_t::device_prep_e;
  using vdd_state_e = display_device::vdd_capability::state_e;
  using probe_error_e = video::probe_error_e;
  using probe_policy_e = video::probe_target_policy_e;

  struct configure_call_t {
    bool vdd, reconfigure;
    std::uint32_t id;
    int width, height, fps;
    bool hdr, video, audio, control;
    float reported_nits;
  };

  struct probe_call_t {
    video::probe_target_t target;
    config::video_t config;
  };

  struct fake_t {
    intent_t intent { intent_t::target_e::physical, "physical", false, prep_e::ensure_active };
    vdd_state_e vdd_state { vdd_state_e::ready };
    std::deque<configure_result_t> configurations;
    std::deque<video::probe_result_t> probes;
    std::vector<configure_call_t> configured;
    std::vector<probe_call_t> probed;
    std::vector<std::string> events;
    std::vector<bool> touch;
    int queries {};
    int commits {};
  } fake;

  template <class T>
  T
  consume(std::deque<T> &script, const char *operation) {
    EXPECT_FALSE(script.empty()) << "unexpected " << operation;
    if (script.empty()) throw std::logic_error { operation };
    auto value = std::move(script.front());
    script.pop_front();
    return value;
  }

  configure_result_t
  configured(result_e result, bool cleanup = false) {
    return { result, "specific display failure", "specific display hint", cleanup };
  }

  video::probe_result_t
  probed(probe_error_e error) {
    return { error, "original probe failure", "original probe hint" };
  }
}  // namespace

boost::log::sources::severity_logger<int> debug, info, warning;

namespace config {
  video_t video {};

  bool
  get_client_touch_keyboard_enabled(const std::string &) {
    return false;
  }
}  // namespace config

namespace display_device {
  struct settings_t::persistent_data_t {};
  struct settings_t::audio_data_t {};
  settings_t::settings_t() = default;
  settings_t::~settings_t() = default;
  class session_t::StateRetryTimer {};
  session_t::session_t() = default;

  session_t &
  session_t::get() {
    static session_t instance;
    return instance;
  }

  session_t::configure_result_t
  session_t::configure_display(const config::video_t &, const rtsp_stream::launch_session_t &session, bool reconfigure) {
    fake.events.push_back(session.use_vdd ? "configure.vdd" : "configure.physical");
    fake.configured.push_back({ session.use_vdd, reconfigure, session.id,
      session.width, session.height, session.fps, session.enable_hdr,
      session.setup_video, session.setup_audio, session.setup_control,
      session.reported_hdr_capabilities.max_nits });
    if (session.use_vdd) config::video.output_name = "vdd-" + std::to_string(fake.configured.size());
    return consume(fake.configurations, "display configuration");
  }

  void
  session_t::restore_state() {
    fake.events.push_back("restore");
    config::video.output_name = "physical";
  }

  display_intent_t
  resolve_display_intent(const config::video_t &, const rtsp_stream::launch_session_t &) {
    return fake.intent;
  }

  namespace vdd_capability {
    state_e
    query_state() {
      ++fake.queries;
      return fake.vdd_state;
    }

    std::string_view
    to_string(state_e) {
      return "fake VDD capability";
    }
  }  // namespace vdd_capability
}  // namespace display_device

namespace hdr {
  void
  adopt_vdd_calibration_if_needed(rtsp_stream::launch_session_t &session) {
    fake.events.push_back(session.use_vdd ? "hdr.vdd" : "hdr.physical");
    if (session.use_vdd) {
      session.hdr_capabilities.max_nits = 600;
      session.hdr_target_source = target_source_e::windows_hdr_calibration;
    }
  }
}  // namespace hdr

namespace touch_kb {
  bool
  start_session(bool effective) {
    fake.events.push_back("touch");
    fake.touch.push_back(effective);
    return true;
  }
}  // namespace touch_kb

namespace rtsp_stream {
  void
  launch_session_t::sync_hdr_environment() {
    ++fake.commits;
    fake.events.push_back("commit");
  }
}  // namespace rtsp_stream

namespace video {
  probe_result_t last_encoder_probe_result {};

  int
  probe_encoders(std::optional<probe_target_t> target) {
    EXPECT_TRUE(target);
    if (!target) throw std::logic_error { "startup probe omitted its target" };
    fake.events.push_back("probe");
    fake.probed.push_back({ *target, config::video });
    last_encoder_probe_result = consume(fake.probes, "encoder probe");
    return last_encoder_probe_result ? 0 : -1;
  }
}  // namespace video

namespace {
  class StreamStartPolicy: public ::testing::Test {
  protected:
    config::video_t saved_config { config::video };
    video::probe_result_t saved_probe { video::last_encoder_probe_result };
    std::unique_ptr<rtsp_stream::launch_session_t> launch;
    boost::property_tree::ptree tree;

    void
    SetUp() override {
      reset();
    }

    void
    TearDown() override {
      config::video = std::move(saved_config);
      video::last_encoder_probe_result = std::move(saved_probe);
    }

    void
    reset() {
      fake = {};
      tree.clear();
      config::video = {};
      config::video.output_name = "physical";
      config::video.encoder = "configured-encoder";
      config::video.hevc_mode = 3;
      config::video.av1_mode = 3;
      video::last_encoder_probe_result = {};
      launch = std::make_unique<rtsp_stream::launch_session_t>();
      launch->id = 42;
      launch->width = 1440;
      launch->height = 900;
      launch->fps = 75;
      launch->enable_hdr = true;
      launch->setup_video = launch->setup_audio = launch->setup_control = true;
      launch->reported_hdr_capabilities.max_nits = 700;
      launch->hdr_capabilities.max_nits = 700;
      launch->client_cert_uuid = "paired-client";
      launch->touch_keyboard = 1;
      launch->env["SUNSHINE_CLIENT_USE_VDD"] = "false";
    }

    bool
    prepare(bool reconfigure = true) {
      return nvhttp::stream_start::prepare_display_and_probe_encoders(tree, *launch, reconfigure);
    }

    void
    events(std::vector<std::string> expected) {
#ifndef _WIN32
      std::erase(expected, "touch");
#endif
      EXPECT_EQ(fake.events, expected);
      EXPECT_TRUE(fake.configurations.empty());
      EXPECT_TRUE(fake.probes.empty());
    }
  };

  TEST_F(StreamStartPolicy, PreflightRefusesUnavailableTargetsWithoutSideEffects) {
    struct case_t {
      intent_t::target_e target;
      vdd_state_e state;
      int status;
      const char *code;
    };
    for (const auto &test : {
           case_t { intent_t::target_e::unavailable, vdd_state_e::ready, 503, "DISPLAY_NOT_CONNECTED" },
           case_t { intent_t::target_e::vdd, vdd_state_e::driver_missing, 503, "VDD_DRIVER_MISSING" },
           case_t { intent_t::target_e::vdd, vdd_state_e::driver_unreachable, 503, "VDD_DRIVER_UNREACHABLE" },
           case_t { intent_t::target_e::vdd, vdd_state_e::unsupported_platform, 501, "VDD_NOT_SUPPORTED" } }) {
      SCOPED_TRACE(test.code);
      reset();
      fake.intent.target = test.target;
      fake.vdd_state = test.state;
      EXPECT_FALSE(prepare());
      EXPECT_EQ(tree.get<int>("root.<xmlattr>.status_code"), test.status);
      EXPECT_EQ(tree.get<std::string>("root.sunshine_error_code"), test.code);
      EXPECT_EQ(tree.get<int>("root.sunshine_recoverable"), test.status == 501 ? 0 : 1);
      EXPECT_EQ(fake.queries, test.target == intent_t::target_e::vdd ? 1 : 0);
      events({});
    }
  }

  TEST_F(StreamStartPolicy, SuccessfulPreparationUsesResolvedTargetAndOrdering) {
    for (const int target : { 0, 1, 2 }) {
      for (const bool reconfigure : { false, true }) {
        SCOPED_TRACE(::testing::Message() << "target=" << target << " reconfigure=" << reconfigure);
        reset();
        launch->use_vdd = target == 2;
        if (target == 1) fake.intent.device_id.clear();
        if (target == 2) fake.intent.target = intent_t::target_e::vdd;
        fake.configurations = { configured(result_e::success) };
        fake.probes = { probed(probe_error_e::none) };
        ASSERT_TRUE(prepare(reconfigure));
        ASSERT_EQ(fake.probed.size(), 1);
        EXPECT_EQ(fake.probed[0].target.output_name, target == 2 ? "vdd-1" : fake.intent.device_id);
        EXPECT_EQ(fake.probed[0].target.policy, target == 2 ? probe_policy_e::vdd_compatible :
                                                target == 1 ? probe_policy_e::backend_autoselect :
                                                              probe_policy_e::exact);
        ASSERT_EQ(fake.configured.size(), 1);
        EXPECT_EQ(fake.configured[0].reconfigure, reconfigure);
        events({ target == 2 ? "configure.vdd" : "configure.physical",
          target == 2 ? "hdr.vdd" : "hdr.physical", "touch", "probe" });
      }
    }
  }

  TEST_F(StreamStartPolicy, FatalConfigurationPreservesRichErrorsWithoutProbing) {
    fake.configurations = { configured(result_e::parse_fail) };
    EXPECT_FALSE(prepare());
    EXPECT_EQ(tree.get<std::string>("root.sunshine_error_code"), "DISPLAY_CONFIG_PARSE_FAILED");
    EXPECT_EQ(tree.get<std::string>("root.<xmlattr>.status_message"), "specific display failure specific display hint");
    EXPECT_EQ(tree.get<std::string>("root.sunshine_error_hint"), "specific display hint");
    EXPECT_EQ(tree.get<std::string>("root.sunshine_error_stage"), "display_configuration");
    events({ "configure.physical", "touch" });
  }

  TEST_F(StreamStartPolicy, DeferredRetriesReclassifyTheirLastResultAndStopAtTheBound) {
    for (const auto last : { result_e::success, result_e::parse_fail, result_e::deferred_retry, result_e::modes_fail }) {
      SCOPED_TRACE(static_cast<int>(last));
      reset();
      fake.intent.user_named_display = true;
      fake.configurations = { configured(result_e::deferred_retry), configured(last) };
      fake.probes = { probed(probe_error_e::no_working_encoder) };
      const bool recovered = last == result_e::success || last == result_e::modes_fail;
      if (recovered) fake.probes.push_back(probed(probe_error_e::none));
      if (last == result_e::deferred_retry) {
        fake.configurations.push_back(configured(last));
        fake.configurations.push_back(configured(last));
      }
      EXPECT_EQ(prepare(), recovered);
      EXPECT_EQ(tree.get<int>("root.sunshine_auto_recovery_attempted"), 1);
      EXPECT_EQ(tree.get<std::string>("root.sunshine_auto_recovery_action"),
        last == result_e::modes_fail ? "current_display_settings_fallback" : "deferred_display_retry");
      if (last == result_e::success) {
        EXPECT_EQ(tree.get<std::string>("root.sunshine_auto_recovery_result"), "succeeded");
        events({ "configure.physical", "hdr.physical", "touch", "probe", "configure.physical", "hdr.physical", "probe" });
      }
      else if (last == result_e::modes_fail) {
        events({ "configure.physical", "hdr.physical", "touch", "probe", "configure.physical", "probe" });
      }
      else {
        EXPECT_EQ(tree.get<std::string>("root.sunshine_error_code"),
          last == result_e::parse_fail ? "DISPLAY_CONFIG_PARSE_FAILED" : "DISPLAY_CONFIG_DEFERRED");
        EXPECT_EQ(fake.configured.size(), last == result_e::parse_fail ? 2 : 4);
        EXPECT_EQ(fake.probed.size(), 1);
        EXPECT_EQ(std::count(fake.events.begin(), fake.events.end(), "restore"), 0);
        EXPECT_TRUE(fake.configurations.empty());
        EXPECT_TRUE(fake.probes.empty());
      }
    }
  }

  TEST_F(StreamStartPolicy, CurrentDisplayFallbackHonorsExplicitIntentAndCleanupOwnership) {
    for (const int policy : { 0, 1, 2, 3 }) {
      for (const bool cleanup : { false, true }) {
        SCOPED_TRACE(::testing::Message() << "policy=" << policy << " cleanup=" << cleanup);
        reset();
        fake.intent.user_named_display = policy == 0;
        if (policy == 1) fake.intent.device_prep = prep_e::no_operation;
        if (policy == 2) fake.vdd_state = vdd_state_e::driver_missing;
        const auto result = policy == 3 ? result_e::file_save_fail : result_e::modes_fail;
        fake.configurations = { configured(result, cleanup) };
        fake.probes = { probed(probe_error_e::no_working_encoder) };
        EXPECT_FALSE(prepare());
        EXPECT_EQ(fake.queries, policy == 2 ? 1 : 0);
        EXPECT_FALSE(launch->use_vdd);
        EXPECT_EQ(fake.commits, 0);
        EXPECT_EQ(tree.get<std::string>("root.sunshine_error_code"),
          policy == 3 ? "DISPLAY_PERSISTENCE_SAVE_FAILED" : "DISPLAY_MODE_FAILED");
        auto expected = std::vector<std::string> { "configure.physical", "touch", "probe" };
        if (cleanup) expected.push_back("restore");
        events(std::move(expected));
      }
    }
  }

  TEST_F(StreamStartPolicy, TopologyFallbackTriesCurrentBeforeVddAndCopiesLaunchFields) {
    fake.configurations = { configured(result_e::topology_fail, true), configured(result_e::success) };
    fake.probes = { probed(probe_error_e::no_working_encoder), probed(probe_error_e::none) };
    ASSERT_TRUE(prepare(false));
    events({ "configure.physical", "touch", "probe", "restore", "configure.vdd", "hdr.vdd", "probe", "commit" });
    ASSERT_EQ(fake.configured.size(), 2);
    const auto &recovery = fake.configured[1];
    EXPECT_TRUE(recovery.vdd);
    EXPECT_FALSE(recovery.reconfigure);
    EXPECT_EQ(recovery.id, 42);
    EXPECT_EQ(recovery.width, 1440);
    EXPECT_EQ(recovery.height, 900);
    EXPECT_EQ(recovery.fps, 75);
    EXPECT_TRUE(recovery.hdr && recovery.video && recovery.audio && recovery.control);
    EXPECT_EQ(recovery.reported_nits, 700);
    EXPECT_TRUE(launch->use_vdd);
    EXPECT_EQ(launch->hdr_capabilities.max_nits, 600);
    EXPECT_EQ(launch->hdr_target_source, hdr::target_source_e::windows_hdr_calibration);
    EXPECT_EQ(launch->client_cert_uuid, "paired-client");
    EXPECT_EQ(launch->env["SUNSHINE_CLIENT_USE_VDD"].to_string(), "true");
    EXPECT_EQ(launch->env["SUNSHINE_CLIENT_DISPLAY_NAME"].to_string(), "vdd-2");
    EXPECT_EQ(fake.probed[1].target.output_name, "vdd-2");
    EXPECT_EQ(fake.probed[1].target.policy, probe_policy_e::vdd_compatible);
    EXPECT_EQ(fake.commits, 1);
  }

  TEST_F(StreamStartPolicy, FailedVddRecoveryRollsBackOnlyOwnedWorkAndPreservesOriginalError) {
    for (const int failure : { 0, 1, 2, 3 }) {
      SCOPED_TRACE(failure);
      reset();
      const auto vdd_result = failure == 0 ? result_e::success :
                              failure == 1 ? result_e::modes_fail :
                              failure == 2 ? result_e::parse_fail :
                                             result_e::deferred_retry;
      fake.configurations = { configured(result_e::success), configured(vdd_result, failure == 1) };
      fake.probes = { probed(probe_error_e::no_active_display) };
      if (failure == 0) fake.probes.push_back({ probe_error_e::no_working_encoder, "VDD failure", "VDD hint" });
      if (failure == 3) fake.configurations.push_back(configured(result_e::parse_fail));
      EXPECT_FALSE(prepare());
      EXPECT_EQ(tree.get<std::string>("root.sunshine_error_code"), "NO_ACTIVE_DISPLAY");
      EXPECT_EQ(tree.get<std::string>("root.sunshine_error_hint"), "original probe hint");
      EXPECT_EQ(video::last_encoder_probe_result.error, probe_error_e::no_active_display);
      EXPECT_EQ(std::count(fake.events.begin(), fake.events.end(), "restore"), failure == 2 ? 0 : 1);
      EXPECT_EQ(fake.commits, 0);
      EXPECT_FALSE(launch->use_vdd);
      EXPECT_EQ(launch->env["SUNSHINE_CLIENT_USE_VDD"].to_string(), "false");
      EXPECT_EQ(launch->hdr_capabilities.max_nits, 700);
      EXPECT_TRUE(fake.configurations.empty());
      EXPECT_TRUE(fake.probes.empty());
    }
  }

  TEST_F(StreamStartPolicy, DeferredVddRecoveryProbesFreshTargetBeforeCommitting) {
    fake.configurations = { configured(result_e::success), configured(result_e::deferred_retry), configured(result_e::success) };
    fake.probes = { probed(probe_error_e::no_active_display), probed(probe_error_e::none) };
    ASSERT_TRUE(prepare());
    events({ "configure.physical", "hdr.physical", "touch", "probe", "configure.vdd", "configure.vdd", "hdr.vdd", "probe", "commit" });
    ASSERT_EQ(fake.probed.size(), 2);
    EXPECT_EQ(fake.probed[1].target.output_name, "vdd-3");
    EXPECT_EQ(fake.probed[1].target.policy, probe_policy_e::vdd_compatible);
    EXPECT_TRUE(launch->use_vdd);
    EXPECT_EQ(launch->env["SUNSHINE_CLIENT_DISPLAY_NAME"].to_string(), "vdd-3");
    EXPECT_EQ(tree.get<std::string>("root.sunshine_auto_recovery_action"), "vdd_display_recovery");
    EXPECT_EQ(tree.get<std::string>("root.sunshine_auto_recovery_result"), "succeeded");
  }

  TEST_F(StreamStartPolicy, TemporaryEncoderFallbackRestoresConfigurationOnSuccessAndFailure) {
    for (const auto error : { probe_error_e::configured_encoder_unavailable, probe_error_e::codec_requirements_unmet }) {
      for (const bool succeeds : { false, true }) {
        SCOPED_TRACE(::testing::Message() << "error=" << static_cast<int>(error) << " success=" << succeeds);
        reset();
        fake.configurations = { configured(result_e::success) };
        fake.probes = { probed(error), probed(succeeds ? probe_error_e::none : probe_error_e::no_working_encoder) };
        EXPECT_EQ(prepare(), succeeds);
        events({ "configure.physical", "hdr.physical", "touch", "probe", "probe" });
        ASSERT_EQ(fake.probed.size(), 2);
        const auto &temporary = fake.probed[1].config;
        const bool encoder = error == probe_error_e::configured_encoder_unavailable;
        EXPECT_EQ(temporary.encoder, encoder ? "" : "configured-encoder");
        EXPECT_EQ(temporary.hevc_mode, encoder ? 3 : 1);
        EXPECT_EQ(temporary.av1_mode, encoder ? 3 : 1);
        EXPECT_EQ(config::video.encoder, "configured-encoder");
        EXPECT_EQ(config::video.hevc_mode, 3);
        EXPECT_EQ(config::video.av1_mode, 3);
        EXPECT_EQ(config::video.output_name, "physical");
        EXPECT_EQ(fake.queries, 0);
        EXPECT_EQ(tree.get<std::string>("root.sunshine_auto_recovery_action"),
          encoder ? "temporary_auto_encoder_fallback" : "temporary_h264_fallback");
        if (!succeeds) {
          EXPECT_EQ(video::last_encoder_probe_result.error, error);
          EXPECT_EQ(tree.get<std::string>("root.sunshine_error_hint"), "original probe hint");
        }
      }
    }
  }
}  // namespace
