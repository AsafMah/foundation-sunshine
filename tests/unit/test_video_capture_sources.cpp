/**
 * @file tests/unit/test_video_capture_sources.cpp
 * @brief P2 (provisional pending G1 hardware): exact managed sessions capture
 * their own source through the real capture thread; legacy sessions keep the
 * single shared capture thread.
 */
#include "../tests_common.h"

#include "src/globals.h"
#include "src/utility.h"
#include "src/video.h"
#include "src/video_admission_policy.h"

#include <atomic>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {
  // Each fake source stamps its id into every image's width.
  class fake_display_t: public platf::display_t {
  public:
    explicit fake_display_t(int id, std::function<void()> on_close = {}):
        _id { id }, _on_close { std::move(on_close) } {}

    ~fake_display_t() override {
      if (_on_close) {
        _on_close();
      }
    }

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
    std::function<void()> _on_close;
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
          return std::make_shared<fake_display_t>(it->second, [this] { closed(); });
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

    // Runs as each source's display is destroyed, on its capture thread.
    void
    on_close(std::function<void()> callback) {
      std::lock_guard lg { _lock };
      _on_close = std::move(callback);
    }

  private:
    void
    closed() {
      std::function<void()> callback;
      {
        std::lock_guard lg { _lock };
        callback = _on_close;
      }
      if (callback) {
        callback();
      }
    }

    std::function<void()> _on_close;
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

namespace {
  namespace admission = video::admission;
  constexpr video::probe_mode_t mode_1080p60 { 1920, 1080, 60 };

  class scoped_budget_t {
  public:
    explicit scoped_budget_t(std::map<std::uint64_t, admission::adapter_limits_t> adapters):
        budget { std::make_shared<admission::budget_t>(admission::policy_t { std::move(adapters), std::chrono::milliseconds { 5000 }, 64 },
          [] { return display_device::topology::monotonic_time_t { 1000 }; }) } {
      video::testing::set_admission_budget(budget);
    }
    ~scoped_budget_t() {
      video::testing::set_admission_budget(nullptr);
    }
    std::uint64_t
    sessions(std::uint64_t adapter) const {
      return budget->held(admission::resource_kind_e::encode_session, adapter);
    }
    std::shared_ptr<admission::budget_t> budget;
  };

  video::probe_target_t
  exact_target(const std::string &name, std::optional<video::probe_mode_t> mode = mode_1080p60) {
    return { name, video::probe_target_policy_e::exact, mode };
  }

  constexpr std::uint64_t ample_rate = 1920ull * 1080 * 60 * 8;
}  // namespace

TEST(VideoLiveAdmission, EachCallReportsItsOwnOutcome) {
  fake_capture_platform_t platform { { { "FAKE_A", 1 }, { "FAKE_B", 2 } } };
  auto live = video::testing::attach_capture(exact_config("FAKE_A"));
  ASSERT_TRUE(live);
  expect_only(*live, 1);

  std::atomic<int> crossed {};
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; ++t) {
    threads.emplace_back([&, t] {
      for (int i = 0; i < 50; ++i) {
        const bool available = t % 2 == 0;
        auto outcome = video::probe_encoders_with_outcome(exact_target(available ? "FAKE_B" : "FAKE_GONE", std::nullopt));
        const bool ok = outcome.status == 0 && outcome.result.error == video::probe_error_e::none;
        const bool refused = outcome.status != 0 && outcome.result.error == video::probe_error_e::no_active_display;
        if (available ? !ok : !refused) ++crossed;
      }
    });
  }
  for (auto &thread : threads) thread.join();
  EXPECT_EQ(crossed.load(), 0);
}

TEST(VideoLiveAdmission, AdmissionIsReservedOnTheValidatedAdapter) {
  fake_capture_platform_t platform { { { "FAKE_A", 1 }, { "FAKE_B", 2 } }, { { "FAKE_A", 0x10 }, { "FAKE_B", 0x10 } } };
  auto live = video::testing::attach_capture(exact_config("FAKE_A"));
  ASSERT_TRUE(live);
  video::testing::scoped_hardware_encoder_t hardware { 0x10 };
  scoped_budget_t budget { { { 0x10, { 2, ample_rate } } } };

  auto outcome = video::probe_encoders_with_outcome(exact_target("FAKE_B"));
  ASSERT_EQ(outcome.status, 0) << outcome.result.message;
  EXPECT_EQ(budget.sessions(0x10), 1u);
  auto lease = outcome.admission.commit();
  ASSERT_TRUE(lease) << lease.error().detail;
  EXPECT_EQ(budget.sessions(0x10), 1u);
  *lease = admission::lease_t {};
  EXPECT_EQ(budget.sessions(0x10), 0u);
}

TEST(VideoLiveAdmission, AdmissionBeyondTheBudgetIsRefused) {
  fake_capture_platform_t platform { { { "FAKE_A", 1 }, { "FAKE_B", 2 } }, { { "FAKE_A", 0x10 }, { "FAKE_B", 0x10 } } };
  auto live = video::testing::attach_capture(exact_config("FAKE_A"));
  ASSERT_TRUE(live);
  video::testing::scoped_hardware_encoder_t hardware { 0x10 };
  const auto encoder = video::testing::chosen_encoder_name();
  scoped_budget_t budget { { { 0x10, { 1, ample_rate } } } };

  auto first = video::probe_encoders_with_outcome(exact_target("FAKE_B"));
  ASSERT_EQ(first.status, 0) << first.result.message;
  auto second = video::probe_encoders_with_outcome(exact_target("FAKE_B"));
  EXPECT_NE(second.status, 0);
  EXPECT_EQ(second.result.error, video::probe_error_e::no_working_encoder);
  EXPECT_NE(second.result.message.find("budget"), std::string::npos) << second.result.message;
  EXPECT_TRUE(second.budget_refusal) << "capacity, not a hardware failure: no cooldown";
  EXPECT_EQ(budget.sessions(0x10), 1u);
  EXPECT_EQ(video::testing::chosen_encoder_name(), encoder);

  first = {};
  EXPECT_EQ(budget.sessions(0x10), 0u);
}

TEST(VideoLiveAdmission, TheLegacyEntryPointStillReportsThroughTheGlobalResult) {
  fake_capture_platform_t platform { { { "FAKE_A", 1 }, { "FAKE_B", 2 } }, { { "FAKE_A", 0x10 }, { "FAKE_B", 0x10 } } };
  auto live = video::testing::attach_capture(exact_config("FAKE_A"));
  ASSERT_TRUE(live);
  video::testing::scoped_hardware_encoder_t hardware { 0x10 };
  scoped_budget_t budget { { { 0x10, { 1, ample_rate } } } };

  // A probe whose ticket nobody can carry must not hold the budget.
  EXPECT_EQ(video::probe_encoders(exact_target("FAKE_B")), 0) << video::last_encoder_probe_result.message;
  EXPECT_EQ(budget.sessions(0x10), 0u);
  EXPECT_TRUE(video::last_encoder_probe_result);
}

TEST(VideoLiveAdmission, AnEnforcedBudgetNeedsTheRequestedMode) {
  fake_capture_platform_t platform { { { "FAKE_A", 1 }, { "FAKE_B", 2 } }, { { "FAKE_A", 0x10 }, { "FAKE_B", 0x10 } } };
  auto live = video::testing::attach_capture(exact_config("FAKE_A"));
  ASSERT_TRUE(live);
  video::testing::scoped_hardware_encoder_t hardware { 0x10 };
  scoped_budget_t budget { { { 0x10, { 4, ample_rate } } } };

  auto outcome = video::probe_encoders_with_outcome(exact_target("FAKE_B", std::nullopt));
  EXPECT_NE(outcome.status, 0);
  EXPECT_EQ(outcome.result.error, video::probe_error_e::no_working_encoder);
  EXPECT_NE(outcome.result.message.find("needs the requested mode"), std::string::npos) << outcome.result.message;
  EXPECT_TRUE(outcome.budget_refusal);
  EXPECT_EQ(budget.sessions(0x10), 0u);
}

TEST(VideoLiveAdmission, WithoutABudgetNothingIsAccounted) {
  fake_capture_platform_t platform { { { "FAKE_A", 1 }, { "FAKE_B", 2 } }, { { "FAKE_A", 0x10 }, { "FAKE_B", 0x10 } } };
  auto live = video::testing::attach_capture(exact_config("FAKE_A"));
  ASSERT_TRUE(live);
  video::testing::scoped_hardware_encoder_t hardware { 0x10 };

  auto outcome = video::probe_encoders_with_outcome(exact_target("FAKE_B"));
  ASSERT_EQ(outcome.status, 0) << outcome.result.message;
  EXPECT_TRUE(outcome.admission.commit());
}

TEST(VideoLiveAdmission, SoftwareAdmissionIsAccountedOnTheDisplayAdapter) {
  fake_capture_platform_t platform { { { "FAKE_A", 1 }, { "FAKE_B", 2 }, { "FAKE_C", 3 } }, { { "FAKE_A", 0x10 }, { "FAKE_B", 0x20 } } };
  auto live = video::testing::attach_capture(exact_config("FAKE_A"));
  ASSERT_TRUE(live);
  ASSERT_EQ(video::testing::chosen_encoder_name(), "software");
  scoped_budget_t budget { { { 0x20, { 2, ample_rate } } } };

  auto outcome = video::probe_encoders_with_outcome(exact_target("FAKE_B"));
  ASSERT_EQ(outcome.status, 0) << outcome.result.message;
  EXPECT_EQ(budget.sessions(0x20), 1u);

  // Room remains on 0x20, so only the unknown adapter can refuse this.
  auto unknown = video::probe_encoders_with_outcome(exact_target("FAKE_C"));
  EXPECT_NE(unknown.status, 0);
  EXPECT_NE(unknown.result.message.find("unknown adapter"), std::string::npos) << unknown.result.message;
  EXPECT_TRUE(unknown.budget_refusal);
  EXPECT_EQ(budget.sessions(0x20), 1u);
}

namespace {
  // The production managed policy, on a fixed clock.
  class scoped_managed_budget_t {
  public:
    scoped_managed_budget_t():
        budget { std::make_shared<admission::budget_t>(admission::production_policy(true),
          [] { return display_device::topology::monotonic_time_t { 1000 }; }) } {
      video::testing::set_admission_budget(budget);
    }
    ~scoped_managed_budget_t() {
      video::testing::set_admission_budget(nullptr);
    }
    std::uint64_t
    sessions(std::uint64_t adapter) const {
      return budget->held(admission::resource_kind_e::encode_session, adapter);
    }
    std::shared_ptr<admission::budget_t> budget;
  };

  video::encoder_capacity_t
  reported_sessions(std::uint32_t sessions) {
    video::encoder_capacity_t capacity;
    capacity.encode_sessions = sessions;
    capacity.macroblocks_per_second = 4ull * 3840 * 2160 * 120 / 256;
    return capacity;
  }
}  // namespace

TEST(VideoAdmissionPolicyWiring, ProbesAskForCapacityOnlyUnderAnEnforcedBudget) {
  EXPECT_FALSE(video::testing::capacity_reporting_requested()) << "no budget: the blank profile";
  {
    video::testing::set_admission_budget(std::make_shared<admission::budget_t>(admission::production_policy(false),
      [] { return display_device::topology::monotonic_time_t { 1000 }; }));
    EXPECT_FALSE(video::testing::capacity_reporting_requested()) << "an unenforced budget";
    video::testing::set_admission_budget(nullptr);
  }
  scoped_managed_budget_t budget;
  EXPECT_TRUE(video::testing::capacity_reporting_requested());
  EXPECT_FALSE(video::config_t {}.report_admission_capacity) << "a stream's config never asks";
}

TEST(VideoAdmissionPolicyWiring, AManagedBudgetIsInstalledOnceAndKeepsWhatItLearned) {
  ASSERT_FALSE(video::testing::capacity_reporting_requested());
  fake_capture_platform_t platform { { { "FAKE_A", 1 }, { "FAKE_B", 2 } }, { { "FAKE_A", 0x10 }, { "FAKE_B", 0x10 } } };
  auto live = video::testing::attach_capture(exact_config("FAKE_A"));
  ASSERT_TRUE(live);
  video::install_managed_admission_budget();
  auto cleanup = util::fail_guard([] { video::testing::set_admission_budget(nullptr); });
  ASSERT_TRUE(video::testing::capacity_reporting_requested());

  video::testing::learn_validated_capacity(0x10, reported_sessions(1));
  video::install_managed_admission_budget();
  video::testing::scoped_hardware_encoder_t hardware { 0x10 };
  auto first = video::probe_encoders_with_outcome(exact_target("FAKE_B"));
  ASSERT_EQ(first.status, 0) << first.result.message;
  auto second = video::probe_encoders_with_outcome(exact_target("FAKE_A"));
  EXPECT_NE(second.status, 0) << "the reinstall must not reset the learned single session";
  EXPECT_TRUE(second.budget_refusal);
}

TEST(VideoAdmissionPolicyWiring, ALearnedSessionLimitBoundsLiveAdmission) {
  fake_capture_platform_t platform { { { "FAKE_A", 1 }, { "FAKE_B", 2 } }, { { "FAKE_A", 0x10 }, { "FAKE_B", 0x10 } } };
  auto live = video::testing::attach_capture(exact_config("FAKE_A"));
  ASSERT_TRUE(live);
  video::testing::scoped_hardware_encoder_t hardware { 0x10 };
  scoped_managed_budget_t budget;
  video::testing::learn_validated_capacity(0x10, reported_sessions(1));
  ASSERT_TRUE(budget.budget->limits(0x10));
  EXPECT_EQ(budget.budget->limits(0x10)->encode_sessions, 1u);

  auto first = video::probe_encoders_with_outcome(exact_target("FAKE_B"));
  ASSERT_EQ(first.status, 0) << first.result.message;
  auto second = video::probe_encoders_with_outcome(exact_target("FAKE_B"));
  EXPECT_NE(second.status, 0);
  EXPECT_TRUE(second.budget_refusal);
  EXPECT_EQ(budget.sessions(0x10), 1u);
}

TEST(VideoAdmissionPolicyWiring, LearningNeverMovesAHeldAdapter) {
  fake_capture_platform_t platform { { { "FAKE_A", 1 }, { "FAKE_B", 2 } }, { { "FAKE_A", 0x10 }, { "FAKE_B", 0x10 } } };
  auto live = video::testing::attach_capture(exact_config("FAKE_A"));
  ASSERT_TRUE(live);
  video::testing::scoped_hardware_encoder_t hardware { 0x10 };
  scoped_managed_budget_t budget;
  video::testing::learn_validated_capacity(0x10, reported_sessions(2));
  auto held = video::probe_encoders_with_outcome(exact_target("FAKE_B"));
  ASSERT_EQ(held.status, 0) << held.result.message;

  video::testing::learn_validated_capacity(0x10, reported_sessions(1));
  EXPECT_EQ(budget.budget->limits(0x10)->encode_sessions, 2u);
}

TEST(VideoAdmissionPolicyWiring, AnUnprobedEnumeratedAdapterTakesTheFallback) {
  fake_capture_platform_t platform { { { "FAKE_A", 1 }, { "FAKE_B", 2 } }, { { "FAKE_A", 0x10 }, { "FAKE_B", 0x20 } } };
  auto live = video::testing::attach_capture(exact_config("FAKE_A"));
  ASSERT_TRUE(live);
  ASSERT_EQ(video::testing::chosen_encoder_name(), "software");
  scoped_managed_budget_t budget;
  ASSERT_FALSE(budget.budget->limits(0x20));

  auto outcome = video::probe_encoders_with_outcome(exact_target("FAKE_B"));
  ASSERT_EQ(outcome.status, 0) << outcome.result.message;
  EXPECT_EQ(budget.sessions(0x20), 1u);
  ASSERT_TRUE(budget.budget->limits(0x20));
  EXPECT_EQ(budget.budget->limits(0x20)->encode_sessions, admission::fallback_encode_sessions);
  EXPECT_EQ(budget.budget->limits(0x20)->pixel_rate, admission::fallback_pixel_rate);
}

TEST(VideoAdmissionPolicyWiring, ADisplayWithoutAnAdapterIsStillRefused) {
  fake_capture_platform_t platform { { { "FAKE_A", 1 }, { "FAKE_C", 3 } }, { { "FAKE_A", 0x10 } } };
  auto live = video::testing::attach_capture(exact_config("FAKE_A"));
  ASSERT_TRUE(live);
  ASSERT_EQ(video::testing::chosen_encoder_name(), "software");
  scoped_managed_budget_t budget;

  auto outcome = video::probe_encoders_with_outcome(exact_target("FAKE_C"));
  EXPECT_NE(outcome.status, 0);
  EXPECT_NE(outcome.result.message.find("unknown adapter"), std::string::npos) << outcome.result.message;
  EXPECT_TRUE(outcome.budget_refusal);
}

TEST(VideoAdmissionPolicyWiring, WithoutAFallbackAnUnlistedAdapterIsRefused) {
  fake_capture_platform_t platform { { { "FAKE_A", 1 }, { "FAKE_B", 2 } }, { { "FAKE_A", 0x10 }, { "FAKE_B", 0x20 } } };
  auto live = video::testing::attach_capture(exact_config("FAKE_A"));
  ASSERT_TRUE(live);
  scoped_budget_t budget { { { 0x10, { 2, ample_rate } } } };

  auto outcome = video::probe_encoders_with_outcome(exact_target("FAKE_B"));
  EXPECT_NE(outcome.status, 0);
  EXPECT_TRUE(outcome.budget_refusal);
  EXPECT_NE(outcome.result.message.find("has no limits"), std::string::npos)
    << "refused by the ensure check, not incidentally by reserve: " << outcome.result.message;
  EXPECT_FALSE(budget.budget->limits(0x20));
  EXPECT_EQ(budget.sessions(0x20), 0u);
}

TEST(VideoAdmissionPolicyWiring, LearningWithoutABudgetChangesNothing) {
  video::testing::learn_validated_capacity(0x10, reported_sessions(1));
  EXPECT_FALSE(video::testing::capacity_reporting_requested());
}

TEST(VideoLiveAdmission, EncoderAndDisplayRefusalsAreNotBudgetRefusals) {
  fake_capture_platform_t platform { { { "FAKE_A", 1 }, { "FAKE_B", 2 } }, { { "FAKE_A", 0x10 }, { "FAKE_B", 0x20 } } };
  auto live = video::testing::attach_capture(exact_config("FAKE_A"));
  ASSERT_TRUE(live);
  video::testing::scoped_hardware_encoder_t hardware { 0x10 };
  scoped_budget_t budget { { { 0x10, { 4, ample_rate } }, { 0x20, { 4, ample_rate } } } };

  const auto other_adapter = video::probe_encoders_with_outcome(exact_target("FAKE_B"));
  EXPECT_NE(other_adapter.status, 0);
  EXPECT_NE(other_adapter.result.message.find("adapter"), std::string::npos) << other_adapter.result.message;
  EXPECT_FALSE(other_adapter.budget_refusal) << "an encoder that cannot run on the target is a hardware failure";

  const auto missing = video::probe_encoders_with_outcome(exact_target("FAKE_MISSING"));
  EXPECT_NE(missing.status, 0);
  EXPECT_EQ(missing.result.error, video::probe_error_e::no_active_display);
  EXPECT_FALSE(missing.budget_refusal);
}

TEST(VideoLiveAdmission, ChoosingTheEncoderAdvancesTheGeneration) {
  if (!video::testing::chosen_encoder_name().empty()) {
    GTEST_SKIP() << "An encoder was already chosen by an earlier test";
  }
  const auto before = video::testing::encoder_generation();
  std::uint64_t during {};
  {
    fake_capture_platform_t platform { { { "FAKE_A", 1 } } };
    ASSERT_EQ(video::testing::chosen_encoder_name(), "software");
    during = video::testing::encoder_generation();
    EXPECT_NE(during, before);
  }
  EXPECT_TRUE(video::testing::chosen_encoder_name().empty());
  EXPECT_NE(video::testing::encoder_generation(), during);
}

TEST(VideoLiveAdmission, TheOutcomeCarriesTheEncoderGeneration) {
  fake_capture_platform_t platform { { { "FAKE_A", 1 }, { "FAKE_B", 2 } }, { { "FAKE_A", 0x10 }, { "FAKE_B", 0x10 } } };
  auto live = video::testing::attach_capture(exact_config("FAKE_A"));
  ASSERT_TRUE(live);
  const auto software = video::probe_encoders_with_outcome(exact_target("FAKE_B", std::nullopt));
  ASSERT_EQ(software.status, 0) << software.result.message;
  EXPECT_EQ(software.encoder_generation, video::testing::encoder_generation());
  std::uint64_t hardware_generation {};
  {
    video::testing::scoped_hardware_encoder_t hardware { 0x10 };
    const auto outcome = video::probe_encoders_with_outcome(exact_target("FAKE_B", std::nullopt));
    ASSERT_EQ(outcome.status, 0) << outcome.result.message;
    hardware_generation = outcome.encoder_generation;
    EXPECT_NE(hardware_generation, software.encoder_generation);
  }
  EXPECT_NE(video::testing::encoder_generation(), hardware_generation);
}

namespace {
  class scoped_global_probe_t {
  public:
    explicit scoped_global_probe_t(int status) {
      video::testing::set_global_probe([status] { return status; });
    }
    ~scoped_global_probe_t() {
      video::testing::set_global_probe({});
    }
  };

  std::shared_ptr<video::admission_ticket_t>
  ticket_from(video::probe_outcome_t &outcome) {
    auto ticket = std::make_shared<video::admission_ticket_t>();
    ticket->pending = std::move(outcome.admission);
    ticket->encoder_generation = outcome.encoder_generation;
    return ticket;
  }

  video::config_t
  ticketed_config(const std::string &display, std::shared_ptr<video::admission_ticket_t> ticket) {
    auto config = exact_config(display);
    config.admission = std::move(ticket);
    return config;
  }
}  // namespace

TEST(VideoAdmissionTicket, CommitsBeforeAttachAndReleasesWithTheCapture) {
  fake_capture_platform_t platform { { { "FAKE_A", 1 }, { "FAKE_B", 2 } }, { { "FAKE_A", 0x10 }, { "FAKE_B", 0x10 } } };
  auto live = video::testing::attach_capture(exact_config("FAKE_A"));
  ASSERT_TRUE(live);
  video::testing::scoped_hardware_encoder_t hardware { 0x10 };
  scoped_budget_t budget { { { 0x10, { 2, ample_rate } } } };

  auto outcome = video::probe_encoders_with_outcome(exact_target("FAKE_B"));
  ASSERT_EQ(outcome.status, 0) << outcome.result.message;
  const auto ticket = ticket_from(outcome);
  auto attached = video::testing::attach_capture(ticketed_config("FAKE_B", ticket));
  ASSERT_TRUE(attached);
  expect_only(*attached, 2);
  EXPECT_EQ(budget.sessions(0x10), 1u);
  EXPECT_FALSE(ticket->pending.commit()) << "the attach committed the ticket";
  attached.reset();
  EXPECT_EQ(budget.sessions(0x10), 0u) << "the lease ends with the capture ref";
}

TEST(VideoAdmissionTicket, AStaleGenerationIsRefusedBeforeTheSourceIsAcquired) {
  fake_capture_platform_t platform { { { "FAKE_A", 1 }, { "FAKE_B", 2 } }, { { "FAKE_A", 0x10 }, { "FAKE_B", 0x10 } } };
  auto live = video::testing::attach_capture(exact_config("FAKE_A"));
  ASSERT_TRUE(live);
  video::testing::scoped_hardware_encoder_t hardware { 0x10 };
  scoped_budget_t budget { { { 0x10, { 2, ample_rate } } } };

  auto outcome = video::probe_encoders_with_outcome(exact_target("FAKE_B"));
  ASSERT_EQ(outcome.status, 0) << outcome.result.message;
  auto ticket = ticket_from(outcome);
  {
    video::testing::scoped_hardware_encoder_t reselected { 0x10 };
    EXPECT_FALSE(video::testing::attach_capture(ticketed_config("FAKE_B", ticket)));
  }
  EXPECT_FALSE(video::testing::attach_capture(ticketed_config("FAKE_B", ticket))) << "a reselection in between is never undone";
  EXPECT_EQ(platform.opened("FAKE_B"), 0);
  EXPECT_EQ(budget.sessions(0x10), 1u) << "still pending";
  ticket.reset();
  EXPECT_EQ(budget.sessions(0x10), 0u);
}

TEST(VideoAdmissionTicket, ATicketAdmitsOneCapture) {
  fake_capture_platform_t platform { { { "FAKE_A", 1 }, { "FAKE_B", 2 } }, { { "FAKE_A", 0x10 }, { "FAKE_B", 0x10 } } };
  auto live = video::testing::attach_capture(exact_config("FAKE_A"));
  ASSERT_TRUE(live);
  video::testing::scoped_hardware_encoder_t hardware { 0x10 };
  scoped_budget_t budget { { { 0x10, { 4, ample_rate } } } };

  auto outcome = video::probe_encoders_with_outcome(exact_target("FAKE_B"));
  ASSERT_EQ(outcome.status, 0) << outcome.result.message;
  const auto config = ticketed_config("FAKE_B", ticket_from(outcome));
  auto first = video::testing::attach_capture(config);
  ASSERT_TRUE(first);
  EXPECT_FALSE(video::testing::attach_capture(config));
  EXPECT_EQ(budget.sessions(0x10), 1u);
}

TEST(VideoAdmissionTicket, AnEmptyTicketIsRefused) {
  fake_capture_platform_t platform { { { "FAKE_A", 1 } } };
  auto ticket = std::make_shared<video::admission_ticket_t>();
  ticket->encoder_generation = video::testing::encoder_generation();
  EXPECT_FALSE(video::testing::attach_capture(ticketed_config("FAKE_A", ticket)));
  EXPECT_EQ(platform.opened("FAKE_A"), 0);
}

TEST(VideoAdmissionTicket, AnExactCaptureWithoutATicketIsRefusedUnderABudget) {
  fake_capture_platform_t platform { { { "FAKE_A", 1 }, { "FAKE_B", 2 } }, { { "FAKE_A", 0x10 }, { "FAKE_B", 0x10 } } };
  scoped_budget_t budget { { { 0x10, { 4, ample_rate } } } };
  EXPECT_FALSE(video::testing::attach_capture(exact_config("FAKE_A")));
  EXPECT_EQ(platform.opened("FAKE_A"), 0);

  // Legacy sessions are not accounted, with or without a budget.
  video::config_t legacy {};
  legacy.display_name = "FAKE_B";
  auto attached = video::testing::attach_capture(legacy);
  ASSERT_TRUE(attached);
  expect_only(*attached, 2);
  EXPECT_EQ(budget.sessions(0x10), 0u);
}

TEST(VideoAdmissionTicket, WithoutABudgetUnticketedCapturesAttachAsBefore) {
  fake_capture_platform_t platform { { { "FAKE_A", 1 } } };
  auto attached = video::testing::attach_capture(exact_config("FAKE_A"));
  ASSERT_TRUE(attached);
  expect_only(*attached, 1);
}

TEST(VideoAdmissionTicket, AnUnattachedTicketReturnsItsReservation) {
  fake_capture_platform_t platform { { { "FAKE_A", 1 }, { "FAKE_B", 2 } }, { { "FAKE_A", 0x10 }, { "FAKE_B", 0x10 } } };
  auto live = video::testing::attach_capture(exact_config("FAKE_A"));
  ASSERT_TRUE(live);
  video::testing::scoped_hardware_encoder_t hardware { 0x10 };
  scoped_budget_t budget { { { 0x10, { 2, ample_rate } } } };
  {
    auto outcome = video::probe_encoders_with_outcome(exact_target("FAKE_B"));
    ASSERT_EQ(outcome.status, 0) << outcome.result.message;
    const auto ticket = ticket_from(outcome);
    EXPECT_EQ(budget.sessions(0x10), 1u);
  }
  EXPECT_EQ(budget.sessions(0x10), 0u);
}

TEST(VideoAdmissionTicket, TheFirstStreamIsReservedByTheGlobalProbe) {
  fake_capture_platform_t platform { { { "FAKE_A", 1 } }, { { "FAKE_A", 0x10 } } };
  video::testing::scoped_hardware_encoder_t hardware { 0x10 };
  scoped_budget_t budget { { { 0x10, { 1, ample_rate } } } };
  scoped_global_probe_t global { 0 };

  const auto before = video::testing::encoder_generation();
  auto outcome = video::probe_encoders_with_outcome(exact_target("FAKE_A"));
  ASSERT_EQ(outcome.status, 0) << outcome.result.message;
  EXPECT_GT(outcome.encoder_generation, before) << "the ticket is fenced on the probe's own selection";
  EXPECT_EQ(outcome.encoder_generation, video::testing::encoder_generation());
  EXPECT_EQ(budget.sessions(0x10), 1u);
  auto attached = video::testing::attach_capture(ticketed_config("FAKE_A", ticket_from(outcome)));
  ASSERT_TRUE(attached);
  expect_only(*attached, 1);
  EXPECT_EQ(budget.sessions(0x10), 1u);
  attached.reset();
  EXPECT_EQ(budget.sessions(0x10), 0u);
}

TEST(VideoAdmissionTicket, TheFirstStreamIsRefusedOverTheBudget) {
  fake_capture_platform_t platform { { { "FAKE_A", 1 } }, { { "FAKE_A", 0x10 } } };
  video::testing::scoped_hardware_encoder_t hardware { 0x10 };
  scoped_budget_t budget { { { 0x10, { 1, ample_rate } } } };
  scoped_global_probe_t global { 0 };

  auto held = video::probe_encoders_with_outcome(exact_target("FAKE_A"));
  ASSERT_EQ(held.status, 0) << held.result.message;
  auto refused = video::probe_encoders_with_outcome(exact_target("FAKE_A"));
  EXPECT_NE(refused.status, 0);
  EXPECT_NE(refused.result.message.find("budget"), std::string::npos) << refused.result.message;
  EXPECT_EQ(video::last_encoder_probe_result.message, refused.result.message) << "the published result reports the refusal";
  EXPECT_EQ(video::probe_encoders(exact_target("FAKE_A", std::nullopt)), -1);
  EXPECT_NE(video::last_encoder_probe_result.message.find("needs the requested mode"), std::string::npos) << video::last_encoder_probe_result.message;
  EXPECT_EQ(budget.sessions(0x10), 1u);
}

TEST(VideoAdmissionTicket, TheFirstStreamNeedsAdapterAgreement) {
  fake_capture_platform_t platform { { { "FAKE_A", 1 } }, { { "FAKE_A", 0x20 } } };
  video::testing::scoped_hardware_encoder_t hardware { 0x10 };
  scoped_budget_t budget { { { 0x10, { 1, ample_rate } }, { 0x20, { 1, ample_rate } } } };
  scoped_global_probe_t global { 0 };

  auto outcome = video::probe_encoders_with_outcome(exact_target("FAKE_A"));
  EXPECT_NE(outcome.status, 0);
  EXPECT_NE(outcome.result.message.find("adapter"), std::string::npos) << outcome.result.message;
  EXPECT_EQ(budget.sessions(0x10) + budget.sessions(0x20), 0u);
}

TEST(VideoAdmissionTicket, TheFirstStreamWithoutABudgetIsUnaccounted) {
  fake_capture_platform_t platform { { { "FAKE_A", 1 } } };
  scoped_global_probe_t global { 0 };
  auto outcome = video::probe_encoders_with_outcome(exact_target("FAKE_A"));
  ASSERT_EQ(outcome.status, 0) << outcome.result.message;
  auto attached = video::testing::attach_capture(ticketed_config("FAKE_A", ticket_from(outcome)));
  ASSERT_TRUE(attached);
  expect_only(*attached, 1);
}

TEST(VideoAdmissionTicket, AFailedOrUntargetedGlobalProbeCarriesNoAdmission) {
  fake_capture_platform_t platform { { { "FAKE_A", 1 } }, { { "FAKE_A", 0x10 } } };
  video::testing::scoped_hardware_encoder_t hardware { 0x10 };
  scoped_budget_t budget { { { 0x10, { 1, ample_rate } } } };
  {
    scoped_global_probe_t global { -1 };
    auto failed = video::probe_encoders_with_outcome(exact_target("FAKE_A"));
    EXPECT_NE(failed.status, 0);
    EXPECT_FALSE(failed.admission.commit());
  }
  scoped_global_probe_t global { 0 };
  auto untargeted = video::probe_encoders_with_outcome();
  ASSERT_EQ(untargeted.status, 0);
  EXPECT_FALSE(untargeted.admission.commit()) << "the startup probe admits nobody";
  EXPECT_EQ(budget.sessions(0x10), 0u);
}

TEST(VideoAdmissionTicket, AStreamWithARefusedTicketShutsDownBeforeCapturing) {
  fake_capture_platform_t platform { { { "FAKE_A", 1 } } };
  auto ticket = std::make_shared<video::admission_ticket_t>();
  ticket->pending = video::admission::pending_t::unaccounted();
  ticket->encoder_generation = video::testing::encoder_generation() + 1;
  auto mail = std::make_shared<safe::mail_raw_t>();
  // Mail posts are held weakly; keep the session's shutdown event alive like the stream does.
  auto shutdown = mail->event<bool>(mail::shutdown);
  video::capture(mail, ticketed_config("FAKE_A", ticket), nullptr);
  EXPECT_TRUE(shutdown->peek());
  EXPECT_EQ(platform.opened("FAKE_A"), 0);
  EXPECT_TRUE(ticket->pending.commit()) << "a refused ticket is not consumed";
}

TEST(VideoAdmissionTicket, TheLeaseOutlivesTheCaptureSource) {
  fake_capture_platform_t platform { { { "FAKE_A", 1 }, { "FAKE_B", 2 } }, { { "FAKE_A", 0x10 }, { "FAKE_B", 0x10 } } };
  auto live = video::testing::attach_capture(exact_config("FAKE_A"));
  ASSERT_TRUE(live);
  video::testing::scoped_hardware_encoder_t hardware { 0x10 };
  scoped_budget_t budget { { { 0x10, { 2, ample_rate } } } };

  auto outcome = video::probe_encoders_with_outcome(exact_target("FAKE_B"));
  ASSERT_EQ(outcome.status, 0) << outcome.result.message;
  auto attached = video::testing::attach_capture(ticketed_config("FAKE_B", ticket_from(outcome)));
  ASSERT_TRUE(attached);
  expect_only(*attached, 2);  // the source's display is open
  std::atomic<int> held_at_close { -1 };
  platform.on_close([&] { held_at_close = static_cast<int>(budget.sessions(0x10)); });
  attached.reset();
  EXPECT_EQ(held_at_close.load(), 1) << "the source closed after its admission was released";
  EXPECT_EQ(budget.sessions(0x10), 0u);
  platform.on_close({});
}
TEST(VideoAdmissionTicket, AReprobeBeforeTheFirstStreamAttachesStalesItsTicket) {
  fake_capture_platform_t platform { { { "FAKE_A", 1 } }, { { "FAKE_A", 0x10 } } };
  video::testing::scoped_hardware_encoder_t hardware { 0x10 };
  scoped_budget_t budget { { { 0x10, { 1, ample_rate } } } };
  scoped_global_probe_t global { 0 };

  auto outcome = video::probe_encoders_with_outcome(exact_target("FAKE_A"));
  ASSERT_EQ(outcome.status, 0) << outcome.result.message;
  auto ticket = ticket_from(outcome);
  ASSERT_EQ(video::probe_encoders_with_outcome().status, 0);
  EXPECT_FALSE(video::testing::attach_capture(ticketed_config("FAKE_A", ticket)));
  EXPECT_EQ(platform.opened("FAKE_A"), 0);
  ticket.reset();
  EXPECT_EQ(budget.sessions(0x10), 0u);
}

namespace {
  // A ticketed FAKE_A capture on adapter 0x10 at 1920x1080x60, as a managed stream holds it.
  struct replacement_fixture_t {
    fake_capture_platform_t platform { { { "FAKE_A", 1 } }, { { "FAKE_A", 0x10 } } };
    video::testing::scoped_hardware_encoder_t hardware { 0x10 };
    scoped_budget_t budget;
    scoped_global_probe_t global { 0 };
    std::shared_ptr<video::admission_ticket_t> ticket;
    std::unique_ptr<video::testing::capture_attachment_t> attached;

    explicit replacement_fixture_t(std::uint32_t sessions):
        budget { { { 0x10, { sessions, ample_rate } } } } {
      auto outcome = video::probe_encoders_with_outcome(exact_target("FAKE_A"));
      EXPECT_EQ(outcome.status, 0) << outcome.result.message;
      ticket = ticket_from(outcome);
      attached = video::testing::attach_capture(ticketed_config("FAKE_A", ticket));
      EXPECT_TRUE(attached);
    }

    std::uint32_t
    held_width() {
      std::lock_guard lg { ticket->lock };
      return ticket->lease.request() ? ticket->lease.request()->width : 0;
    }
  };
}  // namespace

TEST(VideoAdmissionTicket, AReplacementIsAdmittedWhileTheOldModeIsStillHeld) {
  replacement_fixture_t fixture { 2 };
  ASSERT_EQ(fixture.budget.sessions(0x10), 1u);
  ASSERT_EQ(fixture.held_width(), 1920u);

  auto replacement = video::reserve_replacement(fixture.ticket, 1280, 720);
  ASSERT_TRUE(replacement) << replacement.error();
  ASSERT_TRUE(replacement->request());
  EXPECT_EQ(replacement->request()->adapter_luid, 0x10u);
  EXPECT_EQ(replacement->request()->fps, 60u);
  EXPECT_EQ(fixture.budget.sessions(0x10), 2u) << "make before break: both modes are held";
  EXPECT_EQ(fixture.held_width(), 1920u);

  ASSERT_TRUE(video::commit_replacement(fixture.ticket, std::move(*replacement)));
  EXPECT_EQ(fixture.budget.sessions(0x10), 1u) << "the commit released the old mode";
  EXPECT_EQ(fixture.held_width(), 1280u);
  fixture.attached.reset();
  EXPECT_EQ(fixture.budget.sessions(0x10), 0u) << "the capture end releases the replacement";
}

TEST(VideoAdmissionTicket, AReplacementOverTheBudgetIsRefusedAndTheOldModeKept) {
  replacement_fixture_t fixture { 1 };
  auto replacement = video::reserve_replacement(fixture.ticket, 1280, 720);
  ASSERT_FALSE(replacement);
  EXPECT_NE(replacement.error().find("budget"), std::string::npos) << replacement.error();
  EXPECT_EQ(fixture.budget.sessions(0x10), 1u);
  EXPECT_EQ(fixture.held_width(), 1920u);
}

TEST(VideoAdmissionTicket, ADroppedReplacementRollsBack) {
  replacement_fixture_t fixture { 2 };
  {
    auto replacement = video::reserve_replacement(fixture.ticket, 1280, 720);
    ASSERT_TRUE(replacement) << replacement.error();
    EXPECT_EQ(fixture.budget.sessions(0x10), 2u);
  }
  EXPECT_EQ(fixture.budget.sessions(0x10), 1u);
  EXPECT_EQ(fixture.held_width(), 1920u);
}

TEST(VideoAdmissionTicket, NoReplacementAfterTheCaptureEnds) {
  replacement_fixture_t fixture { 3 };
  auto early = video::reserve_replacement(fixture.ticket, 1280, 720);
  ASSERT_TRUE(early) << early.error();
  fixture.attached.reset();
  EXPECT_EQ(fixture.budget.sessions(0x10), 1u) << "only the uncommitted replacement remains";
  const auto committed = video::commit_replacement(fixture.ticket, std::move(*early));
  ASSERT_FALSE(committed);
  EXPECT_NE(committed.error().find("not capturing"), std::string::npos) << committed.error();
  EXPECT_EQ(fixture.budget.sessions(0x10), 0u) << "the refused replacement rolled back";
  const auto late = video::reserve_replacement(fixture.ticket, 1280, 720);
  ASSERT_FALSE(late);
  EXPECT_NE(late.error().find("not capturing"), std::string::npos) << late.error();
  EXPECT_EQ(fixture.held_width(), 0u);
}

TEST(VideoAdmissionTicket, AManagedHeadAlwaysFollowsItsSizeOtherDisplaysHonourTheSetting) {
  EXPECT_TRUE(video::follows_display_resolution(true, false)) << "a client-requested managed change must reach the encoder";
  EXPECT_TRUE(video::follows_display_resolution(true, true));
  EXPECT_TRUE(video::follows_display_resolution(false, true));
  EXPECT_FALSE(video::follows_display_resolution(false, false)) << "legacy opt-out unchanged";
}

TEST(VideoAdmissionTicket, AReplacementIsRefusedBeforeTheCaptureAttaches) {
  fake_capture_platform_t platform { { { "FAKE_A", 1 } }, { { "FAKE_A", 0x10 } } };
  video::testing::scoped_hardware_encoder_t hardware { 0x10 };
  scoped_budget_t budget { { { 0x10, { 3, ample_rate } } } };
  scoped_global_probe_t global { 0 };
  auto outcome = video::probe_encoders_with_outcome(exact_target("FAKE_A"));
  ASSERT_EQ(outcome.status, 0) << outcome.result.message;
  const auto ticket = ticket_from(outcome);
  const auto early = video::reserve_replacement(ticket, 1280, 720);
  ASSERT_FALSE(early);
  EXPECT_NE(early.error().find("not capturing"), std::string::npos) << early.error();
  EXPECT_EQ(budget.sessions(0x10), 1u);
}

TEST(VideoAdmissionTicket, AReplacementIsFencedOnTheEncoderGeneration) {
  replacement_fixture_t fixture { 2 };
  auto replacement = video::reserve_replacement(fixture.ticket, 1280, 720);
  ASSERT_TRUE(replacement) << replacement.error();
  std::expected<void, std::string> committed;
  {
    video::testing::scoped_hardware_encoder_t reselected { 0x10 };
    committed = video::commit_replacement(fixture.ticket, std::move(*replacement));
  }
  ASSERT_FALSE(committed);
  EXPECT_NE(committed.error().find("generation"), std::string::npos) << committed.error();
  EXPECT_EQ(fixture.budget.sessions(0x10), 1u) << "the refused replacement rolled back";
  EXPECT_EQ(fixture.held_width(), 1920u);
}

TEST(VideoAdmissionTicket, AnUnaccountedReplacementCannotReplaceAnAccountedMode) {
  replacement_fixture_t fixture { 2 };
  const auto committed = video::commit_replacement(fixture.ticket, video::admission::pending_t::unaccounted());
  ASSERT_FALSE(committed);
  EXPECT_EQ(fixture.budget.sessions(0x10), 1u);
  EXPECT_EQ(fixture.held_width(), 1920u);
}

TEST(VideoAdmissionTicket, WithoutABudgetAReplacementIsUnaccounted) {
  fake_capture_platform_t platform { { { "FAKE_A", 1 } } };
  scoped_global_probe_t global { 0 };
  auto outcome = video::probe_encoders_with_outcome(exact_target("FAKE_A"));
  ASSERT_EQ(outcome.status, 0) << outcome.result.message;
  const auto ticket = ticket_from(outcome);
  auto attached = video::testing::attach_capture(ticketed_config("FAKE_A", ticket));
  ASSERT_TRUE(attached);
  auto replacement = video::reserve_replacement(ticket, 1280, 720);
  ASSERT_TRUE(replacement) << replacement.error();
  EXPECT_FALSE(replacement->request());
  EXPECT_TRUE(video::commit_replacement(ticket, std::move(*replacement)));

  auto untracked = video::reserve_replacement(nullptr, 1280, 720);
  ASSERT_TRUE(untracked);
  EXPECT_TRUE(video::commit_replacement(nullptr, std::move(*untracked)));
}

TEST(VideoAdmissionTicket, UnderABudgetASessionWithoutATicketCannotChangeMode) {
  fake_capture_platform_t platform { { { "FAKE_A", 1 } }, { { "FAKE_A", 0x10 } } };
  scoped_budget_t budget { { { 0x10, { 3, ample_rate } } } };
  EXPECT_FALSE(video::reserve_replacement(nullptr, 1280, 720));
  EXPECT_EQ(budget.sessions(0x10), 0u);
}
TEST(VideoJoinerAdmission, AJoinerIsAdmittedOnTheOwnersSelectionBeforeItCaptures) {
  fake_capture_platform_t platform { { { "FAKE_A", 1 } }, { { "FAKE_A", 0x10 } } };
  video::testing::scoped_hardware_encoder_t hardware { 0x10 };
  scoped_budget_t budget { { { 0x10, { 2, ample_rate } } } };
  scoped_global_probe_t global { 0 };

  auto owner = video::probe_encoders_with_outcome(exact_target("FAKE_A"));
  ASSERT_EQ(owner.status, 0) << owner.result.message;
  const auto published = video::last_encoder_probe_result.message;
  const auto generation = video::testing::encoder_generation();

  // Nothing captures yet: the owner's stream has not attached.
  auto joiner = video::admit_existing(exact_target("FAKE_A", video::probe_mode_t { 1280, 720, 60 }));
  ASSERT_EQ(joiner.status, 0) << joiner.result.message;
  EXPECT_EQ(video::testing::encoder_generation(), generation) << "a joiner never reselects";
  EXPECT_EQ(joiner.encoder_generation, owner.encoder_generation);
  EXPECT_EQ(video::last_encoder_probe_result.message, published) << "a joiner never publishes";
  EXPECT_EQ(budget.sessions(0x10), 2u);

  auto owner_capture = video::testing::attach_capture(ticketed_config("FAKE_A", ticket_from(owner)));
  ASSERT_TRUE(owner_capture) << "the joiner did not stale the owner's ticket";
  auto joiner_capture = video::testing::attach_capture(ticketed_config("FAKE_A", ticket_from(joiner)));
  ASSERT_TRUE(joiner_capture);
  expect_only(*joiner_capture, 1);
  EXPECT_EQ(platform.opened("FAKE_A"), 1) << "both share the one source";
  EXPECT_EQ(budget.sessions(0x10), 2u);
  joiner_capture.reset();
  EXPECT_EQ(budget.sessions(0x10), 1u);
  owner_capture.reset();
  EXPECT_EQ(budget.sessions(0x10), 0u);
}

TEST(VideoJoinerAdmission, AJoinerOverTheBudgetIsRefused) {
  fake_capture_platform_t platform { { { "FAKE_A", 1 } }, { { "FAKE_A", 0x10 } } };
  video::testing::scoped_hardware_encoder_t hardware { 0x10 };
  scoped_budget_t budget { { { 0x10, { 1, ample_rate } } } };
  scoped_global_probe_t global { 0 };

  auto owner = video::probe_encoders_with_outcome(exact_target("FAKE_A"));
  ASSERT_EQ(owner.status, 0) << owner.result.message;
  const auto generation = video::testing::encoder_generation();
  auto joiner = video::admit_existing(exact_target("FAKE_A"));
  EXPECT_NE(joiner.status, 0);
  EXPECT_NE(joiner.result.message.find("budget"), std::string::npos) << joiner.result.message;
  EXPECT_FALSE(joiner.admission.commit());
  EXPECT_EQ(video::testing::encoder_generation(), generation);
  EXPECT_EQ(budget.sessions(0x10), 1u);
}

TEST(VideoJoinerAdmission, AJoinerWithoutAChosenEncoderIsRefused) {
  if (!video::testing::chosen_encoder_name().empty()) {
    GTEST_SKIP() << "An encoder was already chosen by an earlier test";
  }
  const auto generation = video::testing::encoder_generation();
  auto joiner = video::admit_existing(exact_target("FAKE_A"));
  EXPECT_NE(joiner.status, 0);
  EXPECT_EQ(joiner.result.error, video::probe_error_e::no_working_encoder);
  EXPECT_FALSE(joiner.admission.commit());
  EXPECT_EQ(video::testing::encoder_generation(), generation);
}