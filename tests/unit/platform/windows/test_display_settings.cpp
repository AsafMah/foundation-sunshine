/**
 * Actual Windows settings and topology policy with fake OS, audio and VDD edges.
 * No native display, driver, color-profile or audio implementation is linked.
 */
#include "src/audio.h"
#include "src/config.h"
#include "src/display_device/color_profile.h"
#include "src/display_device/session.h"
#include "src/globals.h"
#include "src/logging_severity.h"
#include "src/platform/windows/display_device/color_profile.h"
#include "src/platform/windows/display_device/settings_topology.h"
#include "src/platform/windows/display_device/windows_utils.h"
#include "src/rtsp.h"

#include <algorithm>
#include <gtest/gtest.h>
#include <stdexcept>

namespace dd = display_device;
using vdd_policy = dd::automatic_vdd_policy_e;

namespace {
  struct host_t {
    dd::active_topology_t topology { { "physical" }, { "vdd" } };
    bool vdd_present { true };
    bool fail_topology {};
    dd::display_mode_t physical_mode { { 1920, 1080 }, { 60, 1 } };
    std::vector<std::string> writes;
    std::vector<std::pair<dd::active_topology_t, bool>> topology_writes;
  } host;

  dd::parsed_config_t
  configuration(bool vdd) {
    dd::parsed_config_t config {};
    config.device_id = vdd ? "vdd" : "physical";
    config.device_prep = dd::parsed_config_t::device_prep_e::no_operation;
    config.vdd_prep = dd::parsed_config_t::vdd_prep_e::no_operation;
    config.use_vdd = vdd;
    return config;
  }

  class DisplaySettingsPolicy: public ::testing::Test {
  protected:
    void
    SetUp() override {
      host = {};
      config::video = {};
    }

    void
    prepare_virtual() {
      const auto result = settings.apply_config(configuration(true), launch, vdd_policy::allow, dd::active_topology_t { { "physical" } });
      ASSERT_TRUE(result) << result.get_error_message();
      ASSERT_TRUE(settings.has_persistent_data());
      host.writes.clear();
      host.topology_writes.clear();
    }

    dd::settings_t settings;
    rtsp_stream::launch_session_t launch {};
  };
}  // namespace

boost::log::sources::severity_logger<int> verbose, debug, info, warning, error, fatal;
const std::string ZAKO_NAME { "test-vdd" };

namespace config {
  video_t video {};
  std::string
  get_clients_config() { return "[]"; }
}  // namespace config

namespace audio {
  audio_ctx_ref_t
  get_audio_ctx_ref() { return {}; }
}  // namespace audio

namespace display_device::color_profile {
  resolve_result_t
  resolve_client_hdr_profile(std::string_view, std::string_view, std::string_view) {
    return {};
  }
}  // namespace display_device::color_profile

namespace display_device::win_color_profile {
  void
  to_json(nlohmann::json &, const state_t &) { throw std::logic_error { "unexpected color profile serialization" }; }
  void
  from_json(const nlohmann::json &, state_t &) { throw std::logic_error { "unexpected color profile deserialization" }; }
  std::optional<state_t>
  snapshot(const std::string &, const std::string &) { throw std::logic_error { "unexpected color profile snapshot" }; }
  bool
  apply(const state_t &) { throw std::logic_error { "unexpected color profile apply" }; }
  bool
  restore(const state_t &) { throw std::logic_error { "unexpected color profile restore" }; }
}  // namespace display_device::win_color_profile

namespace display_device::w_utils {
  bool
  is_user_session_locked() { return false; }
  bool
  test_no_access_to_ccd_api() { return false; }
}  // namespace display_device::w_utils

namespace display_device {
  device_info_map_t
  enum_available_devices() {
    device_info_map_t devices {
      { "physical", { "physical", "Physical", device_state_e::primary, hdr_state_e::disabled } }
    };
    if (host.vdd_present) devices.emplace("vdd", device_info_t { "vdd", ZAKO_NAME, device_state_e::active, hdr_state_e::disabled });
    const auto active = get_device_ids_from_topology(host.topology);
    for (auto &[id, device] : devices) {
      if (!active.contains(id)) device.device_state = device_state_e::inactive;
    }
    return devices;
  }

  std::string
  get_display_friendly_name(const std::string &device_id) {
    const auto devices = enum_available_devices();
    const auto found = devices.find(device_id);
    return found == devices.end() ? std::string {} : found->second.friendly_name;
  }

  active_topology_t
  get_current_topology() { return host.topology; }

  bool
  is_topology_valid(const active_topology_t &topology) {
    const auto devices = enum_available_devices();
    const auto ids = get_device_ids_from_topology(topology);
    return !topology.empty() && std::all_of(ids.begin(), ids.end(), [&](const auto &id) { return devices.contains(id); });
  }

  bool
  is_topology_the_same(const active_topology_t &a, const active_topology_t &b) { return a == b; }

  bool
  set_topology(const active_topology_t &topology) {
    host.writes.push_back("topology");
    host.topology_writes.emplace_back(topology, !host.fail_topology);
    if (host.fail_topology) return false;
    host.topology = topology;
    return true;
  }

  bool
  is_primary_device(const std::string &device_id) { return device_id == "physical"; }

  bool
  set_as_primary_device(const std::string &) {
    host.writes.push_back("primary");
    return true;
  }

  device_display_mode_map_t
  get_current_display_modes(const std::unordered_set<std::string> &ids) {
    device_display_mode_map_t modes;
    for (const auto &id : ids) modes.emplace(id, host.physical_mode);
    return modes;
  }

  bool
  set_display_modes(const device_display_mode_map_t &modes) {
    host.writes.push_back("modes");
    if (const auto found = modes.find("physical"); found != modes.end()) host.physical_mode = found->second;
    return true;
  }

  hdr_state_map_t
  get_current_hdr_states(const std::unordered_set<std::string> &ids) {
    hdr_state_map_t states;
    for (const auto &id : ids) states.emplace(id, hdr_state_e::disabled);
    return states;
  }

  bool
  set_hdr_states(const hdr_state_map_t &) {
    host.writes.push_back("hdr");
    return true;
  }

  class session_t::StateRetryTimer {};
  session_t::session_t() = default;
  session_t &
  session_t::get() {
    static session_t session;
    return session;
  }

  bool
  session_t::destroy_vdd_monitor() {
    host.writes.push_back("destroy");
    host.vdd_present = false;
    for (auto &group : host.topology) std::erase(group, "vdd");
    std::erase_if(host.topology, [](const auto &group) { return group.empty(); });
    return true;
  }
}  // namespace display_device

namespace {
  template <class Settings>
  void
  expect_explicit_policy() {
    constexpr bool implicit_apply = requires(Settings &settings, const dd::parsed_config_t &config, const rtsp_stream::launch_session_t &launch) {
      settings.apply_config(config, launch);
    };
    constexpr bool implicit_revert = requires(Settings &settings) { settings.revert_settings(); };
    constexpr bool implicit_reset = requires(Settings &settings) { settings.reset_persistence(); };
    constexpr bool boolean_revert = requires(Settings &settings) { settings.revert_settings(dd::revert_reason_e::stream_ended, true); };
    EXPECT_FALSE(implicit_apply);
    EXPECT_FALSE(implicit_revert);
    EXPECT_FALSE(implicit_reset);
    EXPECT_FALSE(boolean_revert);
  }

  TEST_F(DisplaySettingsPolicy, EveryMutationRequiresAnExplicitTypedVddPolicy) {
    expect_explicit_policy<dd::settings_t>();
  }

  TEST_F(DisplaySettingsPolicy, NestedTopologyRollbackRunsLegacyVddCleanup) {
    ASSERT_NO_FATAL_FAILURE(prepare_virtual());
    const auto result = settings.apply_config(configuration(false), launch, vdd_policy::allow);
    ASSERT_TRUE(result) << result.get_error_message();
    EXPECT_EQ(std::count(host.writes.begin(), host.writes.end(), "destroy"), 1);
    EXPECT_FALSE(host.vdd_present);
    EXPECT_EQ(host.topology, (dd::active_topology_t { { "physical" } }));
    EXPECT_FALSE(settings.has_persistent_data());
    ASSERT_FALSE(host.writes.empty());
    EXPECT_EQ(host.writes.front(), "destroy");
  }

  TEST_F(DisplaySettingsPolicy, CallerHandledVddRestoreNeverDestroysTheHeadAgain) {
    ASSERT_NO_FATAL_FAILURE(prepare_virtual());
    ASSERT_TRUE(settings.revert_settings(dd::revert_reason_e::stream_ended, vdd_policy::suppress));
    EXPECT_EQ(std::count(host.writes.begin(), host.writes.end(), "destroy"), 0);
    EXPECT_TRUE(host.vdd_present);
    EXPECT_EQ(host.topology, (dd::active_topology_t { { "physical" } }));
    EXPECT_FALSE(settings.has_persistent_data());
  }

  TEST_F(DisplaySettingsPolicy, FailedRestoreRetainsPersistenceForRetry) {
    host.topology = { { "vdd" } };
    ASSERT_NO_FATAL_FAILURE(prepare_virtual());
    host.fail_topology = true;
    EXPECT_FALSE(settings.revert_settings(dd::revert_reason_e::stream_ended, vdd_policy::allow));
    EXPECT_TRUE(settings.has_persistent_data());
    ASSERT_EQ(host.topology_writes.size(), 1u);
    EXPECT_EQ(host.topology_writes.front().first, (dd::active_topology_t { { "physical" } }));
    EXPECT_FALSE(host.topology_writes.front().second);
    EXPECT_TRUE(host.topology.empty());

    host.fail_topology = false;
    EXPECT_TRUE(settings.revert_settings(dd::revert_reason_e::stream_ended, vdd_policy::allow));
    EXPECT_FALSE(settings.has_persistent_data());
    EXPECT_EQ(host.topology, (dd::active_topology_t { { "physical" } }));
    EXPECT_EQ(std::count(host.writes.begin(), host.writes.end(), "destroy"), 1);
  }

  TEST_F(DisplaySettingsPolicy, ResetAttemptsFinalRestoreThenForgetsEvenOnFailure) {
    host.topology = { { "vdd" } };
    ASSERT_NO_FATAL_FAILURE(prepare_virtual());
    host.fail_topology = true;
    settings.reset_persistence(vdd_policy::allow);
    EXPECT_EQ(std::count(host.writes.begin(), host.writes.end(), "destroy"), 1);
    ASSERT_EQ(host.topology_writes.size(), 1u);
    EXPECT_EQ(host.topology_writes.front().first, (dd::active_topology_t { { "physical" } }));
    EXPECT_FALSE(host.topology_writes.front().second);
    EXPECT_TRUE(host.topology.empty());
    EXPECT_FALSE(settings.has_persistent_data());
    host.writes.clear();
    EXPECT_TRUE(settings.revert_settings(dd::revert_reason_e::stream_ended, vdd_policy::allow));
    EXPECT_TRUE(host.writes.empty());
  }

  TEST_F(DisplaySettingsPolicy, ConfigurationCleanupRestoresModesAndReleasesPersistence) {
    host.vdd_present = false;
    host.topology = { { "physical" } };
    auto config = configuration(false);
    config.resolution = dd::resolution_t { 1280, 720 };
    ASSERT_TRUE(settings.apply_config(config, launch, vdd_policy::allow));
    ASSERT_TRUE(settings.has_persistent_data());
    ASSERT_EQ(host.physical_mode.resolution.width, 1280u);
    config.resolution.reset();
    host.writes.clear();
    ASSERT_TRUE(settings.apply_config(config, launch, vdd_policy::allow));
    EXPECT_EQ(host.physical_mode.resolution.width, 1920u);
    EXPECT_EQ(host.physical_mode.resolution.height, 1080u);
    EXPECT_EQ(host.writes, (std::vector<std::string> { "modes" }));
    EXPECT_FALSE(settings.has_persistent_data());
  }

  TEST_F(DisplaySettingsPolicy, NestedTopologyRollbackPreservesVddSuppression) {
    ASSERT_NO_FATAL_FAILURE(prepare_virtual());
    const auto result = settings.apply_config(configuration(false), launch, vdd_policy::suppress);
    ASSERT_TRUE(result) << result.get_error_message();
    EXPECT_EQ(std::count(host.writes.begin(), host.writes.end(), "destroy"), 0);
    EXPECT_TRUE(host.vdd_present);
    EXPECT_EQ(host.topology, (dd::active_topology_t { { "physical" } }));
    EXPECT_FALSE(settings.has_persistent_data());
  }

  TEST_F(DisplaySettingsPolicy, FailedNestedRollbackPreservesVddSuppressionAndItsError) {
    ASSERT_NO_FATAL_FAILURE(prepare_virtual());
    host.fail_topology = true;
    const auto result = settings.apply_config(configuration(false), launch, vdd_policy::suppress);
    EXPECT_EQ(result.result, dd::settings_t::apply_result_t::result_e::revert_fail);
    EXPECT_EQ(std::count(host.writes.begin(), host.writes.end(), "destroy"), 0);
    EXPECT_TRUE(host.vdd_present);
    EXPECT_EQ(host.topology, (dd::active_topology_t { { "physical" }, { "vdd" } }));
    EXPECT_TRUE(settings.has_persistent_data());
  }

  TEST_F(DisplaySettingsPolicy, FinalLegacyResetRestorePreservesVddSuppression) {
    ASSERT_NO_FATAL_FAILURE(prepare_virtual());
    settings.reset_persistence(vdd_policy::suppress);
    EXPECT_EQ(std::count(host.writes.begin(), host.writes.end(), "destroy"), 0);
    EXPECT_TRUE(host.vdd_present);
    EXPECT_EQ(host.topology, (dd::active_topology_t { { "physical" } }));
    EXPECT_FALSE(settings.has_persistent_data());
    ASSERT_EQ(host.topology_writes.size(), 1u);
    EXPECT_EQ(host.topology_writes.front().first, (dd::active_topology_t { { "physical" } }));
    EXPECT_TRUE(host.topology_writes.front().second);
  }
}  // namespace
