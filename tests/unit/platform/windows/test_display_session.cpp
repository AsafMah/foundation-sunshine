// Actual session policy and retry timer. Settings persistence, VDD and OS edges are fake.
#include "src/display_device/session.cpp"

#include <atomic>
#include <future>
#include <random>

#include <gtest/gtest.h>

#include "src/display_device/topology_actor_record.h"

namespace dd = display_device;
namespace topology = dd::topology;
using cap = topology::mutation_capability_t;
using policy = dd::automatic_vdd_policy_e;
using kind = topology::command_kind_e;
using op = topology::operation_e;

namespace {
  struct host_t {
    topology::coordinator_t *coordinator {};
    dd::session_t *session {};
    bool disk_pending { true };
    bool cached {};
    bool locked {};
    bool busy {};
    bool vdd {};
    int restores {}, applies {}, creates {}, destroys {}, reads {};
    std::function<void()> unlock;
    std::vector<kind> restore_kinds;
    const cap *expected {};
    std::function<topology::adapter_restart_plan_t()> restart_plan;
    int restart_plans {};
  } host;
  std::atomic_int ccd_checks;

  void
  authority(const cap &capability, op operation) {
    capability.require(operation);
    if (host.expected) EXPECT_EQ(&capability, host.expected);
  }
}  // namespace

boost::log::sources::severity_logger<int> verbose, debug, info, warning, error, fatal;
const std::string ZAKO_NAME { "test-vdd" };
bool is_running_as_system_user {};

namespace config {
  video_t video {};
}
namespace hdr {
  std::string_view
  to_string(target_source_e) noexcept { return "test"; }
}  // namespace hdr
namespace rtsp_stream {
  bool
  session_starting_or_active() { return host.busy; }
}  // namespace rtsp_stream
namespace platf {
  std::filesystem::path
  appdata() {
    ADD_FAILURE();
    return {};
  }
}  // namespace platf
namespace platf::vulkan_hdr_bridge {
  void
  startup_cleanup() { ADD_FAILURE(); }
  void
  shutdown_cleanup() { ADD_FAILURE(); }
  void
  disable() {}
  bool
  enable_for_vdd_hdr_session() {
    ADD_FAILURE();
    return false;
  }
}  // namespace platf::vulkan_hdr_bridge
namespace display_device::coordinated {
  topology::coordinator_t &
  coordinator() { return *host.coordinator; }
  automatic_vdd_policy_e
  automatic_vdd_policy() { return automatic_vdd_policy_e::suppress; }
  void
  start() { ADD_FAILURE(); }
  void
  shutdown_vdd_destroy() { ADD_FAILURE(); }
}  // namespace display_device::coordinated
namespace display_device {
  struct session_test_access_t {
    static std::unique_ptr<session_t>
    make() { return std::unique_ptr<session_t> { new session_t }; }
    static void
    fail_vdd(session_t &session, const cap &capability, bool adapter_restart) {
      std::lock_guard lock { session.mutex };
      session.restore_after_failed_vdd(capability, automatic_vdd_policy_e::suppress,
        adapter_restart ? session_t::vdd_stage_result_e::adapter_restart_required : session_t::vdd_stage_result_e::create_failed);
    }
    /** One retry tick, as the 5 s timer would deliver it. */
    static void
    tick(session_t &session) { session.timer->request_now(session.timer->current_request()); }
  };
  struct settings_t::persistent_data_t {};
  struct settings_t::audio_data_t {};
  settings_t::settings_t() = default;
  settings_t::~settings_t() = default;
  bool
  settings_t::is_changing_settings_going_to_fail() const {
    ++ccd_checks;
    return host.locked;
  }
  bool
  settings_t::has_persistent_data() const { return host.cached; }
  void
  settings_t::load_persistence(const cap &capability) {
    authority(capability, op::restore_state);
    host.cached = host.cached || host.disk_pending;
  }
  void
  settings_t::capture_audio_sink() {}
  void
  settings_t::release_audio_sink() {}
  void
  settings_t::remove_vdd_from_initial_topology(const std::string &) { ADD_FAILURE(); }
  void
  settings_t::replace_vdd_id(const std::string &, const std::string &) { ADD_FAILURE(); }
  settings_t::apply_result_t
  settings_t::apply_config(const cap &capability, const parsed_config_t &,
    const rtsp_stream::launch_session_t &, automatic_vdd_policy_e, const boost::optional<active_topology_t> &) {
    authority(capability, op::configure_display);
    ++host.applies;
    host.cached = host.disk_pending = true;
    return { apply_result_t::result_e::success };
  }
  bool
  settings_t::revert_settings(const cap &capability, revert_reason_e, automatic_vdd_policy_e vdd_policy) {
    authority(capability, op::restore_state);
    EXPECT_EQ(vdd_policy, policy::suppress);
    ++host.restores;
    host.restore_kinds.push_back(capability.kind());
    if (host.locked) return false;
    host.disk_pending = host.cached = false;
    return true;
  }
  void
  settings_t::reset_persistence(const cap &capability, automatic_vdd_policy_e) {
    authority(capability, op::reset_persistence);
    host.disk_pending = host.cached = false;
  }
  parsed_config_t::vdd_prep_e
  parsed_config_t::to_vdd_prep(device_prep_e) { return vdd_prep_e::no_operation; }
  parsed_config_t::device_prep_e
  parsed_config_t::to_physical_device_prep(device_prep_e value) { return value; }
  boost::optional<parsed_config_t>
  make_parsed_config(const config::video_t &, const rtsp_stream::launch_session_t &, bool) {
    ++host.reads;
    parsed_config_t result {};
    result.device_id = "physical";
    result.use_vdd = false;
    return result;
  }
  bool
  display_prepared_for_hdr(const config::video_t &, const rtsp_stream::launch_session_t &) { return false; }
  device_info_map_t
  enum_available_devices() {
    ++host.reads;
    device_info_map_t devices { { "physical", { "physical", "Physical", device_state_e::primary, hdr_state_e::disabled } } };
    if (host.vdd) devices.emplace("vdd", device_info_t { "vdd", ZAKO_NAME, device_state_e::active, hdr_state_e::disabled });
    return devices;
  }
  std::string
  find_device_by_friendlyname(const std::string &) {
    ++host.reads;
    return host.vdd ? "vdd" : "";
  }
  active_topology_t
  get_current_topology() {
    ++host.reads;
    return { { "physical" } };
  }

  bool
  SessionEventListener::init() {
    ADD_FAILURE();
    return false;
  }
  void
  SessionEventListener::deinit() { host.unlock = {}; }
  void
  SessionEventListener::add_unlock_task(UnlockCallback callback) { host.unlock = std::move(callback); }
  void
  SessionEventListener::clear_unlock_task() { host.unlock = {}; }
}  // namespace display_device
namespace display_device::w_utils {
  bool
  is_any_rdp_session_active() {
    ++host.reads;
    return false;
  }
}  // namespace display_device::w_utils
namespace display_device::vdd_ioctl {
  bool
  ping() {
    ADD_FAILURE();
    return false;
  }
}  // namespace display_device::vdd_ioctl
namespace display_device::vdd_utils {
  bool
  is_display_on() { return host.vdd; }
  bool
  set_hdr_state(const cap &capability, bool) {
    authority(capability, op::write_hdr);
    ADD_FAILURE();
    return false;
  }
  bool
  apply_vdd_prep(const cap &capability, const std::string &, parsed_config_t::vdd_prep_e, const boost::optional<device_info_map_t> &) {
    authority(capability, op::write_topology);
    ADD_FAILURE();
    return false;
  }
  vdd_status_t
  get_vdd_status() {
    ADD_FAILURE();
    return {};
  }
  bool
  create_vdd_monitor(const cap &capability, const std::string &, const hdr_brightness_t &, const physical_size_t &) {
    authority(capability, op::vdd_create);
    ++host.creates;
    host.vdd = true;
    return true;
  }
  void
  disable_enable_vdd(const cap &capability) {
    authority(capability, op::adapter_restart);
    ADD_FAILURE();
  }
  std::filesystem::path
  adapter_restart_record_path() {
    ADD_FAILURE();
    return {};
  }
  topology::adapter_restart_plan_t
  make_adapter_restart_plan() {
    ++host.restart_plans;
    return host.restart_plan ? host.restart_plan() : topology::adapter_restart_plan_t {};
  }
  bool
  is_mode_advertised(const std::string &, const display_mode_t &) {
    ADD_FAILURE();
    return false;
  }
  bool
  destroy_vdd_monitor(const cap &capability) {
    authority(capability, op::vdd_destroy);
    ++host.destroys;
    host.vdd = false;
    return true;
  }
  VddSettings
  prepare_vdd_settings(const parsed_config_t &) {
    ADD_FAILURE();
    return {};
  }
  set_vdd_result
  set_vdd_session_mode(const cap &capability, const parsed_config_t &, const VddSettings &) {
    authority(capability, op::vdd_set_modes);
    ADD_FAILURE();
    return set_vdd_result::failed;
  }
  physical_size_t
  get_client_physical_size(const std::string &) { return {}; }
  bool
  wait_for_mode_publication(const std::string &, const display_mode_t &) {
    ADD_FAILURE();
    return false;
  }
  bool
  create_vdd_monitor_noninteractive(const cap &capability) {
    authority(capability, op::vdd_create);
    ++host.creates;
    return true;
  }
  bool
  ensure_hardware_cursor_enabled_for_capture(const cap &capability, bool *) {
    authority(capability, op::vdd_cursor);
    ADD_FAILURE();
    return false;
  }
}  // namespace display_device::vdd_utils

namespace {
  class DisplaySessionPolicy: public ::testing::Test {
  protected:
    void
    initialize(bool managed = false) {
      host = {};
      config::video = {};
      topology::coordinator_options_t options;
      options.managed_mode = managed;
      options.native_restore = [](const auto &, const auto &) {};
      options.probes.active_streams = [] { return 0u; };
      options.probes.startup_restore_quiescent = [] { return !host.busy; };
      options.probes.legacy_restore_pending = [] { return host.session && host.session->restore_pending(); };
      coordinator = std::make_unique<topology::coordinator_t>(std::move(options));
      host.coordinator = coordinator.get();
      session = dd::session_test_access_t::make();
      host.session = session.get();
    }

    void
    recover() {
      const auto result = coordinator->start([&](const auto &capability, const auto &) {
                                       host.expected = &capability;
                                       session->restore_state(capability, policy::suppress);
                                       host.expected = nullptr;
                                     })
                            .get();
      ASSERT_TRUE(result.completed());
    }

    void
    TearDown() override {
      host.unlock = {};
      if (coordinator) coordinator->stop();
      session.reset();
      coordinator.reset();
    }
    std::unique_ptr<topology::coordinator_t> coordinator;
    std::unique_ptr<dd::session_t> session;
  };

  TEST_F(DisplaySessionPolicy, ColdStartupLoadsPersistenceWithoutACurrentSession) {
    initialize();
    EXPECT_FALSE(host.cached);
    ASSERT_TRUE(host.disk_pending);
    recover();
    EXPECT_EQ(host.restores, 1);
    EXPECT_FALSE(host.disk_pending);
    EXPECT_EQ(host.creates + host.destroys, 0);
  }

  TEST_F(DisplaySessionPolicy, ManagedStartupRegistersOnlyRestrictedQuiescentRestoration) {
    initialize(true);
    host.locked = true;
    recover();
    EXPECT_TRUE(host.cached) << "locked-desktop launch must inherit the persisted baseline";
    ASSERT_TRUE(coordinator->status().startup_restore_pending);
    EXPECT_TRUE(session->restore_pending());
    host.locked = false;
    host.busy = true;
    EXPECT_FALSE(coordinator->retry_startup_restore().completion.get().completed());
    EXPECT_EQ(host.restores, 0);
    host.busy = false;
    EXPECT_TRUE(coordinator->retry_startup_restore().completion.get().completed());
    EXPECT_FALSE(coordinator->status().startup_restore_pending);
    EXPECT_FALSE(session->restore_pending());
    EXPECT_EQ(host.restore_kinds, (std::vector<kind> { kind::legacy_startup_restore }));
    EXPECT_EQ(host.creates + host.destroys, 0);
  }

  TEST_F(DisplaySessionPolicy, ManagedResetForgetsStartupWithoutRestoringDisplays) {
    initialize(true);
    host.locked = true;
    recover();
    ASSERT_TRUE(coordinator->status().startup_restore_pending);
    const auto result = coordinator->run(kind::reset_persistence, [&](const auto &capability, const auto &) {
      session->reset_persistence(capability, policy::suppress);
    });
    ASSERT_TRUE(result.completed());
    EXPECT_FALSE(coordinator->status().startup_restore_pending);
    EXPECT_FALSE(session->restore_pending());
    EXPECT_EQ(host.restores + host.creates + host.destroys, 0);
    EXPECT_FALSE(host.disk_pending);
  }

  TEST_F(DisplaySessionPolicy, UnlockSchedulesRegisteredRestoreInsteadOfBorrowingStartupAuthority) {
    initialize(true);
    host.locked = true;
    recover();
    ASSERT_TRUE(coordinator->status().startup_restore_pending);
    ASSERT_TRUE(host.unlock);
    auto notify = host.unlock;
    host.locked = false;
    notify();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds { 2 };
    while (coordinator->status().startup_restore_pending && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::yield();
    }
    ASSERT_FALSE(coordinator->status().startup_restore_pending);
    EXPECT_EQ(host.restore_kinds, (std::vector<kind> { kind::legacy_startup_restore }));
    EXPECT_FALSE(session->restore_pending());
    EXPECT_EQ(host.creates + host.destroys, 0);
  }

  // Base 204a968: a restore that cannot run waits for unlock before polling starts,
  // so a long lock never spends the 20-retry budget.
  TEST_F(DisplaySessionPolicy, LockedDeferredRestoreSurvivesALongLockAndRunsAfterUnlock) {
    initialize();
    recover();
    ASSERT_EQ(host.restores, 1);
    host.cached = true;
    host.locked = true;
    const auto sync = [&] {
      ASSERT_TRUE(coordinator->run(kind::rotate, [](const auto &, const auto &) {}).completed());
    };
    ASSERT_TRUE(coordinator->run(kind::stream_restore, [&](const auto &capability, const auto &) {
                             session->restore_state(capability, policy::suppress);
                           })
                  .completed());
    ASSERT_TRUE(host.unlock);
    ASSERT_TRUE(session->restore_pending());
    const int checks_at_defer = ccd_checks.load();
    for (int tick = 0; tick < 25; ++tick) {
      // request_now only expedites an armed retry; repeat until this tick's attempt is posted.
      const auto before = coordinator->status().submitted_commands;
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds { 2 };
      while (coordinator->status().submitted_commands == before && std::chrono::steady_clock::now() < deadline) {
        dd::session_test_access_t::tick(*session);
        std::this_thread::sleep_for(std::chrono::milliseconds { 1 });
      }
      ASSERT_GT(coordinator->status().submitted_commands, before) << "retry disarmed at tick " << tick << " while still locked";
    }
    sync();
    EXPECT_EQ(ccd_checks.load(), checks_at_defer) << "base 204a968 queries no display API until unlock";
    EXPECT_TRUE(session->restore_pending()) << "the lock spent the retry budget";
    EXPECT_EQ(host.restores, 1) << "nothing may be restored while locked";
    host.locked = false;
    auto notify = host.unlock;
    notify();
    const auto unlocked = std::chrono::steady_clock::now() + std::chrono::seconds { 2 };
    while (session->restore_pending() && std::chrono::steady_clock::now() < unlocked) {
      std::this_thread::sleep_for(std::chrono::milliseconds { 1 });
    }
    sync();
    EXPECT_EQ(host.restores, 2);
    EXPECT_FALSE(host.cached);
    EXPECT_FALSE(session->restore_pending());
  }

  TEST_F(DisplaySessionPolicy, SessionRejectsWrongAuthorityBeforeAnyStateOrPlatformAccess) {
    initialize();
    ASSERT_TRUE(coordinator->start({}).get().completed());
    rtsp_stream::launch_session_t launch {};
    const auto result = coordinator->run(kind::rotate, [&](const auto &capability, const auto &) {
      EXPECT_THROW((void) session->configure_display(capability, config::video, launch, policy::allow), topology::capability_error_t);
      EXPECT_THROW(session->restore_state(capability, policy::allow), topology::capability_error_t);
      EXPECT_THROW(session->reset_persistence(capability, policy::allow), topology::capability_error_t);
      EXPECT_THROW((void) session->create_vdd_monitor(capability), topology::capability_error_t);
      EXPECT_THROW((void) session->create_vdd_monitor_noninteractive(capability), topology::capability_error_t);
      EXPECT_THROW((void) session->destroy_vdd_monitor(capability), topology::capability_error_t);
    });
    EXPECT_TRUE(result.completed());
    EXPECT_EQ(host.reads + host.applies + host.restores + host.creates + host.destroys, 0);
    EXPECT_TRUE(host.disk_pending);
  }

  TEST_F(DisplaySessionPolicy, AutomaticSuppressionDoesNotBlockExplicitDestroy) {
    initialize();
    host.vdd = true;
    recover();
    EXPECT_TRUE(host.vdd);
    EXPECT_EQ(host.restores, 1);
    EXPECT_EQ(host.creates + host.destroys, 0);
    const auto result = coordinator->run(kind::vdd_destroy, [&](const auto &capability, const auto &) {
      host.expected = &capability;
      EXPECT_TRUE(session->destroy_vdd_monitor(capability));
      host.expected = nullptr;
    });
    EXPECT_TRUE(result.completed());
    EXPECT_EQ(host.destroys, 1);
    EXPECT_FALSE(host.vdd);
  }

  /** A restart that stays running until released. */
  struct restart_actor_t {
    std::promise<void> exit;
    std::shared_future<void> exited { exit.get_future().share() };
    std::atomic_int launched {};

    void
    release() {
      try {
        exit.set_value();
      }
      catch (const std::future_error &) {
      }
    }
  };

  class restart_process_t final: public topology::actor_process_t {
  public:
    explicit restart_process_t(std::shared_ptr<restart_actor_t> actor):
        actor_ { std::move(actor) } {}
    void
    launch(std::error_code &) override {
      ++actor_->launched;
      started_ = true;
    }
    bool
    started() const override { return started_; }
    std::optional<topology::actor_receipt_t>
    receipt() override { return topology::actor_receipt_t { 7310, 133'300'000'000'000'000ULL }; }
    topology::actor_wait_t
    wait() override {
      actor_->exited.wait();
      return { topology::actor_wait_e::exited, 0u, "restart ended" };
    }

  private:
    std::shared_ptr<restart_actor_t> actor_;
    bool started_ {};
  };

  class AdapterRestartSession: public DisplaySessionPolicy {
  protected:
    void
    SetUp() override {
      std::random_device random;
      dir = std::filesystem::temp_directory_path() / ("sunshine-session-restart-" + std::to_string(random()) + "-" + std::to_string(random()));
      std::filesystem::create_directories(dir);
      initialize();
      recover();
      host.restart_plan = [this] {
        topology::adapter_restart_plan_t plan;
        plan.store = std::make_shared<topology::file_actor_record_store_t>(dir / "actor.json");
        plan.operation = 4242;
        plan.process = [actor = actor] { return std::make_shared<restart_process_t>(actor); };
        return plan;
      };
      // A configuration was applied before preparing the VDD failed.
      host.disk_pending = true;
      host.restore_kinds.clear();
      host.restores = 0;
    }

    void
    TearDown() override {
      actor->release();
      DisplaySessionPolicy::TearDown();
      std::error_code ignored;
      std::filesystem::remove_all(dir, ignored);
    }

    std::filesystem::path dir;
    std::shared_ptr<restart_actor_t> actor { std::make_shared<restart_actor_t>() };
  };

  TEST_F(AdapterRestartSession, RestartDefersTheRestorationUntilItSettlesAndAheadOfQueuedWork) {
    topology::posted_command_t prequeued;
    std::atomic_int restores_before_prequeued { -1 };
    const auto result = coordinator->run(kind::reconfigure, [&](const auto &capability, const auto &) {
      dd::session_test_access_t::fail_vdd(*session, capability, true);
      EXPECT_EQ(host.restores, 0) << "the restoration must not run while the restart may still be running";
      prequeued = coordinator->post_tracked(kind::shutdown_restore, [&](const auto &, const auto &) {
        restores_before_prequeued = host.restores;
      });
    });
    ASSERT_TRUE(result.completed());
    EXPECT_EQ(host.restart_plans, 1);
    EXPECT_EQ(prequeued.completion.wait_for(std::chrono::milliseconds { 100 }), std::future_status::timeout);
    EXPECT_EQ(host.restores, 0);
    EXPECT_EQ(actor->launched, 1);

    actor->release();
    ASSERT_EQ(prequeued.completion.wait_for(std::chrono::seconds { 5 }), std::future_status::ready);
    EXPECT_TRUE(prequeued.completion.get().completed());
    EXPECT_EQ(restores_before_prequeued, 1);
    EXPECT_EQ(host.restore_kinds, (std::vector<kind> { kind::launch_failed_restore }));
    EXPECT_FALSE(host.disk_pending);
    EXPECT_EQ(actor->launched, 1) << "an adapter restart is never retried";
  }

  TEST_F(AdapterRestartSession, OtherVddFailuresStillRestoreInlineWithoutARestart) {
    const auto result = coordinator->run(kind::reconfigure, [&](const auto &capability, const auto &) {
      dd::session_test_access_t::fail_vdd(*session, capability, false);
      EXPECT_EQ(host.restores, 1);
    });
    ASSERT_TRUE(result.completed());
    EXPECT_EQ(host.restore_kinds, (std::vector<kind> { kind::reconfigure }));
    EXPECT_EQ(host.restart_plans + actor->launched, 0);
  }

  TEST_F(AdapterRestartSession, AnUnplannableRestartRestoresInlineAndLaunchesNothing) {
    host.restart_plan = {};
    const auto result = coordinator->run(kind::reconfigure, [&](const auto &capability, const auto &) {
      dd::session_test_access_t::fail_vdd(*session, capability, true);
      EXPECT_EQ(host.restores, 1);
    });
    ASSERT_TRUE(result.completed());
    EXPECT_EQ(host.restore_kinds, (std::vector<kind> { kind::reconfigure }));
    EXPECT_EQ(host.restart_plans, 1);
    EXPECT_EQ(actor->launched, 0);
  }

  TEST_F(AdapterRestartSession, AFailureToPlanTheRestartRestoresInlineInsteadOfEscaping) {
    host.restart_plan = []() -> topology::adapter_restart_plan_t { throw std::runtime_error("no appdata"); };
    const auto result = coordinator->run(kind::reconfigure, [&](const auto &capability, const auto &) {
      EXPECT_NO_THROW(dd::session_test_access_t::fail_vdd(*session, capability, true));
      EXPECT_EQ(host.restores, 1);
    });
    ASSERT_TRUE(result.completed());
    EXPECT_EQ(host.restore_kinds, (std::vector<kind> { kind::reconfigure }));
    EXPECT_EQ(actor->launched, 0);
    auto next = coordinator->post_tracked(kind::reconfigure, [](const auto &, const auto &) {});
    ASSERT_EQ(next.completion.wait_for(std::chrono::seconds { 2 }), std::future_status::ready) << "no obligation may be left behind";
    EXPECT_TRUE(next.completion.get().completed());
  }
}  // namespace
