/**
 * Actual command entrypoint with a link-time fake at the raw transport boundary.
 * No SetupAPI, display, driver or IOCTL implementation is linked into this target.
 */
#include "topology_test_worker.h"
#include "src/display_device/vdd_command_buffer.h"
#include "src/logging_severity.h"

#include <functional>
#include <future>
#include <gtest/gtest.h>
#include <limits>
#include <locale>
#include <set>
#ifdef _WIN32
  #include "platform/windows/vdd_transport_shim.h"
#endif

namespace vdd = display_device::vdd_ioctl;
namespace modes = display_device::vdd_mode_list;
namespace topology = display_device::topology;

namespace {
  std::vector<std::wstring> sent;
  vdd::result transport_result;
  vdd::gated_send_t gated_result;
  bool use_native_transport {};
  bool send_transport_off_worker {};
  int transport_calls {};
  const std::string guid = "{01234567-89ab-cdef-0123-456789abcdef}";

  template <class T>
  concept accepted_command = requires(const T &request) {
    vdd::send_command(request);
  };

  template <class T>
  concept capability_command = requires(const topology::mutation_capability_t &capability, const T &request) {
    vdd::send_command(capability, request);
  };

  using topology_test::with_capability;

  class VddCommands: public ::testing::Test {
  protected:
    void
    SetUp() override {
      sent.clear();
      transport_result = vdd::result::success;
      gated_result = { vdd::delivery_e::delivered, {} };
      use_native_transport = false;
      send_transport_off_worker = false;
      transport_calls = 0;
#ifdef _WIN32
      vdd_transport_test::state = {};
#endif
    }

    void
    rejects(const vdd::command_t &request) {
      sent.clear();
      EXPECT_EQ(send(request), vdd::result::failed);
      EXPECT_TRUE(sent.empty()) << "invalid request reached the driver transport";
    }

    vdd::result
    send(const vdd::command_t &request) {
      auto result = vdd::result::failed;
      with_capability(topology::command_kind_e::launch_prepare, false, [&](const auto &capability) {
        result = vdd::send_command(capability, request);
      });
      return result;
    }

    vdd::gated_send_t
    send_gated(const vdd::command_t &request, const vdd::driver_gate_t &gate = [](const auto &) { return std::nullopt; }) {
      vdd::gated_send_t result;
      with_capability(topology::command_kind_e::launch_prepare, false, [&](const auto &capability) {
        result = vdd::send_command_gated(capability, request, gate);
      });
      return result;
    }
  };
}  // namespace

boost::log::sources::severity_logger<int> warning;
boost::log::sources::severity_logger<int> debug;

namespace display_device::vdd_ioctl::detail {
  result
  send_command_buffer(const encoded_command_t &buffer) {
    ++transport_calls;
#ifdef _WIN32
    if (use_native_transport) {
      if (send_transport_off_worker) {
        return std::async(std::launch::async, [&] { return vdd_transport_test::send(buffer); }).get();
      }
      return vdd_transport_test::send(buffer);
    }
#endif
    sent.push_back(buffer.authorized_bytes());
    return transport_result;
  }

  gated_send_t
  send_command_buffer_gated(const encoded_command_t &buffer, const driver_gate_t &gate) {
    ++transport_calls;
#ifdef _WIN32
    if (use_native_transport) {
      return vdd_transport_test::send_gated(buffer, gate);
    }
#endif
    sent.push_back(buffer.authorized_bytes());
    return gated_result;
  }
}  // namespace display_device::vdd_ioctl::detail

namespace {
  TEST_F(VddCommands, MutationRequestsRequireAnExplicitCapability) {
    EXPECT_FALSE(accepted_command<vdd::command_t>);
    EXPECT_FALSE(accepted_command<vdd::create_monitor_t>);
    EXPECT_FALSE(accepted_command<vdd::destroy_monitors_t>);
    EXPECT_FALSE(accepted_command<vdd::set_modes_t>);
    EXPECT_FALSE(accepted_command<vdd::hardware_cursor_t>);
    EXPECT_TRUE(capability_command<vdd::command_t>);
    EXPECT_FALSE(capability_command<std::string>);
    EXPECT_FALSE(capability_command<std::wstring>);
  }

  TEST_F(VddCommands, TransportEnvelopesCannotBeForgedCopiedOrMoved) {
    using envelope = vdd::detail::encoded_command_t;
    EXPECT_FALSE(std::is_default_constructible_v<envelope>);
    EXPECT_FALSE((std::is_constructible_v<envelope, const topology::mutation_capability_t &,
      topology::operation_e, std::wstring>) );
    EXPECT_FALSE(std::is_copy_constructible_v<envelope>);
    EXPECT_FALSE(std::is_move_constructible_v<envelope>);
    EXPECT_FALSE(std::is_copy_assignable_v<envelope>);
    EXPECT_FALSE(std::is_move_assignable_v<envelope>);
  }

  TEST_F(VddCommands, EveryCommandKindIsCheckedBeforeTypedRequestsReachTransport) {
    using kind = topology::command_kind_e;
    using op = topology::operation_e;
    struct row_t {
      kind command;
      std::set<op> legacy;
      std::set<op> managed;
    };
    const std::vector<row_t> rows {
      { kind::startup_recovery, { op::vdd_create, op::vdd_destroy }, {} },
      { kind::legacy_startup_restore, {}, {} },
      { kind::launch_prepare, { op::vdd_create, op::vdd_destroy, op::vdd_set_modes, op::vdd_cursor }, { op::vdd_create, op::vdd_destroy } },
      { kind::launch_failed_restore, { op::vdd_create, op::vdd_destroy }, { op::vdd_destroy } },
      { kind::reconfigure, { op::vdd_create, op::vdd_destroy, op::vdd_set_modes, op::vdd_cursor }, { op::vdd_set_modes } },
      { kind::stream_restore, { op::vdd_create, op::vdd_destroy }, { op::vdd_destroy } },
      { kind::shutdown_restore, { op::vdd_create, op::vdd_destroy }, { op::vdd_destroy } },
      { kind::shutdown_vdd_destroy, { op::vdd_destroy }, {} },
      { kind::rotate, {}, {} },
      { kind::scale, {}, {} },
      { kind::vdd_toggle, { op::vdd_create, op::vdd_destroy }, {} },
      { kind::vdd_toggle_followup, { op::vdd_destroy }, {} },
      { kind::vdd_create, { op::vdd_create }, {} },
      { kind::vdd_destroy, { op::vdd_destroy }, {} },
      { kind::reset_persistence, { op::vdd_destroy }, {} },
      { kind::legacy_retry_tick, { op::vdd_destroy }, {} },
      { kind::legacy_unlock_restore, {}, {} },
      { kind::native_restore, { op::vdd_destroy }, { op::vdd_destroy } },
    };
    const std::vector<std::pair<op, vdd::command_t>> requests {
      { op::vdd_create, vdd::create_monitor_t {} },
      { op::vdd_destroy, vdd::destroy_monitors_t {} },
      { op::vdd_set_modes, vdd::set_modes_t { { { { 1920, 1080 }, 60 } } } },
      { op::vdd_cursor, vdd::hardware_cursor_t { true } },
    };
    for (const auto &row : rows) {
      for (const bool managed : { false, true }) {
        with_capability(row.command, managed, [&](const auto &capability) {
          SCOPED_TRACE(std::string { topology::to_string(row.command) } + (managed ? "/managed" : "/legacy"));
          const auto &allowed = managed ? row.managed : row.legacy;
          for (const auto &[operation, request] : requests) {
            SCOPED_TRACE(topology::to_string(operation));
            sent.clear();
            transport_calls = 0;
            if (allowed.contains(operation)) {
              EXPECT_EQ(vdd::send_command(capability, request), vdd::result::success);
            }
            else {
              EXPECT_THROW((void) vdd::send_command(capability, request), topology::capability_error_t);
            }
            EXPECT_EQ(sent.size(), allowed.contains(operation) ? 1 : 0);
            EXPECT_EQ(transport_calls, allowed.contains(operation) ? 1 : 0);
          }
        });
      }
    }
  }

  TEST_F(VddCommands, AWorkerCapabilityCannotSendFromAnotherThread) {
    with_capability(topology::command_kind_e::launch_prepare, false, [&](const auto &capability) {
      for (const auto &request : std::vector<vdd::command_t> {
             vdd::create_monitor_t {}, vdd::destroy_monitors_t {},
             vdd::set_modes_t { { { { 1920, 1080 }, 60 } } }, vdd::hardware_cursor_t { true } }) {
        sent.clear();
        transport_calls = 0;
        auto elsewhere = std::async(std::launch::async, [&] { return vdd::send_command(capability, request); });
        EXPECT_THROW((void) elsewhere.get(), topology::capability_error_t);
        EXPECT_TRUE(sent.empty());
        EXPECT_EQ(transport_calls, 0);
      }
    });
  }

  TEST_F(VddCommands, UnauthorizedRequestsAreRejectedBeforePayloadValidation) {
    with_capability(topology::command_kind_e::rotate, false, [&](const auto &capability) {
      EXPECT_THROW((void) vdd::send_command(capability, vdd::create_monitor_t { "invalid-guid" }), topology::capability_error_t);
      EXPECT_THROW((void) vdd::send_command(capability, vdd::set_modes_t {}), topology::capability_error_t);
      EXPECT_TRUE(sent.empty());
    });
  }

#ifdef _WIN32
  TEST_F(VddCommands, RawTransportRejectsOffWorkerBeforeEnumerationOrOpen) {
    use_native_transport = true;
    send_transport_off_worker = true;
    with_capability(topology::command_kind_e::launch_prepare, false, [&](const auto &capability) {
      for (const auto &request : std::vector<vdd::command_t> {
             vdd::create_monitor_t {}, vdd::destroy_monitors_t {},
             vdd::set_modes_t { { { { 1920, 1080 }, 60 } } }, vdd::hardware_cursor_t { true } }) {
        vdd_transport_test::state = {};
        EXPECT_THROW((void) vdd::send_command(capability, request), topology::capability_error_t);
        EXPECT_EQ(vdd_transport_test::state.enumerations, 0);
        EXPECT_EQ(vdd_transport_test::state.opens, 0);
        EXPECT_EQ(vdd_transport_test::state.writes, 0);
      }
    });
  }

  TEST_F(VddCommands, RawTransportPreservesTerminatedBytesAndFailureResultsWithoutRetry) {
    use_native_transport = true;
    using native_outcome = vdd_transport_test::outcome_e;
    with_capability(topology::command_kind_e::launch_prepare, false, [&](const auto &capability) {
      for (const auto outcome : { native_outcome::success, native_outcome::missing,
             native_outcome::open_failed, native_outcome::write_failed }) {
        vdd_transport_test::state = { .outcome = outcome };
        const auto result = vdd::send_command(capability, vdd::hardware_cursor_t { true });
        const auto &state = vdd_transport_test::state;
        EXPECT_EQ(result, outcome == native_outcome::success ? vdd::result::success :
                          outcome == native_outcome::missing ? vdd::result::interface_missing :
                                                               vdd::result::failed);
        EXPECT_EQ(state.enumerations, 1);
        EXPECT_EQ(state.opens, outcome == native_outcome::missing ? 0 : 1);
        const bool opened = outcome == native_outcome::success || outcome == native_outcome::write_failed;
        EXPECT_EQ(state.writes, opened ? 1 : 0);
        EXPECT_EQ(state.closes, opened ? 1 : 0);
        EXPECT_EQ(state.list_destroys, outcome == native_outcome::missing ? 0 : 1);
        if (opened) {
          EXPECT_EQ(state.payload, L"HARDWARECURSOR true");
          EXPECT_EQ(state.payload_bytes, (state.payload.size() + 1) * sizeof(wchar_t));
        }
        EXPECT_EQ(state.version_reads, 0) << "the ungated transport must not read the driver version";
      }
    });
  }

  TEST_F(VddCommands, GatedRawTransportGatesOnTheInterfaceDeviceVersionAndReportsDelivery) {
    use_native_transport = true;
    using native_outcome = vdd_transport_test::outcome_e;
    std::vector<std::optional<std::string>> seen;
    const vdd::driver_gate_t accept = [&](const std::optional<std::string> &version) -> std::optional<std::string> {
      seen.push_back(version);
      return std::nullopt;
    };
    with_capability(topology::command_kind_e::launch_prepare, false, [&](const auto &capability) {
      for (const auto outcome : { native_outcome::success, native_outcome::missing,
             native_outcome::open_failed, native_outcome::write_failed }) {
        SCOPED_TRACE(static_cast<int>(outcome));
        vdd_transport_test::state = { .outcome = outcome };
        seen.clear();
        const auto sent_result = vdd::send_command_gated(capability, vdd::create_monitor_t { guid }, accept);
        const auto &state = vdd_transport_test::state;
        EXPECT_EQ(sent_result.delivery, outcome == native_outcome::success     ? vdd::delivery_e::delivered :
                                        outcome == native_outcome::write_failed ? vdd::delivery_e::uncertain :
                                                                                  vdd::delivery_e::not_sent);
        if (outcome != native_outcome::success) EXPECT_FALSE(sent_result.detail.empty());
        const bool resolved = outcome != native_outcome::missing;
        EXPECT_EQ(state.version_reads, resolved ? 1 : 0);
        EXPECT_EQ(seen, resolved ? std::vector<std::optional<std::string>> { "100.0.18.2" } : std::vector<std::optional<std::string>> {});
        EXPECT_EQ(state.opens, resolved ? 1 : 0);
        const bool opened = outcome == native_outcome::success || outcome == native_outcome::write_failed;
        EXPECT_EQ(state.writes, opened ? 1 : 0);
        EXPECT_EQ(state.closes, opened ? 1 : 0);
        EXPECT_EQ(state.list_destroys, resolved ? 1 : 0);
        if (opened) EXPECT_EQ(state.payload, L"CREATEMONITOR {01234567-89ab-cdef-0123-456789abcdef}:[1000,0.001,1000]");
      }

      vdd_transport_test::state = {};
      vdd_transport_test::state.driver_version = L"100.0.15.9";
      const auto refused = vdd::send_command_gated(capability, vdd::create_monitor_t { guid },
        [](const std::optional<std::string> &version) -> std::optional<std::string> { return "too old: " + version.value_or("?"); });
      EXPECT_EQ(refused.delivery, vdd::delivery_e::not_sent);
      EXPECT_EQ(refused.detail, "too old: 100.0.15.9");
      EXPECT_EQ(vdd_transport_test::state.opens, 0);
      EXPECT_EQ(vdd_transport_test::state.writes, 0);
      EXPECT_EQ(vdd_transport_test::state.list_destroys, 1);

      vdd_transport_test::state = {};
      vdd_transport_test::state.driver_version.reset();
      seen.clear();
      EXPECT_EQ(vdd::send_command_gated(capability, vdd::destroy_monitors_t {}, accept).delivery, vdd::delivery_e::delivered);
      EXPECT_EQ(seen, (std::vector<std::optional<std::string>> { std::nullopt }));
      EXPECT_EQ(vdd_transport_test::state.payload, L"DESTROYMONITOR");
    });
  }
#endif

  TEST_F(VddCommands, GatedSendsKeepTheWireBytesAndPassDeliveryThrough) {
    for (const auto delivery : { vdd::delivery_e::delivered, vdd::delivery_e::not_sent, vdd::delivery_e::uncertain }) {
      gated_result = { delivery, "transport detail" };
      sent.clear();
      const auto created = send_gated(vdd::create_monitor_t { guid });
      EXPECT_EQ(created.delivery, delivery);
      EXPECT_EQ(created.detail, "transport detail");
      EXPECT_EQ(send_gated(vdd::destroy_monitors_t {}).delivery, delivery);
      ASSERT_EQ(send(vdd::create_monitor_t { guid }), vdd::result::success);
      ASSERT_EQ(sent.size(), 3u);
      EXPECT_EQ(sent[0], sent[2]);
      EXPECT_EQ(sent[1], L"DESTROYMONITOR");
    }
  }

  TEST_F(VddCommands, GatedInvalidRequestsAreNotSent) {
    for (const vdd::command_t &invalid : { vdd::command_t {}, vdd::command_t { vdd::create_monitor_t { "RELOAD_DRIVER" } },
           vdd::command_t { vdd::set_modes_t {} } }) {
      const auto result = send_gated(invalid);
      EXPECT_EQ(result.delivery, vdd::delivery_e::not_sent);
      EXPECT_FALSE(result.detail.empty());
    }
    EXPECT_EQ(transport_calls, 0);
    EXPECT_TRUE(sent.empty());
  }

  TEST_F(VddCommands, GatedSendsNeedTheSameCapability) {
    with_capability(topology::command_kind_e::rotate, false, [&](const auto &capability) {
      EXPECT_THROW((void) vdd::send_command_gated(capability, vdd::create_monitor_t { guid }, [](const auto &) { return std::nullopt; }),
        topology::capability_error_t);
    });
    EXPECT_EQ(transport_calls, 0);
  }

  TEST_F(VddCommands, SupportedCommandsKeepExistingWireBytesAndResults) {
    for (const auto outcome : { vdd::result::success, vdd::result::failed, vdd::result::interface_missing }) {
      transport_result = outcome;
      sent.clear();
      EXPECT_EQ(send(vdd::create_monitor_t {}), outcome);
      EXPECT_EQ(send(vdd::destroy_monitors_t {}), outcome);
      EXPECT_EQ(send(vdd::hardware_cursor_t { true }), outcome);
      EXPECT_EQ(send(vdd::hardware_cursor_t { false }), outcome);
      EXPECT_EQ(sent, (std::vector<std::wstring> {
                        L"CREATEMONITOR", L"DESTROYMONITOR", L"HARDWARECURSOR true", L"HARDWARECURSOR false" }));
    }
  }

  TEST_F(VddCommands, CreationPreservesGuidHdrAndOptionalPhysicalSize) {
    vdd::create_monitor_t request { guid, 750.25f, 0.0025f, 350.5f, 34.5f, 19.4f };
    EXPECT_EQ(send(request), vdd::result::success);
    ASSERT_EQ(sent.size(), 1);
    EXPECT_EQ(sent.back(), L"CREATEMONITOR {01234567-89ab-cdef-0123-456789abcdef}:[750.25,0.0025,350.5][34.5,19.4]");
    for (const auto dimension : { &vdd::create_monitor_t::width_cm, &vdd::create_monitor_t::height_cm }) {
      for (const float unspecified : { 0.0f, -1.0f }) {
        auto no_size = request;
        no_size.*dimension = unspecified;
        EXPECT_EQ(send(no_size), vdd::result::success);
        EXPECT_EQ(sent.back(), L"CREATEMONITOR {01234567-89ab-cdef-0123-456789abcdef}:[750.25,0.0025,350.5]");
      }
    }
    request.guid = "{01234567-89AB-CDEF-0123-456789ABCDEF}";
    EXPECT_EQ(send(request), vdd::result::success);
    EXPECT_EQ(sent.back(), L"CREATEMONITOR {01234567-89AB-CDEF-0123-456789ABCDEF}:[750.25,0.0025,350.5][34.5,19.4]");
    request.guid.clear();
    request.max_nits = std::numeric_limits<float>::quiet_NaN();
    EXPECT_EQ(send(request), vdd::result::success);
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
    ASSERT_EQ(send(vdd::set_modes_t { ordered }), vdd::result::success);
    ASSERT_EQ(sent.size(), 1);
    EXPECT_EQ(sent.back(), L"SETMODES 1280x720x120,1920x1080x60");
    EXPECT_EQ(sent.back(), modes::format_setmodes_command(ordered));
    const auto full = std::vector<modes::vdd_mode_t>(modes::kMaxTotalModes, { { 1280, 720 }, 60 });
    ASSERT_EQ(send(vdd::set_modes_t { full }), vdd::result::success);
    EXPECT_EQ(sent.back(), modes::format_setmodes_command(full));

    std::vector<modes::vdd_mode_t> limit(61, { { 1000000000, 1000000000 }, 1000000000 });
    limit.push_back({ { 1000000000, 1000000 }, 100000 });
    ASSERT_EQ(modes::format_setmodes_command(limit).size(), modes::kMaxCommandChars);
    ASSERT_EQ(send(vdd::set_modes_t { limit }), vdd::result::success);
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
    ASSERT_EQ(send(vdd::create_monitor_t { guid, 750.25f, 0.0025f, 350.5f }), vdd::result::success);
    ASSERT_EQ(sent.size(), 1);
    EXPECT_EQ(sent.back(), L"CREATEMONITOR {01234567-89ab-cdef-0123-456789abcdef}:[750.25,0.0025,350.5]");
  }
}  // namespace
