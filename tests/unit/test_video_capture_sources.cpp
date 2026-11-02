/**
 * @file tests/unit/test_video_capture_sources.cpp
 * @brief P2 (provisional pending G1 hardware): exact managed sessions capture
 * their own source through the real capture thread; legacy sessions keep the
 * single shared capture thread.
 */
#include "../tests_common.h"

#include "src/globals.h"
#include "src/video.h"

#include <map>
#include <mutex>
#include <thread>

using namespace std::chrono_literals;

namespace {
  // Each fake source stamps its id into every image's width.
  class fake_display_t: public platf::display_t {
  public:
    explicit fake_display_t(int id):
        _id { id } {}

    platf::capture_e
    capture(const push_captured_image_cb_t &push, const pull_free_image_cb_t &pull, bool *) override {
      while (true) {
        std::shared_ptr<platf::img_t> img;
        if (!pull(img)) {
          return platf::capture_e::ok;
        }
        img->width = _id;
        if (!push(std::move(img), true)) {
          return platf::capture_e::ok;
        }
        std::this_thread::sleep_for(2ms);
      }
    }

    std::shared_ptr<platf::img_t>
    alloc_img() override {
      return std::make_shared<platf::img_t>();
    }

    int
    dummy_img(platf::img_t *) override {
      return 0;
    }

  private:
    int _id;
  };

  class fake_capture_platform_t {
  public:
    explicit fake_capture_platform_t(std::map<std::string, int> sources, std::map<std::string, std::uint64_t> adapters = {}):
        _sources { std::move(sources) }, _adapters { std::move(adapters) } {
      if (!mail::man) {
        mail::man = std::make_shared<safe::mail_raw_t>();
        _installed_mail = true;
      }
      video::testing::set_capture_platform(
        [this](platf::mem_type_e, const std::string &name, const video::config_t &) -> std::shared_ptr<platf::display_t> {
          std::lock_guard lg { _lock };
          ++_opened[name];
          const auto it = _sources.find(name);
          if (it == _sources.end()) {
            return nullptr;
          }
          return std::make_shared<fake_display_t>(it->second);
        },
        [this](platf::mem_type_e) {
          std::vector<std::string> names;
          for (const auto &[name, id] : _sources) {
            names.push_back(name);
          }
          return names;
        },
        [this](const std::string &name) -> std::optional<std::uint64_t> {
          const auto it = _adapters.find(name);
          return it == _adapters.end() ? std::nullopt : std::optional { it->second };
        });
    }

    ~fake_capture_platform_t() {
      video::testing::set_capture_platform({}, {}, {});
      if (_installed_mail) {
        mail::man.reset();
      }
    }

    int
    opened(const std::string &name) {
      std::lock_guard lg { _lock };
      return _opened[name];
    }

  private:
    std::map<std::string, int> _sources;
    std::map<std::string, std::uint64_t> _adapters;
    std::mutex _lock;
    std::map<std::string, int> _opened;
    bool _installed_mail = false;
  };

  video::config_t
  exact_config(const std::string &display) {
    video::config_t config {};
    config.display_name = display;
    config.display_exact = true;
    return config;
  }

  // The source id of the next frame; -1 when no frame arrives.
  int
  next_source(video::testing::capture_attachment_t &attachment) {
    const auto img = attachment.pop(2s);
    return img ? img->width : -1;
  }

  void
  expect_only(video::testing::capture_attachment_t &attachment, int expected) {
    for (int i = 0; i < 20; ++i) {
      ASSERT_EQ(next_source(attachment), expected) << "frame " << i;
    }
  }
}  // namespace

TEST(VideoCaptureSources, ExactSessionsCaptureTheirOwnSources) {
  fake_capture_platform_t platform { { { "FAKE_A", 1 }, { "FAKE_B", 2 } } };

  auto a = video::testing::attach_capture(exact_config("FAKE_A"));
  ASSERT_TRUE(a);
  expect_only(*a, 1);

  auto b = video::testing::attach_capture(exact_config("FAKE_B"));
  ASSERT_TRUE(b);
  expect_only(*b, 2);
  expect_only(*a, 1);
  EXPECT_EQ(platform.opened("FAKE_A"), 1);
  EXPECT_EQ(platform.opened("FAKE_B"), 1);
}

TEST(VideoCaptureSources, ExactSessionsOnOneSourceShareItsCapture) {
  fake_capture_platform_t platform { { { "FAKE_A", 1 }, { "FAKE_B", 2 } } };

  auto first = video::testing::attach_capture(exact_config("FAKE_A"));
  ASSERT_TRUE(first);
  expect_only(*first, 1);
  auto second = video::testing::attach_capture(exact_config("FAKE_A"));
  ASSERT_TRUE(second);
  expect_only(*second, 1);
  EXPECT_EQ(platform.opened("FAKE_A"), 1);
  EXPECT_EQ(platform.opened("FAKE_B"), 0);
}

TEST(VideoCaptureSources, SourceRestartsAfterItsLastSessionLeaves) {
  fake_capture_platform_t platform { { { "FAKE_A", 1 } } };

  auto first = video::testing::attach_capture(exact_config("FAKE_A"));
  ASSERT_TRUE(first);
  expect_only(*first, 1);
  first.reset();

  auto again = video::testing::attach_capture(exact_config("FAKE_A"));
  ASSERT_TRUE(again);
  expect_only(*again, 1);
  EXPECT_EQ(platform.opened("FAKE_A"), 2);
}

TEST(VideoCaptureSources, LegacySessionsShareOneCaptureBesideExactSources) {
  fake_capture_platform_t platform { { { "FAKE_A", 1 }, { "FAKE_B", 2 } } };

  // A later legacy session joins the shared thread's display, as before.
  video::config_t legacy {};
  legacy.display_name = "FAKE_B";
  auto legacy_first = video::testing::attach_capture(legacy);
  ASSERT_TRUE(legacy_first);
  expect_only(*legacy_first, 2);

  auto exact = video::testing::attach_capture(exact_config("FAKE_A"));
  ASSERT_TRUE(exact);
  expect_only(*exact, 1);

  video::config_t other {};
  other.display_name = "FAKE_A";
  auto legacy_second = video::testing::attach_capture(other);
  ASSERT_TRUE(legacy_second);
  expect_only(*legacy_second, 2);
  expect_only(*legacy_first, 2);
  EXPECT_EQ(platform.opened("FAKE_B"), 1);
}

TEST(VideoCaptureSources, DisplaySwitchMovesOnlyTheLegacyCapture) {
  fake_capture_platform_t platform { { { "FAKE_A", 1 }, { "FAKE_B", 2 }, { "FAKE_C", 3 } } };

  video::config_t legacy {};
  legacy.display_name = "FAKE_B";
  auto shared = video::testing::attach_capture(legacy);
  ASSERT_TRUE(shared);
  expect_only(*shared, 2);
  auto exact = video::testing::attach_capture(exact_config("FAKE_C"));
  ASSERT_TRUE(exact);
  expect_only(*exact, 3);

  mail::man->event<int>(mail::switch_display)->raise(0);

  bool switched = false;
  for (int i = 0; i < 500 && !switched; ++i) {
    switched = next_source(*shared) == 1;
  }
  EXPECT_TRUE(switched);
  expect_only(*exact, 3);
  EXPECT_EQ(platform.opened("FAKE_C"), 1);
}

TEST(VideoCaptureSources, ExactSourceDropsASwitchNoLegacyCaptureWillTake) {
  fake_capture_platform_t platform { { { "FAKE_A", 1 }, { "FAKE_C", 3 } } };

  auto exact = video::testing::attach_capture(exact_config("FAKE_C"));
  ASSERT_TRUE(exact);
  expect_only(*exact, 3);

  auto switch_event = mail::man->event<int>(mail::switch_display);
  switch_event->raise(0);
  bool dropped = false;
  for (int i = 0; i < 200 && !dropped; ++i) {
    std::this_thread::sleep_for(10ms);
    dropped = !switch_event->peek();
  }
  EXPECT_TRUE(dropped);
  expect_only(*exact, 3);
  EXPECT_EQ(platform.opened("FAKE_C"), 1);
  EXPECT_EQ(platform.opened("FAKE_A"), 0);
}

TEST(VideoCaptureSources, ExactSessionRefusesAMissingSource) {
  fake_capture_platform_t platform { { { "FAKE_A", 1 } } };

  auto missing = video::testing::attach_capture(exact_config("FAKE_GONE"));
  if (missing) {
    EXPECT_EQ(next_source(*missing), -1);
  }
  EXPECT_EQ(platform.opened("FAKE_A"), 0);
}

TEST(VideoLiveAdmission, GlobalProbeNeverRunsOnAFakePlatform) {
  fake_capture_platform_t platform { { { "FAKE_A", 1 } } };
  const auto encoder = video::testing::chosen_encoder_name();

  EXPECT_NE(video::probe_encoders(video::probe_target_t { "FAKE_A", video::probe_target_policy_e::exact }), 0);
  EXPECT_NE(video::probe_encoders(), 0);
  EXPECT_EQ(video::testing::chosen_encoder_name(), encoder);
  EXPECT_EQ(platform.opened("FAKE_A"), 0);
}

TEST(VideoLiveAdmission, ExactTargetBesideALiveStreamKeepsTheEncoder) {
  fake_capture_platform_t platform { { { "FAKE_A", 1 }, { "FAKE_B", 2 } } };
  auto live = video::testing::attach_capture(exact_config("FAKE_A"));
  ASSERT_TRUE(live);
  expect_only(*live, 1);
  const auto encoder = video::testing::chosen_encoder_name();
  ASSERT_FALSE(encoder.empty());

  EXPECT_EQ(video::probe_encoders(video::probe_target_t { "FAKE_B", video::probe_target_policy_e::exact }), 0)
    << video::last_encoder_probe_result.message;
  EXPECT_TRUE(video::last_encoder_probe_result);
  EXPECT_EQ(video::testing::chosen_encoder_name(), encoder);
  expect_only(*live, 1);
  EXPECT_EQ(platform.opened("FAKE_A"), 1);
  EXPECT_EQ(platform.opened("FAKE_B"), 0);
}

TEST(VideoLiveAdmission, ExactTargetBesideALegacyStreamKeepsTheEncoder) {
  fake_capture_platform_t platform { { { "FAKE_A", 1 }, { "FAKE_B", 2 } } };
  video::config_t legacy {};
  legacy.display_name = "FAKE_A";
  auto live = video::testing::attach_capture(legacy);
  ASSERT_TRUE(live);
  expect_only(*live, 1);
  const auto encoder = video::testing::chosen_encoder_name();

  EXPECT_EQ(video::probe_encoders(video::probe_target_t { "FAKE_B", video::probe_target_policy_e::exact }), 0)
    << video::last_encoder_probe_result.message;
  EXPECT_EQ(video::testing::chosen_encoder_name(), encoder);
}

TEST(VideoLiveAdmission, LiveAdmissionRefusesAnUnavailableTarget) {
  fake_capture_platform_t platform { { { "FAKE_A", 1 } } };
  auto live = video::testing::attach_capture(exact_config("FAKE_A"));
  ASSERT_TRUE(live);
  expect_only(*live, 1);
  const auto encoder = video::testing::chosen_encoder_name();

  EXPECT_NE(video::probe_encoders(video::probe_target_t { "FAKE_GONE", video::probe_target_policy_e::exact }), 0);
  EXPECT_EQ(video::last_encoder_probe_result.error, video::probe_error_e::no_active_display);
  EXPECT_EQ(video::testing::chosen_encoder_name(), encoder);
  expect_only(*live, 1);
}

TEST(VideoLiveAdmission, NonExactTargetsNeverTakeTheLiveAdmission) {
  fake_capture_platform_t platform { { { "FAKE_A", 1 }, { "FAKE_B", 2 } } };
  auto live = video::testing::attach_capture(exact_config("FAKE_A"));
  ASSERT_TRUE(live);
  expect_only(*live, 1);

  // Falls through to the global probe, which the fake platform refuses.
  EXPECT_NE(video::probe_encoders(video::probe_target_t { "FAKE_B", video::probe_target_policy_e::vdd_compatible }), 0);
  EXPECT_NE(video::probe_encoders(), 0);
}

TEST(VideoLiveAdmission, HardwareEncoderRefusesAnUnknownAdapter) {
  fake_capture_platform_t platform { { { "FAKE_A", 1 }, { "FAKE_B", 2 } } };
  auto live = video::testing::attach_capture(exact_config("FAKE_A"));
  ASSERT_TRUE(live);
  expect_only(*live, 1);
  video::testing::scoped_hardware_encoder_t hardware { 0x10 };
  const auto encoder = video::testing::chosen_encoder_name();
  ASSERT_NE(encoder, "software");

  EXPECT_NE(video::probe_encoders(video::probe_target_t { "FAKE_B", video::probe_target_policy_e::exact }), 0);
  EXPECT_EQ(video::last_encoder_probe_result.error, video::probe_error_e::no_working_encoder);
  EXPECT_NE(video::last_encoder_probe_result.message.find("adapter"), std::string::npos)
    << video::last_encoder_probe_result.message;
  EXPECT_EQ(video::testing::chosen_encoder_name(), encoder);
  EXPECT_EQ(platform.opened("FAKE_B"), 0);
}

TEST(VideoLiveAdmission, HardwareEncoderAdmitsItsValidatedAdapter) {
  fake_capture_platform_t platform { { { "FAKE_A", 1 }, { "FAKE_B", 2 } }, { { "FAKE_A", 0x10 }, { "FAKE_B", 0x10 } } };
  auto live = video::testing::attach_capture(exact_config("FAKE_A"));
  ASSERT_TRUE(live);
  expect_only(*live, 1);
  video::testing::scoped_hardware_encoder_t hardware { 0x10 };
  const auto encoder = video::testing::chosen_encoder_name();

  EXPECT_EQ(video::probe_encoders(video::probe_target_t { "FAKE_B", video::probe_target_policy_e::exact }), 0)
    << video::last_encoder_probe_result.message;
  EXPECT_EQ(video::testing::chosen_encoder_name(), encoder);
  expect_only(*live, 1);
  EXPECT_EQ(platform.opened("FAKE_B"), 0);
}

TEST(VideoLiveAdmission, HardwareEncoderRefusesAnotherAdapter) {
  fake_capture_platform_t platform { { { "FAKE_A", 1 }, { "FAKE_B", 2 } }, { { "FAKE_A", 0x10 }, { "FAKE_B", 0x20 } } };
  auto live = video::testing::attach_capture(exact_config("FAKE_A"));
  ASSERT_TRUE(live);
  expect_only(*live, 1);
  video::testing::scoped_hardware_encoder_t hardware { 0x10 };
  const auto encoder = video::testing::chosen_encoder_name();

  EXPECT_NE(video::probe_encoders(video::probe_target_t { "FAKE_B", video::probe_target_policy_e::exact }), 0);
  EXPECT_EQ(video::last_encoder_probe_result.error, video::probe_error_e::no_working_encoder);
  EXPECT_NE(video::last_encoder_probe_result.message.find("adapter"), std::string::npos)
    << video::last_encoder_probe_result.message;
  EXPECT_EQ(video::testing::chosen_encoder_name(), encoder);
}

TEST(VideoLiveAdmission, HardwareEncoderRefusesWithoutAValidatedAdapter) {
  fake_capture_platform_t platform { { { "FAKE_A", 1 }, { "FAKE_B", 2 } }, { { "FAKE_A", 0x10 }, { "FAKE_B", 0x10 } } };
  auto live = video::testing::attach_capture(exact_config("FAKE_A"));
  ASSERT_TRUE(live);
  expect_only(*live, 1);
  video::testing::scoped_hardware_encoder_t hardware { 0 };

  EXPECT_NE(video::probe_encoders(video::probe_target_t { "FAKE_B", video::probe_target_policy_e::exact }), 0);
  EXPECT_EQ(video::last_encoder_probe_result.error, video::probe_error_e::no_working_encoder);
}

TEST(VideoLiveAdmission, SoftwareEncoderIgnoresAdapters) {
  fake_capture_platform_t platform { { { "FAKE_A", 1 }, { "FAKE_B", 2 } }, { { "FAKE_A", 0x10 }, { "FAKE_B", 0x20 } } };
  auto live = video::testing::attach_capture(exact_config("FAKE_A"));
  ASSERT_TRUE(live);
  expect_only(*live, 1);
  ASSERT_EQ(video::testing::chosen_encoder_name(), "software");

  EXPECT_EQ(video::probe_encoders(video::probe_target_t { "FAKE_B", video::probe_target_policy_e::exact }), 0)
    << video::last_encoder_probe_result.message;
}

TEST(VideoLiveAdmission, NamesOnlyPlatformStillBlocksTheGlobalProbe) {
  video::testing::set_capture_platform({}, [](platf::mem_type_e) { return std::vector<std::string> { "FAKE_A" }; }, {});
  const auto result = video::probe_encoders();
  const auto message = video::last_encoder_probe_result.message;
  video::testing::set_capture_platform({}, {}, {});

  EXPECT_NE(result, 0);
  EXPECT_NE(message.find("test capture platform"), std::string::npos) << message;
}
