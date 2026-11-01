// Actual utilities and toggle policy, with fake display, driver, process and UI edges.
#define WIN32_LEAN_AND_MEAN
#include <functional>
#include <gtest/gtest.h>
#include <windows.h>

namespace fake_ui {
  int WINAPI message_box(HWND, LPCWSTR, LPCWSTR, UINT);
  UINT_PTR WINAPI set_timer(HWND, UINT_PTR, UINT, TIMERPROC);
  BOOL WINAPI kill_timer(HWND, UINT_PTR);
  BOOL WINAPI enum_thread_windows(DWORD, WNDENUMPROC, LPARAM);
  BOOL WINAPI enum_windows(WNDENUMPROC, LPARAM);
  int WINAPI
  class_name(HWND, LPWSTR, int);
  LRESULT WINAPI send_message(HWND, UINT, WPARAM, LPARAM);
  BOOL WINAPI
  enum_display_settings(LPCWSTR, DWORD, DEVMODEW *);
}  // namespace fake_ui
#define MessageBoxW fake_ui::message_box
#define SetTimer fake_ui::set_timer
#define KillTimer fake_ui::kill_timer
#define EnumThreadWindows fake_ui::enum_thread_windows
#define EnumWindows fake_ui::enum_windows
#define GetClassNameW fake_ui::class_name
#define SendMessageW fake_ui::send_message
#define EnumDisplaySettingsW fake_ui::enum_display_settings
#include "src/display_device/vdd_toggle.cpp"
#include "src/display_device/vdd_utils.cpp"
#undef MessageBoxW
#undef SetTimer
#undef KillTimer
#undef EnumThreadWindows
#undef EnumWindows
#undef GetClassNameW
#undef SendMessageW
#undef EnumDisplaySettingsW
#include "tests/unit/topology_test_worker.h"

namespace dd = display_device;
namespace vu = dd::vdd_utils;
namespace tp = dd::topology;
using cap = tp::mutation_capability_t;
using kind = tp::command_kind_e;

namespace {
  struct host_t {
    bool installed { true }, active {}, timer_failed {}, timeout {}, keep { true };
    dd::vdd_ioctl::result send_result { dd::vdd_ioctl::result::success };
    int probes {}, sends {}, dialogs {}, timers {}, cancelled_timers {}, extensions {}, cleanup {};
    TIMERPROC timer {};
    UINT interval {};
    DWORD enumerated_thread {};
    std::thread::id worker;
    std::vector<std::string> events;
    std::vector<std::uint64_t> cleanup_generations;
    std::vector<HWND> closed;
    std::function<void()> during_prompt;
    std::function<void()> during_extend;
  } host;
  char own_dialog, foreign_dialog;

  bool
  extend(std::uint64_t generation, const std::string &device, const std::vector<std::string> &physical) noexcept {
    EXPECT_NE(std::this_thread::get_id(), host.worker);
    EXPECT_EQ(generation, vu::vdd_generation());
    EXPECT_EQ(device, "vdd");
    EXPECT_EQ(physical, (std::vector<std::string> { "physical" }));
    ++host.extensions;
    host.events.emplace_back("extend");
    if (host.during_extend) host.during_extend();
    return true;
  }
  void
  cleanup(std::uint64_t generation) noexcept {
    EXPECT_NE(std::this_thread::get_id(), host.worker);
    ++host.cleanup;
    host.cleanup_generations.push_back(generation);
  }
  constexpr vu::toggle_callbacks_t callbacks { extend, cleanup };
}  // namespace

boost::log::sources::severity_logger<int> verbose, debug, info, warning, error, fatal;
const std::string ZAKO_NAME { "test-vdd" };
namespace config {
  video_t video {};
  nvhttp_t nvhttp {};
  std::string
  get_clients_config() { return "[]"; }
}  // namespace config
namespace file_handler {
  std::filesystem::path
  path_from_utf8(std::string_view value) { return std::filesystem::path { value }; }
  std::string
  path_to_utf8(const std::filesystem::path &value) { return value.string(); }
}  // namespace file_handler
namespace platf {
  std::filesystem::path
  appdata() {
    ++host.probes;
    throw std::runtime_error("No real appdata access in VDD fixtures");
  }
  boost::process::v1::child
  run_command(bool, bool, const std::string &, boost::filesystem::path &,
    const boost::process::v1::environment &, FILE *, std::error_code &, boost::process::v1::group *) {
    ++host.probes;
    throw std::runtime_error("No subprocess in VDD fixtures");
  }
  std::shared_ptr<display_device::topology::actor_process_t>
  make_owned_actor_process(actor_launch_spec_t) {
    ++host.probes;
    throw std::runtime_error("No subprocess in VDD fixtures");
  }
}  // namespace platf
namespace tray_state {
  void
  set_vdd_state(bool, bool, bool, bool) {}
}  // namespace tray_state
namespace system_tray {
  void
  update_vdd_menu() {}
}  // namespace system_tray
namespace system_tray_i18n {
  const std::string KEY_VDD_CONFIRM_CREATE_TITLE { "Create" };
  const std::string KEY_VDD_CONFIRM_CREATE_MSG { "Create?" };
  const std::string KEY_VDD_CANCEL_CREATE_LOG { "Cancelled" };
  const std::string KEY_VDD_CONFIRM_KEEP_TITLE { "Keep" };
  const std::string KEY_VDD_CONFIRM_KEEP_MSG { "Keep?" };
  std::string
  get_localized_string(const std::string &key) { return key; }
  std::wstring
  utf8_to_wstring(const std::string &value) { return { value.begin(), value.end() }; }
}  // namespace system_tray_i18n
namespace display_device {
  std::string
  find_device_by_friendlyname(const std::string &) {
    ++host.probes;
    return host.active ? "vdd" : "";
  }
  std::string
  get_display_friendly_name(const std::string &id) { return id == "vdd" ? ZAKO_NAME : "Physical"; }
  device_info_map_t
  enum_available_devices() {
    ++host.probes;
    if (host.active) return { { "vdd", { "vdd", ZAKO_NAME, device_state_e::primary, hdr_state_e::disabled } } };
    return { { "physical", { "physical", "Physical", device_state_e::primary, hdr_state_e::disabled } } };
  }
  active_topology_t
  get_current_topology() {
    ++host.probes;
    return { { host.active ? "vdd" : "physical" } };
  }
  bool
  is_topology_valid(const active_topology_t &) { return true; }
  bool
  set_topology(const cap &, const active_topology_t &) {
    ADD_FAILURE();
    return false;
  }
  hdr_state_map_t
  get_current_hdr_states(const std::unordered_set<std::string> &) {
    ++host.probes;
    return {};
  }
  bool
  set_hdr_states(const cap &, const hdr_state_map_t &) {
    ADD_FAILURE();
    return false;
  }
}  // namespace display_device
namespace display_device::vdd_ioctl {
  adapter_status_t
  query_adapter_status() {
    ++host.probes;
    return { host.installed, true, 0 };
  }
  bool
  ping() {
    ++host.probes;
    return true;
  }
  result
  send_command(const cap &, const command_t &command) {
    ++host.sends;
    if (host.send_result == result::success) {
      if (std::holds_alternative<create_monitor_t>(command)) host.active = true;
      if (std::holds_alternative<destroy_monitors_t>(command)) host.active = false;
    }
    return host.send_result;
  }
}  // namespace display_device::vdd_ioctl
namespace fake_ui {
  int WINAPI
  message_box(HWND, LPCWSTR, LPCWSTR, UINT flags) {
    EXPECT_NE(std::this_thread::get_id(), host.worker);
    ++host.dialogs;
    host.events.emplace_back("dialog");
    if ((flags & MB_YESNO) == MB_YESNO) {
      if (host.timeout && host.timer) host.timer(nullptr, WM_TIMER, 11, 0);
      return host.timeout || !host.keep ? IDNO : IDYES;
    }
    if (host.during_prompt) host.during_prompt();
    return IDOK;
  }
  UINT_PTR WINAPI
  set_timer(HWND window, UINT_PTR, UINT delay, TIMERPROC callback) {
    EXPECT_EQ(window, nullptr);
    ++host.timers;
    host.interval = delay;
    host.timer = callback;
    return host.timer_failed ? 0 : 11;
  }
  BOOL WINAPI
  kill_timer(HWND, UINT_PTR) {
    ++host.cancelled_timers;
    return TRUE;
  }
  BOOL WINAPI
  enum_thread_windows(DWORD thread, WNDENUMPROC callback, LPARAM data) {
    host.enumerated_thread = thread;
    callback(reinterpret_cast<HWND>(&own_dialog), data);
    return TRUE;
  }
  BOOL WINAPI
  enum_windows(WNDENUMPROC callback, LPARAM data) {
    callback(reinterpret_cast<HWND>(&foreign_dialog), data);
    return TRUE;
  }
  int WINAPI
  class_name(HWND, LPWSTR value, int size) {
    constexpr wchar_t name[] = L"#32770";
    EXPECT_GE(size, static_cast<int>(std::size(name)));
    std::copy(std::begin(name), std::end(name), value);
    return static_cast<int>(std::size(name) - 1);
  }
  LRESULT WINAPI
  send_message(HWND window, UINT message, WPARAM action, LPARAM) {
    EXPECT_EQ(message, WM_COMMAND);
    EXPECT_EQ(LOWORD(action), IDNO);
    host.closed.push_back(window);
    return 0;
  }
  BOOL WINAPI
  enum_display_settings(LPCWSTR, DWORD, DEVMODEW *) {
    ADD_FAILURE() << "Real display enumeration is forbidden";
    return FALSE;
  }
}  // namespace fake_ui

namespace {
  class VddUtilsPolicy: public testing::Test {
  protected:
    void
    SetUp() override {
      host = {};
      vu::head_generation = 0;
      make_ready();
    }
    static void
    make_ready() {
      vu::last_toggle_reservation = std::make_shared<const vu::toggle_debounce_t>(
        std::chrono::steady_clock::now() - std::chrono::seconds { 3 });
    }
    static vu::toggle_result_t
    create() {
      const auto request = vu::prepare_display_toggle();
      EXPECT_TRUE(request);
      vu::toggle_result_t result;
      topology_test::with_capability(kind::vdd_toggle, false, [&](const auto &authority) {
        host.worker = std::this_thread::get_id();
        result = vu::apply_display_toggle(authority, request.value_or(vu::toggle_request_t {}));
      });
      return result;
    }
  };

  TEST_F(VddUtilsPolicy, EveryAttemptInvalidatesOldGenerationsIncludingNoopsAndFailures) {
    const auto first = vu::vdd_generation();
    topology_test::with_capability(kind::launch_prepare, false, [&](const auto &authority) {
      host.installed = false;
      EXPECT_FALSE(vu::create_vdd_monitor(authority));
      EXPECT_EQ(vu::vdd_generation(), first + 1);
      host.installed = true;
      host.send_result = dd::vdd_ioctl::result::failed;
      EXPECT_FALSE(vu::create_vdd_monitor(authority));
      EXPECT_EQ(vu::vdd_generation(), first + 2);
      EXPECT_TRUE(vu::destroy_vdd_monitor(authority));
      EXPECT_EQ(vu::vdd_generation(), first + 3);
      vu::destroy_vdd_monitor_nolog(authority);
      EXPECT_EQ(vu::vdd_generation(), first + 4);
      host.send_result = dd::vdd_ioctl::result::success;
      EXPECT_TRUE(vu::create_vdd_monitor(authority));
      EXPECT_EQ(vu::vdd_generation(), first + 5);
      EXPECT_TRUE(vu::destroy_vdd_monitor(authority));
      EXPECT_EQ(vu::vdd_generation(), first + 6);
    });
  }

  TEST_F(VddUtilsPolicy, UtilityAuthorityPrecedesQueriesNoopsAndGenerationChanges) {
    const std::vector<std::function<void(const cap &)>> actions {
      [](const auto &c) { vu::create_vdd_monitor(c); },
      [](const auto &c) { vu::destroy_vdd_monitor(c); },
      [](const auto &c) { vu::destroy_vdd_monitor_nolog(c); },
      [](const auto &c) { vu::create_vdd_monitor_noninteractive(c); },
      [](const auto &c) { vu::apply_display_toggle(c, {}); },
      [](const auto &c) { vu::apply_display_toggle(c, { true }); },
      [](const auto &c) { vu::set_vdd_session_mode(c, {}, {}); },
      [](const auto &c) { vu::ensure_hardware_cursor_enabled_for_capture(c); },
      [](const auto &c) { vu::set_hardware_cursor_enabled(c, false); },
      [](const auto &c) { vu::persist_hardware_cursor_setting(c, false); },
      [](const auto &c) { vu::disable_enable_vdd(c); },
      [](const auto &c) { vu::ensure_vdd_extended_mode(c, {}); },
      [](const auto &c) { vu::apply_vdd_prep(c, {}, dd::parsed_config_t::vdd_prep_e::no_operation, {}); },
      [](const auto &c) { vu::set_hdr_state(c, false); },
    };
    const auto before = vu::vdd_generation();
    topology_test::with_capability(kind::rotate, false, [&](const auto &authority) {
      for (std::size_t i = 0; i < actions.size(); ++i) {
        SCOPED_TRACE(i);
        EXPECT_THROW(actions[i](authority), tp::capability_error_t);
      }
    });
    topology_test::with_capability(kind::launch_prepare, false, [&](const auto &authority) {
      std::async(std::launch::async, [&] {
        for (std::size_t i = 0; i < actions.size(); ++i) {
          SCOPED_TRACE(i);
          EXPECT_THROW(actions[i](authority), tp::capability_error_t);
        }
      }).get();
    });
    EXPECT_EQ(vu::vdd_generation(), before);
    EXPECT_EQ(host.probes + host.sends, 0);
  }

  TEST_F(VddUtilsPolicy, RejectedDebounceReservationDoesNotEraseANewerRequest) {
    auto first = vu::prepare_display_toggle();
    ASSERT_TRUE(first);
    EXPECT_FALSE(vu::prepare_display_toggle());
    vu::reject_display_toggle(*first);
    auto second = vu::prepare_display_toggle();
    ASSERT_TRUE(second);
    vu::reject_display_toggle(*first);
    EXPECT_FALSE(vu::prepare_display_toggle());
    vu::reject_display_toggle(*second);
    EXPECT_TRUE(vu::prepare_display_toggle());
    EXPECT_EQ(host.sends, 0);
    const auto same_time = std::make_shared<const vu::toggle_debounce_t>(first->reservation->when);
    vu::last_toggle_reservation = same_time;
    vu::reject_display_toggle(*first);
    EXPECT_EQ(vu::last_toggle_reservation.load(), same_time);
  }

  TEST_F(VddUtilsPolicy, PromptKeepsItsOriginalGenerationAndWorkerRejectsStaleIntent) {
    const auto before = vu::vdd_generation();
    host.during_prompt = [] {
      topology_test::with_capability(kind::vdd_destroy, false, [](const auto &authority) {
        EXPECT_TRUE(vu::destroy_vdd_monitor(authority));
      });
    };
    const auto request = vu::prepare_display_toggle();
    ASSERT_TRUE(request);
    EXPECT_EQ(request->generation, before);
    topology_test::with_capability(kind::vdd_toggle, false, [&](const auto &authority) {
      EXPECT_EQ(vu::apply_display_toggle(authority, *request).outcome, vu::toggle_outcome_e::stale);
    });
    EXPECT_EQ(host.sends, 0);
  }

  TEST_F(VddUtilsPolicy, CreationCapturesBaselineAndGenerationOnTheWorkerWithoutUi) {
    const auto before = vu::vdd_generation();
    const auto created = create();
    ASSERT_EQ(created.outcome, vu::toggle_outcome_e::created);
    EXPECT_EQ(created.generation, before + 1);
    EXPECT_EQ(created.physical_devices, (std::vector<std::string> { "physical" }));
    EXPECT_EQ(host.dialogs, 1);
    EXPECT_EQ(host.sends, 1);
    host.events.clear();
    vu::confirm_display_toggle(created, callbacks);
    EXPECT_EQ(host.events, (std::vector<std::string> { "extend", "dialog" }));
    EXPECT_EQ(host.cleanup, 0);
    EXPECT_EQ(host.sends, 1);
    EXPECT_EQ(host.timers, 1);
    EXPECT_EQ(host.interval, 20000u);
    EXPECT_EQ(host.cancelled_timers, 1);
  }

  TEST_F(VddUtilsPolicy, StaleConfirmationAndInterveningExtendCannotReachUiOrCleanup) {
    const auto created = create();
    ASSERT_EQ(created.outcome, vu::toggle_outcome_e::created);
    host.during_extend = [] {
      topology_test::with_capability(kind::shutdown_vdd_destroy, false, [](const auto &authority) {
        vu::destroy_vdd_monitor_nolog(authority);
      });
    };
    vu::confirm_display_toggle(created, callbacks);
    EXPECT_EQ(host.extensions, 1);
    EXPECT_EQ(host.dialogs, 1);
    EXPECT_EQ(host.cleanup, 0);
    host.active = true;
    const auto probes = host.probes;
    vu::confirm_display_toggle(created, callbacks);
    EXPECT_EQ(host.extensions, 1);
    EXPECT_EQ(host.dialogs, 1);
    EXPECT_EQ(host.cleanup, 0);
    EXPECT_EQ(host.probes, probes);
  }

  TEST_F(VddUtilsPolicy, TimeoutClosesOnlyThisThreadsDialogAndPostsTaggedCleanup) {
    const auto created = create();
    ASSERT_EQ(created.outcome, vu::toggle_outcome_e::created);
    host.timeout = true;
    vu::confirm_display_toggle(created, callbacks);
    EXPECT_EQ(host.enumerated_thread, GetCurrentThreadId());
    EXPECT_EQ(host.closed, (std::vector<HWND> { reinterpret_cast<HWND>(&own_dialog) }));
    EXPECT_EQ(host.cleanup_generations, (std::vector<std::uint64_t> { created.generation }));
    EXPECT_EQ(host.sends, 1);
    EXPECT_EQ(host.cancelled_timers, 1);
  }

  TEST_F(VddUtilsPolicy, TimerFailureDoesNotOpenAnUnboundedDialog) {
    const auto created = create();
    ASSERT_EQ(created.outcome, vu::toggle_outcome_e::created);
    host.timer_failed = true;
    vu::confirm_display_toggle(created, callbacks);
    EXPECT_EQ(host.dialogs, 1);
    EXPECT_EQ(host.cleanup_generations, (std::vector<std::uint64_t> { created.generation }));
    EXPECT_EQ(host.sends, 1);
  }

  TEST_F(VddUtilsPolicy, GenerationExhaustionFailsBeforeAnyPlatformAccess) {
    vu::head_generation = std::numeric_limits<std::uint64_t>::max();
    topology_test::with_capability(kind::launch_prepare, false, [](const auto &authority) {
      EXPECT_THROW(vu::create_vdd_monitor(authority), std::overflow_error);
      EXPECT_THROW(vu::destroy_vdd_monitor(authority), std::overflow_error);
      EXPECT_THROW(vu::destroy_vdd_monitor_nolog(authority), std::overflow_error);
    });
    EXPECT_EQ(vu::vdd_generation(), std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(host.probes + host.sends, 0);
  }

  TEST_F(VddUtilsPolicy, StateMismatchAndFailedCreationCannotBecomeSuccessfulConfirmation) {
    const auto request = vu::prepare_display_toggle();
    ASSERT_TRUE(request);
    topology_test::with_capability(kind::vdd_toggle, false, [&](const auto &authority) {
      host.active = true;
      EXPECT_EQ(vu::apply_display_toggle(authority, *request).outcome, vu::toggle_outcome_e::stale);
      EXPECT_EQ(host.sends, 0);
      host.active = false;
      host.installed = false;
      const auto failed = vu::apply_display_toggle(authority, *request);
      EXPECT_EQ(failed.outcome, vu::toggle_outcome_e::failed);
      EXPECT_EQ(failed.generation, request->generation + 1);
      EXPECT_THROW(vu::confirm_display_toggle(failed, callbacks), std::invalid_argument);
      host.active = true;
      auto close_request = *request;
      close_request.create = false;
      close_request.generation = vu::vdd_generation();
      const auto closed = vu::apply_display_toggle(authority, close_request);
      EXPECT_EQ(closed.outcome, vu::toggle_outcome_e::destroyed);
      EXPECT_THROW(vu::confirm_display_toggle(closed, callbacks), std::invalid_argument);
    });
    EXPECT_EQ(host.dialogs, 1);
    EXPECT_EQ(host.sends, 1);
    EXPECT_EQ(host.extensions + host.cleanup, 0);
  }
}  // namespace

TEST(VddAdapterRestart, CreationIsRetriedWithTheLegacyBound) {
  const auto retry = vu::adapter_restart_retry();
  EXPECT_EQ(retry.attempts, static_cast<unsigned>(vu::kMaxRetryCount));
  EXPECT_EQ(retry.attempts, 3u);
  EXPECT_TRUE(retry.wait);
}
