/**
 * Actual command entrypoint with a link-time fake at the raw transport boundary.
 * No SetupAPI, display, driver or IOCTL implementation is linked into this target.
 */
#include "src/display_device/vdd_ioctl.h"
#include "src/logging_severity.h"

#include <gtest/gtest.h>
#include <limits>
#include <locale>

namespace vdd = display_device::vdd_ioctl;
namespace modes = display_device::vdd_mode_list;

namespace {
  std::vector<std::wstring> sent;
  vdd::result transport_result;
  const std::string guid = "{01234567-89ab-cdef-0123-456789abcdef}";

  template <class T>
  concept accepted_command = requires(const T &request) {
    vdd::send_command(request);
  };

  class VddCommands: public ::testing::Test {
  protected:
    void
    SetUp() override {
      sent.clear();
      transport_result = vdd::result::success;
    }

    void
    rejects(const vdd::command_t &request) {
      sent.clear();
      EXPECT_EQ(vdd::send_command(request), vdd::result::failed);
      EXPECT_TRUE(sent.empty()) << "invalid request reached the driver transport";
    }
  };
}  // namespace

boost::log::sources::severity_logger<int> warning;

namespace display_device::vdd_ioctl::detail {
  result
  send_command_buffer(const std::wstring &buffer) {
    sent.push_back(buffer);
    return transport_result;
  }
}  // namespace display_device::vdd_ioctl::detail

namespace {
  TEST_F(VddCommands, SupportedCommandsKeepExistingWireBytesAndResults) {
    for (const auto outcome : { vdd::result::success, vdd::result::failed, vdd::result::interface_missing }) {
      transport_result = outcome;
      sent.clear();
      EXPECT_EQ(vdd::send_command(vdd::create_monitor_t {}), outcome);
      EXPECT_EQ(vdd::send_command(vdd::destroy_monitors_t {}), outcome);
      EXPECT_EQ(vdd::send_command(vdd::hardware_cursor_t { true }), outcome);
      EXPECT_EQ(vdd::send_command(vdd::hardware_cursor_t { false }), outcome);
      EXPECT_EQ(sent, (std::vector<std::wstring> {
                        L"CREATEMONITOR", L"DESTROYMONITOR", L"HARDWARECURSOR true", L"HARDWARECURSOR false" }));
    }
  }

  TEST_F(VddCommands, CreationPreservesGuidHdrAndOptionalPhysicalSize) {
    vdd::create_monitor_t request { guid, 750.25f, 0.0025f, 350.5f, 34.5f, 19.4f };
    EXPECT_EQ(vdd::send_command(request), vdd::result::success);
    ASSERT_EQ(sent.size(), 1);
    EXPECT_EQ(sent.back(), L"CREATEMONITOR {01234567-89ab-cdef-0123-456789abcdef}:[750.25,0.0025,350.5][34.5,19.4]");
    for (const auto dimension : { &vdd::create_monitor_t::width_cm, &vdd::create_monitor_t::height_cm }) {
      for (const float unspecified : { 0.0f, -1.0f }) {
        auto no_size = request;
        no_size.*dimension = unspecified;
        EXPECT_EQ(vdd::send_command(no_size), vdd::result::success);
        EXPECT_EQ(sent.back(), L"CREATEMONITOR {01234567-89ab-cdef-0123-456789abcdef}:[750.25,0.0025,350.5]");
      }
    }
    request.guid = "{01234567-89AB-CDEF-0123-456789ABCDEF}";
    EXPECT_EQ(vdd::send_command(request), vdd::result::success);
    EXPECT_EQ(sent.back(), L"CREATEMONITOR {01234567-89AB-CDEF-0123-456789ABCDEF}:[750.25,0.0025,350.5][34.5,19.4]");
    request.guid.clear();
    request.max_nits = std::numeric_limits<float>::quiet_NaN();
    EXPECT_EQ(vdd::send_command(request), vdd::result::success);
    EXPECT_EQ(sent.back(), L"CREATEMONITOR");
  }

  TEST_F(VddCommands, InvalidCreatePayloadsCannotReachTransport) {
    for (const auto &invalid : { "RELOAD_DRIVER", "{01234567-89ab-cdef-0123-456789abcdeg}",
           "{01234567_89ab-cdef-0123-456789abcdef}", "01234567-89ab-cdef-0123-456789abcdef",
           "{01234567-89ab-cdef-0123-456789abcdef} DESTROYMONITOR" }) {
      SCOPED_TRACE(invalid);
      rejects(vdd::create_monitor_t { invalid });
    }
    auto embedded_nul = guid;
    embedded_nul[5] = '\0';
    rejects(vdd::create_monitor_t { embedded_nul });
    for (const auto field : { &vdd::create_monitor_t::max_nits, &vdd::create_monitor_t::min_nits,
           &vdd::create_monitor_t::max_full_nits, &vdd::create_monitor_t::width_cm, &vdd::create_monitor_t::height_cm }) {
      for (const float invalid : { std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN() }) {
        vdd::create_monitor_t request { guid };
        request.*field = invalid;
        rejects(request);
      }
    }
  }

  TEST_F(VddCommands, ModePayloadsReuseBoundedExistingSerialization) {
    const std::vector<modes::vdd_mode_t> ordered { { { 1280, 720 }, 120 }, { { 1920, 1080 }, 60 } };
    ASSERT_EQ(vdd::send_command(vdd::set_modes_t { ordered }), vdd::result::success);
    ASSERT_EQ(sent.size(), 1);
    EXPECT_EQ(sent.back(), L"SETMODES 1280x720x120,1920x1080x60");
    EXPECT_EQ(sent.back(), modes::format_setmodes_command(ordered));
    const auto full = std::vector<modes::vdd_mode_t>(modes::kMaxTotalModes, { { 1280, 720 }, 60 });
    ASSERT_EQ(vdd::send_command(vdd::set_modes_t { full }), vdd::result::success);
    EXPECT_EQ(sent.back(), modes::format_setmodes_command(full));

    std::vector<modes::vdd_mode_t> limit(61, { { 1000000000, 1000000000 }, 1000000000 });
    limit.push_back({ { 1000000000, 1000000 }, 100000 });
    ASSERT_EQ(modes::format_setmodes_command(limit).size(), modes::kMaxCommandChars);
    ASSERT_EQ(vdd::send_command(vdd::set_modes_t { limit }), vdd::result::success);
    EXPECT_EQ(sent.back().size(), modes::kMaxCommandChars);
    limit.back().refresh_hz = 1000000;
    ASSERT_EQ(modes::format_setmodes_command(limit).size(), modes::kMaxCommandChars + 1);
    rejects(vdd::set_modes_t { limit });
  }

  TEST_F(VddCommands, InvalidModesCannotReachTransport) {
    rejects(vdd::set_modes_t {});
    for (const auto invalid : { modes::vdd_mode_t { { 0, 720 }, 60 }, modes::vdd_mode_t { { 1280, 0 }, 60 },
           modes::vdd_mode_t { { 1280, 720 }, 0 } }) {
      rejects(vdd::set_modes_t { { invalid } });
    }
    rejects(vdd::set_modes_t { std::vector<modes::vdd_mode_t>(modes::kMaxTotalModes + 1, { { 1280, 720 }, 60 }) });
    rejects(vdd::set_modes_t { std::vector<modes::vdd_mode_t>(modes::kMaxTotalModes, { { 1000000000, 1000000000 }, 1000000000 }) });
  }

  TEST_F(VddCommands, EmptyAndUntypedCommandsAreNotAccepted) {
    rejects(vdd::command_t {});
    EXPECT_FALSE(accepted_command<std::wstring>);
    EXPECT_FALSE(accepted_command<std::string>);
    EXPECT_FALSE(accepted_command<const wchar_t *>);
  }

  struct comma_decimal_t: std::numpunct<wchar_t> {
    wchar_t
    do_decimal_point() const override { return L','; }
  };

  TEST_F(VddCommands, PayloadFormattingDoesNotUseTheProcessLocale) {
    struct locale_guard_t {
      std::locale original = std::locale();
      ~locale_guard_t() { std::locale::global(original); }
    } restore;
    std::locale::global(std::locale(std::locale::classic(), new comma_decimal_t));
    ASSERT_EQ(vdd::send_command(vdd::create_monitor_t { guid, 750.25f, 0.0025f, 350.5f }), vdd::result::success);
    ASSERT_EQ(sent.size(), 1);
    EXPECT_EQ(sent.back(), L"CREATEMONITOR {01234567-89ab-cdef-0123-456789abcdef}:[750.25,0.0025,350.5]");
  }
}  // namespace
