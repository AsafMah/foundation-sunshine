/**
 * @file tests/unit/test_display_topology_windows_native.cpp
 * @brief The P1 Windows native platform against a fake CCD/SetupAPI/IOCTL edge.
 */
#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <map>

#include <gtest/gtest.h>

#include "display_topology_fixtures.h"
#include "src/display_device/topology_capability.h"
#include "src/display_device/topology_coordinator.h"
#include "src/display_device/topology_journal.h"
#include "src/display_device/topology_journal_store.h"
#include "src/display_device/topology_windows_native.h"

namespace {
  namespace dt = display_device::topology;
  using namespace std::chrono_literals;
  using display_topology_test::id;
  using dt::edge_delivery_e;
  using dt::head_class_e;
  using dt::native_verdict_e;
  using delivery_e = dt::creation_delivery_e;
  using state_e = display_device::device_state_e;

  constexpr auto zako = "Zako HDR";

  ::testing::AssertionResult
  mentions(const std::string &text, const std::string &needle) {
    if (text.find(needle) != std::string::npos) return ::testing::AssertionSuccess();
    return ::testing::AssertionFailure() << "\"" << text << "\" does not mention \"" << needle << "\"";
  }

  dt::uuid_bytes_t
  next_uuid() {
    static std::atomic<std::uint32_t> counter { 0 };
    const auto value = ++counter;
    dt::uuid_bytes_t bytes { 0x57 };
    bytes[14] = static_cast<std::uint8_t>(value >> 8);
    bytes[15] = static_cast<std::uint8_t>(value);
    return bytes;
  }

  dt::driver_instance_id_t
  driver() {
    return id<dt::driver_instance_id_t>(0x60);
  }

  dt::edge_display_t
  physical(const std::string &name, std::int32_t x, state_e state = state_e::active) {
    dt::edge_display_t display;
    display.id = dt::os_display_id_t { name };
    display.info = { "\\\\.\\DISPLAY-" + name, "Panel " + name, state, display_device::hdr_state_e::disabled };
    display.layout = display_topology_test::layout(x);
    display.adapter = dt::adapter_luid_t { 7 };
    display.kind = head_class_e::physical;
    display.arrival = 11;
    return display;
  }

  dt::edge_display_t
  zako_head(const std::string &name, std::optional<dt::uuid_bytes_t> container, std::uint64_t arrival) {
    auto display = physical(name, 3840);
    display.info.friendly_name = zako;
    display.adapter = dt::adapter_luid_t { 9 };
    display.kind = head_class_e::driver_head;
    display.container = container;
    display.arrival = arrival;
    return display;
  }

  class fake_edge_t final: public dt::native_edge_t {
  public:
    dt::edge_snapshot_t
    snapshot() override {
      ++snapshots;
      if (before_snapshot) before_snapshot(snapshots);
      if (!available) return {};
      return { true, displays };
    }

    dt::edge_send_t
    send_create(const dt::mutation_capability_t &, const dt::head_guid_t &intent, const dt::version_gate_t &gate) override {
      if (const auto refusal = gate(version)) {
        refusals.push_back(*refusal);
        return { edge_delivery_e::not_sent, *refusal };
      }
      creates.push_back(intent);
      if (on_create) return on_create(intent);
      auto head = zako_head("vdd-" + std::to_string(creates.size()), intent.value, 500 + creates.size());
      if (create_inactive) deactivate(head);
      displays.push_back(std::move(head));
      return { edge_delivery_e::delivered, {} };
    }

    /** As the Win32 edge lists a display that is not on the desktop. */
    static void
    deactivate(dt::edge_display_t &display) {
      display.info.display_name.clear();
      display.info.device_state = state_e::inactive;
      display.layout.reset();
    }

    dt::edge_send_t
    activate(const dt::mutation_capability_t &capability, const dt::os_display_id_t &display) override {
      capability.require(dt::operation_e::native_activate);
      activations.push_back(display);
      if (on_activate) return on_activate(display);
      for (auto &listed : displays) {
        if (listed.id != display) continue;
        listed.info.display_name = "\\\\.\\DISPLAY-" + display.value;
        listed.info.device_state = state_e::active;
        listed.layout = display_topology_test::layout(3840);
      }
      return { edge_delivery_e::delivered, {} };
    }

    dt::edge_send_t
    arrange(const dt::mutation_capability_t &capability, const std::vector<dt::placement_t> &placements,
      const std::vector<dt::os_display_id_t> &off) override {
      capability.require(dt::arrange_operation(capability.kind()));
      arrangements.push_back(placements);
      offs.push_back(off);
      if (on_arrange) return on_arrange(placements);
      for (auto &listed : displays) {
        if (std::ranges::find(off, listed.id) != off.end()) {
          listed.info.device_state = state_e::inactive;
          listed.info.display_name.clear();
          listed.layout.reset();
        }
      }
      for (const auto &placement : placements) {
        for (auto &listed : displays) {
          if (listed.id == placement.display && listed.layout) listed.layout->position = placement.position;
        }
      }
      return { edge_delivery_e::delivered, {} };
    }

    dt::edge_send_t
    send_destroy_all(const dt::mutation_capability_t &) override {
      ++destroys;
      if (on_destroy) return on_destroy();
      drop_heads();
      return { edge_delivery_e::delivered, {} };
    }

    std::optional<std::vector<dt::edge_mode_t>>
    list_modes(const std::string &display_name) override {
      ++mode_reads;
      if (modes_unreadable) return std::nullopt;
      const bool driver_head = std::ranges::any_of(displays, [&](const auto &display) {
        return display.kind == head_class_e::driver_head && display.info.display_name == display_name;
      });
      if (!driver_head) return std::nullopt;
      return driver_modes;
    }

    dt::edge_send_t
    set_mode(const dt::mutation_capability_t &capability, const dt::os_display_id_t &display, const display_device::display_mode_t &mode) override {
      capability.require(dt::operation_e::write_display_modes);
      mode_sets.emplace_back(display, mode);
      if (on_set_mode) return on_set_mode(display, mode);
      for (auto &listed : displays) {
        if (listed.id == display && listed.layout) listed.layout->mode = mode;
      }
      return { edge_delivery_e::delivered, {} };
    }

    dt::edge_send_t
    send_set_modes(const dt::mutation_capability_t &capability, const std::vector<dt::edge_mode_t> &modes, const dt::version_gate_t &gate) override {
      capability.require(dt::operation_e::vdd_set_modes);
      if (const auto refusal = gate(version)) {
        refusals.push_back(*refusal);
        return { edge_delivery_e::not_sent, *refusal };
      }
      plans.push_back(modes);
      if (on_set_modes) return on_set_modes(modes);
      driver_modes = modes;
      // ZakoVDD recreates its heads: same ContainerId, a new arrival and OS identity.
      for (auto &display : displays) {
        if (display.kind != head_class_e::driver_head) continue;
        const auto name = "vdd-r" + std::to_string(++recreated);
        display.id = dt::os_display_id_t { name };
        display.info.display_name = "\\\\.\\DISPLAY-" + name;
        display.arrival += 100;
        if (recreate_inactive) deactivate(display);
      }
      return { edge_delivery_e::delivered, {} };
    }

    dt::missing_head_report_t
    describe_missing_head(const dt::head_guid_t &intent) override {
      described.push_back(intent);
      if (!describes) return {};
      const bool listed = std::ranges::any_of(displays, [&](const auto &display) { return display.container == std::optional { intent.value }; });
      const bool hidden = std::ranges::find(hidden_monitors, intent) != hidden_monitors.end();
      const bool disabled = std::ranges::find(disabled_monitors, intent) != disabled_monitors.end();
      dt::missing_head_report_t report { listed || hidden || disabled, std::nullopt, "fake monitors=" + std::to_string(hidden_monitors.size()) };
      if (disabled) report.problem = 22;
      return report;
    }

    std::chrono::steady_clock::time_point
    now() override {
      return clock;
    }

    void
    sleep(std::chrono::milliseconds duration) override {
      ++sleeps;
      clock += duration;
    }

    void
    drop_heads() {
      std::erase_if(displays, [](const auto &display) { return display.kind == head_class_e::driver_head; });
    }

    bool available { true };
    std::vector<dt::edge_display_t> displays { physical("os-A", 0, state_e::primary), physical("os-B", 1920) };
    std::optional<std::string> version { "100.0.18.2" };
    std::vector<dt::head_guid_t> creates;
    std::vector<std::string> refusals;
    int destroys {};
    int snapshots {};
    int sleeps {};
    std::chrono::steady_clock::time_point clock {};
    std::function<dt::edge_send_t(const dt::head_guid_t &)> on_create;
    std::function<dt::edge_send_t()> on_destroy;
    std::function<void(int)> before_snapshot;
    std::vector<dt::edge_mode_t> driver_modes { { 1920, 1080, 60 }, { 1280, 720, 60 } };
    bool modes_unreadable {};
    int mode_reads {};
    int recreated {};
    std::vector<std::pair<dt::os_display_id_t, display_device::display_mode_t>> mode_sets;
    std::vector<std::vector<dt::edge_mode_t>> plans;
    std::function<dt::edge_send_t(const dt::os_display_id_t &, const display_device::display_mode_t &)> on_set_mode;
    std::function<dt::edge_send_t(const std::vector<dt::edge_mode_t> &)> on_set_modes;
    /** New heads arrive off the desktop, as ZakoVDD heads did in the G1 hardware run. */
    bool create_inactive {};
    /** SETMODES recreates heads off the desktop. */
    bool recreate_inactive {};
    std::vector<dt::os_display_id_t> activations;
    std::function<dt::edge_send_t(const dt::os_display_id_t &)> on_activate;
    std::vector<std::vector<dt::placement_t>> arrangements;
    std::vector<std::vector<dt::os_display_id_t>> offs;
    std::function<dt::edge_send_t(const std::vector<dt::placement_t> &)> on_arrange;
    /** Whether describe_missing_head can tell (the Win32 edge reads SetupAPI and CCD). */
    bool describes { true };
    /** Monitors the driver created that Windows lists in no display path. */
    std::vector<dt::head_guid_t> hidden_monitors;
    /** Monitors the driver created whose device another program disabled (CM_PROB_DISABLED). */
    std::vector<dt::head_guid_t> disabled_monitors;
    std::vector<dt::head_guid_t> described;
  };

  /** Short waits on the fake clock; creation settles at once unless a test asks for production timing. */
  dt::windows_native_options_t
  options() {
    return { driver(), zako, 500ms, 100ms, 500ms, 0ms };
  }

  void
  with_capability(dt::command_kind_e kind, const std::function<void(const dt::mutation_capability_t &)> &body, bool managed_mode = false) {
    dt::coordinator_options_t coordinator_options;
    coordinator_options.managed_mode = managed_mode;
    coordinator_options.probes.startup_restore_quiescent = [] { return true; };
    if (managed_mode) {
      coordinator_options.probes.active_streams = [] { return std::size_t {}; };
      coordinator_options.native_restore = [](const auto &, const auto &) {};
    }
    dt::coordinator_t coordinator { coordinator_options };
    bool ran = false;
    std::exception_ptr thrown;
    const dt::command_body_t wrapped = [&](const dt::mutation_capability_t &capability, const dt::command_context_t &) {
      ran = true;
      try {
        body(capability);
      }
      catch (...) {
        thrown = std::current_exception();
      }
    };
    coordinator.start([](const dt::mutation_capability_t &, const dt::command_context_t &) {}).get();
    if (kind == dt::command_kind_e::launch_prepare) {
      const auto admission = coordinator.admit_launch({ 1, "cert:windows-native-test" });
      if (!admission) throw std::runtime_error { "the test launch was not admitted" };
      coordinator.run_launch_prepare(*admission.token, wrapped);
      coordinator.abort_launch(*admission.token, {});
    }
    else {
      coordinator.run(kind, wrapped);
    }
    coordinator.stop();
    if (!ran) throw std::runtime_error { "the command body did not run" };
    if (thrown) std::rethrow_exception(thrown);
  }

  class memory_store_t final: public dt::journal_store_t {
  public:
    dt::journal_load_t
    load() override {
      if (!bytes) return {};
      auto record = dt::deserialize_journal(*bytes);
      if (!record) return { dt::journal_load_status_e::invalid, std::nullopt, record.error().detail };
      return { dt::journal_load_status_e::loaded, std::move(*record), {} };
    }

    dt::result_t<void>
    save(const dt::journal_record_t &record) override {
      auto serialized = dt::serialize_journal(record);
      if (!serialized) return std::unexpected(serialized.error());
      bytes = *serialized;
      return {};
    }

    dt::result_t<void>
    clear() override {
      bytes.reset();
      return {};
    }

    std::optional<dt::journal_record_t>
    record() const {
      if (!bytes) return std::nullopt;
      auto record = dt::deserialize_journal(*bytes);
      return record ? std::optional { std::move(*record) } : std::nullopt;
    }

    std::optional<std::string> bytes;
  };

  const dt::observed_display_t *
  find(const dt::inventory_t &inventory, const std::string &name) {
    const auto it = std::find_if(inventory.displays.begin(), inventory.displays.end(),
      [&](const auto &display) { return display.id.value == name; });
    return it == inventory.displays.end() ? nullptr : &*it;
  }

  class WindowsNative: public ::testing::Test {
  protected:
    dt::result_t<delivery_e>
    create(const dt::head_guid_t &intent) {
      std::optional<dt::result_t<delivery_e>> result;
      with_capability(dt::command_kind_e::launch_prepare, [&](const auto &capability) { result = platform.create_head(capability, intent); });
      return *result;
    }

    dt::result_t<void>
    remove() {
      std::optional<dt::result_t<void>> result;
      with_capability(dt::command_kind_e::native_restore, [&](const auto &capability) { result = platform.remove_all_heads(capability); });
      return *result;
    }

    dt::result_t<void>
    change(const dt::observed_display_t &head, const display_device::display_mode_t &mode) {
      std::optional<dt::result_t<void>> result;
      with_capability(dt::command_kind_e::reconfigure, [&](const auto &capability) { result = platform.change_head_mode(capability, head, mode); }, true);
      return *result;
    }

    /** Creates one head and returns it as the platform lists it. */
    dt::observed_display_t
    created_head() {
      const auto created = create(id<dt::head_guid_t>(0x71));
      if (!created) throw std::runtime_error { "the fixture head was not created" };
      const auto inventory = platform.inventory();
      const auto *head = find(inventory, "vdd-1");
      if (!head) throw std::runtime_error { "the fixture head is not listed" };
      return *head;
    }

    const dt::edge_display_t &
    the_head() const {
      for (const auto &display : edge.displays) {
        if (display.kind == head_class_e::driver_head) return display;
      }
      throw std::runtime_error { "the fake lists no head" };
    }

    fake_edge_t edge;
    dt::windows_native_platform_t platform { edge, options() };
  };
}  // namespace

// --- Pure converters ---------------------------------------------------------------

TEST(WindowsNativeConverters, DriverVersionIsExactlyFourDecimalParts) {
  const auto parsed = dt::parse_driver_version("100.0.18.2");
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->parts, (std::array<std::uint32_t, 4> { 100, 0, 18, 2 }));
  EXPECT_EQ(dt::parse_driver_version("0.0.0.65535").value_or(dt::driver_version_t {}).parts[3], 65535u);
  for (const auto *text : { "", "100.0.18", "100.0.18.2.1", "100.0.18.x", "100..18.2", "+1.0.0.0", "-1.0.0.0",
         " 1.0.0.0", "1.0.0.0 ", "1.0.0.65536", "1.0.0.99999999999", "1.0.0.", ".1.0.0", "1,0,0,0" }) {
    EXPECT_FALSE(dt::parse_driver_version(text)) << '"' << text << '"';
  }
}

TEST(WindowsNativeConverters, DriverVersionsCompareNumericallyPartByPart) {
  const auto v = [](const char *text) {
    const auto parsed = dt::parse_driver_version(text);
    if (!parsed) {
      ADD_FAILURE() << "unparsed " << text;
      return dt::driver_version_t {};
    }
    return *parsed;
  };
  EXPECT_LT(v("100.0.15.9"), v("100.0.15.10"));
  EXPECT_LT(v("100.0.15.10"), v("100.0.18.2"));
  EXPECT_LT(v("99.9.99.99"), v("100.0.0.0"));
}

TEST(WindowsNativeConverters, ContainerDriverPolicyFollowsTheZakoVddReleaseLines) {
  const auto refusal = [](const char *text) {
    const auto parsed = dt::parse_driver_version(text);
    if (!parsed) {
      ADD_FAILURE() << "unparsed " << text;
      return std::optional<std::string> { "unparsed" };
    }
    return dt::container_driver_refusal(*parsed);
  };
  // Main line: tag vA.B.C is DriverVer 100.A.B.C; every tag from v0.1.4 sets MonitorContainerId from the client GUID.
  // Win10 line: tag v0.15.P is DriverVer 15.0.15.P (earlier Win10 packages were published as 100.0.15.P).
  for (const auto *accepted : { "100.0.1.4", "100.0.15.10", "100.0.18.2", "100.1.0.0", "15.0.15.0", "15.0.15.10" }) {
    EXPECT_EQ(refusal(accepted), std::nullopt) << accepted;
  }
  // Older than v0.1.4, branch/PR artifacts (15.0.0.N), CI dev builds (99.MM.dd.HHmm) and unknown schemes.
  for (const auto *refused : { "100.0.1.3", "100.0.0.9", "15.0.0.321", "15.0.14.9", "15.1.15.10", "99.10.27.1530", "101.0.0.0", "1.0.0.0" }) {
    EXPECT_NE(refusal(refused), std::nullopt) << refused;
  }
}

TEST(WindowsNativeConverters, GuidTextKeepsBytesInTextualOrder) {
  dt::uuid_bytes_t bytes;
  for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<std::uint8_t>(i * 0x11 + 1);
  const auto text = dt::format_guid(bytes);
  EXPECT_EQ(text, "{01122334-4556-6778-899A-ABBCCDDEEF00}");
  EXPECT_EQ(dt::parse_guid(text), bytes);
  EXPECT_EQ(dt::parse_guid("{01122334-4556-6778-899a-abbccddeef00}"), bytes);
  for (const auto *bad : { "", "01122334-4556-6778-899A-ABBCCDDEEFF0", "{01122334-4556-6778-899A-ABBCCDDEEFF}",
         "{011223344-556-6778-899A-ABBCCDDEEFF0}", "{01122334-4556-6778-899A-ABBCCDDEEFG0}", "{01122334-4556-6778-899A-ABBCCDDEEFF0}x",
         "(01122334-4556-6778-899A-ABBCCDDEEFF0)" }) {
    EXPECT_FALSE(dt::parse_guid(bad)) << bad;
  }
}

// --- Inventory ----------------------------------------------------------------------

TEST_F(WindowsNative, InventoryMapsDisplaysAndProvesHeadsByContainerAndArrival) {
  const auto guid = id<dt::head_guid_t>(0x31);
  edge.displays.push_back(zako_head("vdd-x", guid.value, 77));
  const auto inventory = platform.inventory();
  ASSERT_TRUE(inventory.available);
  ASSERT_EQ(inventory.displays.size(), 3u);
  const auto *a = find(inventory, "os-A");
  ASSERT_TRUE(a);
  EXPECT_EQ(a->info.device_state, state_e::primary);
  EXPECT_EQ(a->layout, display_topology_test::layout(0));
  EXPECT_EQ(a->adapter, dt::adapter_luid_t { 7 });
  EXPECT_EQ(a->incarnation, dt::source_incarnation_t { 11 });
  EXPECT_FALSE(a->head);
  EXPECT_FALSE(platform.is_virtual_head(*a));
  const auto *head = find(inventory, "vdd-x");
  ASSERT_TRUE(head);
  EXPECT_TRUE(platform.is_virtual_head(*head));
  EXPECT_EQ(head->head, (dt::head_identity_t { driver(), guid, dt::head_incarnation_t { 77 } }));
  EXPECT_EQ(head->incarnation, dt::source_incarnation_t { 77 });
  ASSERT_EQ(inventory.heads.size(), 1u);
  EXPECT_EQ(inventory.heads.front().identity, *head->head);
  EXPECT_EQ(inventory.heads.front().display, dt::os_display_id_t { "vdd-x" });
  EXPECT_EQ(inventory.heads.front().adapter, dt::adapter_luid_t { 9 });
  EXPECT_FALSE(inventory.heads.front().owner);
}

TEST_F(WindowsNative, AHeadWithoutAContainerIsVirtualButUnproven) {
  edge.displays.push_back(zako_head("vdd-x", std::nullopt, 77));
  const auto inventory = platform.inventory();
  ASSERT_TRUE(inventory.available);
  const auto *head = find(inventory, "vdd-x");
  ASSERT_TRUE(head);
  EXPECT_TRUE(platform.is_virtual_head(*head));
  EXPECT_FALSE(head->head);
  EXPECT_TRUE(inventory.heads.empty());
}

TEST_F(WindowsNative, AnotherVirtualDisplayDriverIsNeverAdoptedAsAHead) {
  // Punktfunk Virtual Display (root\pf_vdisplay): the edge classes it physical; even with a
  // ContainerId equal to a pending intent and an arrival time, it is not a head of ours.
  const auto guid = id<dt::head_guid_t>(0x33);
  auto punktfunk = physical("pf-1", 5760);
  punktfunk.info.friendly_name = "Punktfunk Virtual Display";
  punktfunk.container = guid.value;
  punktfunk.arrival = 88;
  edge.displays.push_back(punktfunk);
  const auto inventory = platform.inventory();
  ASSERT_TRUE(inventory.available);
  const auto *display = find(inventory, "pf-1");
  ASSERT_TRUE(display);
  EXPECT_FALSE(platform.is_virtual_head(*display));
  EXPECT_FALSE(display->head);
  EXPECT_TRUE(inventory.heads.empty());
}

TEST_F(WindowsNative, AnythingTheEdgeCannotEstablishMakesTheInventoryUnavailable) {
  const std::vector<std::pair<std::string, std::function<void(fake_edge_t &)>>> cases {
    { "snapshot unavailable", [](auto &e) { e.available = false; } },
    { "duplicate id", [](auto &e) { e.displays.push_back(physical("os-A", 3840)); } },
    { "unknown classification", [](auto &e) { e.displays[1].kind = head_class_e::unknown; } },
    { "driver head without the driver's name", [](auto &e) { e.displays[1].kind = head_class_e::driver_head; } },
    { "physical display with the driver's name", [](auto &e) { e.displays[1].info.friendly_name = zako; } },
    { "head with a container but no arrival", [](auto &e) { e.displays.push_back(zako_head("vdd-x", id<dt::head_guid_t>(1).value, 0)); } },
    { "empty os id", [](auto &e) { e.displays[1].id = dt::os_display_id_t {}; } },
  };
  for (const auto &[name, configure] : cases) {
    SCOPED_TRACE(name);
    fake_edge_t local;
    configure(local);
    dt::windows_native_platform_t local_platform { local, options() };
    const auto inventory = local_platform.inventory();
    EXPECT_FALSE(inventory.available);
    EXPECT_TRUE(inventory.displays.empty());
  }
}

TEST_F(WindowsNative, InventoryRevisionsStrictlyIncrease) {
  const auto first = platform.inventory().revision;
  edge.available = false;
  const auto second = platform.inventory().revision;
  edge.available = true;
  const auto third = platform.inventory().revision;
  EXPECT_LT(first, second);
  EXPECT_LT(second, third);
}

// --- Create -------------------------------------------------------------------------

TEST_F(WindowsNative, CreateSendsTheIntentAndWaitsUntilItsHeadIsListed) {
  const auto intent = id<dt::head_guid_t>(0x41);
  edge.on_create = [&](const dt::head_guid_t &) { return dt::edge_send_t { edge_delivery_e::delivered, {} }; };
  edge.before_snapshot = [&](int snapshot) {
    if (snapshot == 3) edge.displays.push_back(zako_head("vdd-other", id<dt::head_guid_t>(0x42).value, 5));
    if (snapshot == 4) {
      edge.displays.pop_back();
      edge.displays.push_back(zako_head("vdd-1", intent.value, 6));
    }
  };
  const auto created = create(intent);
  ASSERT_TRUE(created) << created.error().detail;
  EXPECT_EQ(*created, delivery_e::accepted);
  ASSERT_EQ(edge.creates.size(), 1u);
  EXPECT_EQ(edge.creates.front(), intent);
  EXPECT_EQ(edge.snapshots, 4);
  EXPECT_EQ(edge.sleeps, 3);
}

TEST_F(WindowsNative, AnUnavailableSnapshotDoesNotCountAsListed) {
  const auto intent = id<dt::head_guid_t>(0x43);
  edge.before_snapshot = [&](int snapshot) { edge.available = snapshot >= 3; };
  const auto created = create(intent);
  ASSERT_TRUE(created);
  EXPECT_EQ(*created, delivery_e::accepted);
  EXPECT_EQ(edge.snapshots, 3);
}

TEST_F(WindowsNative, ADeliveredCreateThatIsNeverListedIsStillAcceptedAfterABoundedWait) {
  edge.on_create = [](const dt::head_guid_t &) { return dt::edge_send_t { edge_delivery_e::delivered, {} }; };
  const auto start = edge.clock;
  const auto created = create(id<dt::head_guid_t>(0x44));
  ASSERT_TRUE(created);
  EXPECT_EQ(*created, delivery_e::accepted);
  EXPECT_GE(edge.clock - start, 500ms);
  EXPECT_LE(edge.clock - start, 600ms);
  EXPECT_LE(edge.snapshots, 7);
}

TEST_F(WindowsNative, CreateIsGatedOnTheControlInterfaceDriverVersion) {
  for (const std::optional<std::string> version : { std::optional<std::string> {}, std::optional<std::string> { "garbage" },
         std::optional<std::string> { "100.0.1.3" }, std::optional<std::string> { "15.0.0.321" },
         std::optional<std::string> { "99.10.27.1530" }, std::optional<std::string> { "1.0.0.0" } }) {
    SCOPED_TRACE(version.value_or("<unreadable>"));
    edge.version = version;
    edge.refusals.clear();
    const auto created = create(id<dt::head_guid_t>(0x45));
    ASSERT_FALSE(created);
    EXPECT_TRUE(mentions(created.error().detail, "version"));
    EXPECT_EQ(edge.refusals.size(), 1u);
  }
  EXPECT_TRUE(edge.creates.empty());
  EXPECT_EQ(edge.snapshots, 0);
  for (const auto *version : { "15.0.15.10", "100.0.18.2", "100.0.15.10" }) {
    SCOPED_TRACE(version);
    edge.version = version;
    edge.drop_heads();
    ASSERT_TRUE(create(id<dt::head_guid_t>(0x46)));
  }
  EXPECT_EQ(edge.creates.size(), 3u);
}

TEST_F(WindowsNative, CreateDeliveryMapsToTheContract) {
  edge.on_create = [](const dt::head_guid_t &) { return dt::edge_send_t { edge_delivery_e::not_sent, "open failed" }; };
  const auto refused = create(id<dt::head_guid_t>(0x47));
  ASSERT_FALSE(refused);
  EXPECT_TRUE(mentions(refused.error().detail, "open failed"));
  edge.on_create = [](const dt::head_guid_t &) { return dt::edge_send_t { edge_delivery_e::uncertain, "ioctl failed" }; };
  const auto uncertain = create(id<dt::head_guid_t>(0x48));
  ASSERT_TRUE(uncertain);
  EXPECT_EQ(*uncertain, delivery_e::uncertain);
  EXPECT_EQ(edge.snapshots, 0);
}

// --- Remove -------------------------------------------------------------------------

TEST_F(WindowsNative, RemoveWaitsUntilNoHeadIsListed) {
  edge.displays.push_back(zako_head("vdd-1", id<dt::head_guid_t>(0x51).value, 3));
  edge.on_destroy = [] { return dt::edge_send_t { edge_delivery_e::delivered, {} }; };
  edge.before_snapshot = [&](int snapshot) {
    if (snapshot == 2) edge.available = false;
    if (snapshot == 3) {
      edge.available = true;
      edge.drop_heads();
    }
  };
  const auto removed = remove();
  ASSERT_TRUE(removed) << removed.error().detail;
  EXPECT_EQ(edge.destroys, 1);
  EXPECT_EQ(edge.snapshots, 3);
}

TEST_F(WindowsNative, AHeadLeftWithoutAContainerStillBlocksRemoval) {
  edge.displays.push_back(zako_head("vdd-1", std::nullopt, 3));
  edge.on_destroy = [] { return dt::edge_send_t { edge_delivery_e::delivered, {} }; };
  const auto start = edge.clock;
  const auto removed = remove();
  ASSERT_FALSE(removed);
  EXPECT_TRUE(mentions(removed.error().detail, "still listed"));
  EXPECT_GE(edge.clock - start, 500ms);
  EXPECT_LE(edge.clock - start, 600ms);
}

TEST_F(WindowsNative, RemoveFailsUnlessTheDriverCompletedIt) {
  for (const auto delivery : { edge_delivery_e::not_sent, edge_delivery_e::uncertain }) {
    edge.on_destroy = [=] { return dt::edge_send_t { delivery, "transport" }; };
    const auto removed = remove();
    ASSERT_FALSE(removed);
    EXPECT_TRUE(mentions(removed.error().detail, "transport"));
  }
  EXPECT_EQ(edge.snapshots, 0);
}

// --- Fields -------------------------------------------------------------------------

TEST_F(WindowsNative, FieldWritesAreRefused) {
  std::optional<dt::result_t<void>> applied;
  with_capability(dt::command_kind_e::native_restore, [&](const auto &capability) {
    dt::managed_values_t values;
    values.position = dt::position_t { 1, 2 };
    applied = platform.apply_fields(capability, dt::os_display_id_t { "os-B" }, values);
  });
  ASSERT_TRUE(applied);
  ASSERT_FALSE(*applied);
  EXPECT_EQ(edge.snapshots, 0);
  EXPECT_EQ(edge.destroys, 0);
  EXPECT_TRUE(edge.creates.empty());
}

// --- Head mode change (slice A, provisional pending G1 hardware) ---------------------

namespace {
  display_device::display_mode_t
  mode(unsigned width, unsigned height, unsigned numerator = 60, unsigned denominator = 1) {
    return { { width, height }, { numerator, denominator } };
  }

  ::testing::AssertionResult
  fails_with(const dt::result_t<void> &result, dt::error_code_e code, const std::string &needle) {
    if (result) return ::testing::AssertionFailure() << "the mode change succeeded";
    if (result.error().code != code) {
      return ::testing::AssertionFailure() << "wrong error code " << static_cast<int>(result.error().code) << ": " << result.error().detail;
    }
    return mentions(result.error().detail, needle);
  }
}  // namespace

TEST_F(WindowsNative, ChangeModeAppliesAListedModeThroughTheOsOnly) {
  const auto head = created_head();
  ASSERT_TRUE(change(head, mode(1280, 720)));
  EXPECT_TRUE(edge.plans.empty());
  ASSERT_EQ(edge.mode_sets.size(), 1u);
  EXPECT_EQ(edge.mode_sets[0].first, dt::os_display_id_t { "vdd-1" });
  EXPECT_EQ(the_head().layout->mode.resolution.width, 1280u);
  EXPECT_EQ(the_head().id, dt::os_display_id_t { "vdd-1" });
}

TEST_F(WindowsNative, ChangeModeListsAnUnofferedModeFirstAndFollowsTheRecreatedHead) {
  const auto head = created_head();
  ASSERT_TRUE(change(head, mode(2560, 1440)));
  ASSERT_EQ(edge.plans.size(), 1u);
  EXPECT_EQ(edge.plans[0], (std::vector<dt::edge_mode_t> { { 2560, 1440, 60 }, { 1920, 1080, 60 }, { 1280, 720, 60 } }));
  ASSERT_EQ(edge.mode_sets.size(), 1u);
  EXPECT_EQ(edge.mode_sets[0].first, dt::os_display_id_t { "vdd-r1" });
  EXPECT_EQ(the_head().layout->mode.resolution.width, 2560u);
  EXPECT_EQ(the_head().layout->mode.resolution.height, 1440u);
  EXPECT_EQ(the_head().container, std::optional { id<dt::head_guid_t>(0x71).value });
}

TEST_F(WindowsNative, ChangeModeRoundsTheRefreshRateForTheDriverList) {
  const auto head = created_head();
  ASSERT_TRUE(change(head, mode(2560, 1440, 60000, 1001)));
  ASSERT_EQ(edge.plans.size(), 1u);
  EXPECT_EQ(edge.plans[0].front(), (dt::edge_mode_t { 2560, 1440, 60 }));
  ASSERT_EQ(edge.mode_sets.size(), 1u);
  EXPECT_EQ(edge.mode_sets[0].second.refresh_rate.numerator, 60000u);
  EXPECT_EQ(edge.mode_sets[0].second.refresh_rate.denominator, 1001u);
}

TEST_F(WindowsNative, ChangeModeKeepsTheWholeDriverListAndDropsDuplicates) {
  const auto head = created_head();
  edge.driver_modes = { { 1920, 1080, 60 }, { 1280, 720, 60 }, { 1280, 720, 60 } };
  ASSERT_TRUE(change(head, mode(2560, 1440)));
  ASSERT_EQ(edge.plans.size(), 1u);
  EXPECT_EQ(edge.plans[0], (std::vector<dt::edge_mode_t> { { 2560, 1440, 60 }, { 1920, 1080, 60 }, { 1280, 720, 60 } }));
}

TEST_F(WindowsNative, ChangeModeRefusesToEvictFromAFullDriverList) {
  const auto head = created_head();
  edge.driver_modes.clear();
  for (unsigned i = 0; i < dt::max_driver_modes; ++i) edge.driver_modes.push_back({ 800 + i, 600, 60 });
  EXPECT_TRUE(fails_with(change(head, mode(2560, 1440)), dt::error_code_e::resource_unavailable, "mode list is full"));
  EXPECT_TRUE(edge.plans.empty());
  EXPECT_TRUE(edge.mode_sets.empty());
  edge.driver_modes.pop_back();
  ASSERT_TRUE(change(head, mode(2560, 1440)));
  ASSERT_EQ(edge.plans.size(), 1u);
  EXPECT_EQ(edge.plans[0].size(), dt::max_driver_modes);
}

TEST_F(WindowsNative, ChangeModeThatWasNeverSentIsDefinite) {
  const auto head = created_head();
  edge.on_set_mode = [](const auto &, const auto &) { return dt::edge_send_t { edge_delivery_e::not_sent, "no capability" }; };
  EXPECT_TRUE(fails_with(change(head, mode(1280, 720)), dt::error_code_e::resource_unavailable, "no capability"));
}

TEST_F(WindowsNative, ChangeModeThatShowsAnotherRefreshRateIsUnknown) {
  const auto head = created_head();
  edge.on_set_mode = [&](const auto &display, const auto &requested) {
    for (auto &listed : edge.displays) {
      if (listed.id == display && listed.layout) listed.layout->mode = { requested.resolution, { 120, 1 } };
    }
    return dt::edge_send_t { edge_delivery_e::delivered, {} };
  };
  EXPECT_TRUE(fails_with(change(head, mode(1280, 720)), dt::error_code_e::unknown_applied_state, "does not show 1280x720@60"));
}

TEST_F(WindowsNative, ChangeModeListIsGatedOnTheDriverVersion) {
  const auto head = created_head();
  edge.version = "99.0.0.1";
  EXPECT_TRUE(fails_with(change(head, mode(2560, 1440)), dt::error_code_e::resource_unavailable, "not a tagged ZakoVDD release"));
  EXPECT_TRUE(edge.plans.empty());
  EXPECT_TRUE(edge.mode_sets.empty());
  edge.version.reset();
  EXPECT_TRUE(fails_with(change(head, mode(2560, 1440)), dt::error_code_e::resource_unavailable, "could not be read"));
  EXPECT_TRUE(edge.mode_sets.empty());
}

TEST_F(WindowsNative, ChangeModeListDeliveryMapsToTheContract) {
  const auto head = created_head();
  edge.on_set_modes = [](const auto &) { return dt::edge_send_t { edge_delivery_e::not_sent, "no control interface" }; };
  EXPECT_TRUE(fails_with(change(head, mode(2560, 1440)), dt::error_code_e::resource_unavailable, "no control interface"));
  edge.on_set_modes = [](const auto &) { return dt::edge_send_t { edge_delivery_e::uncertain, "ioctl timed out" }; };
  EXPECT_TRUE(fails_with(change(head, mode(2560, 1440)), dt::error_code_e::unknown_applied_state, "ioctl timed out"));
  edge.on_set_modes = [](const auto &) { return dt::edge_send_t { edge_delivery_e::delivered, {} }; };
  EXPECT_TRUE(fails_with(change(head, mode(2560, 1440)), dt::error_code_e::unknown_applied_state, "does not offer 2560x1440@60"));
  EXPECT_TRUE(edge.mode_sets.empty());
}

TEST_F(WindowsNative, ChangeModeThatTheOsDoesNotShowIsUnknown) {
  const auto head = created_head();
  edge.on_set_mode = [](const auto &, const auto &) { return dt::edge_send_t { edge_delivery_e::uncertain, "SetDisplayConfig failed" }; };
  EXPECT_TRUE(fails_with(change(head, mode(1280, 720)), dt::error_code_e::unknown_applied_state, "SetDisplayConfig failed"));
  edge.on_set_mode = [](const auto &, const auto &) { return dt::edge_send_t { edge_delivery_e::delivered, {} }; };
  EXPECT_TRUE(fails_with(change(head, mode(1280, 720)), dt::error_code_e::unknown_applied_state, "does not show 1280x720@60"));
  EXPECT_EQ(the_head().layout->mode.resolution.width, 1920u);
}

TEST_F(WindowsNative, ChangeModeNeedsReadableModesAndAHeadIdentity) {
  const auto head = created_head();
  edge.modes_unreadable = true;
  EXPECT_TRUE(fails_with(change(head, mode(1280, 720)), dt::error_code_e::resource_unavailable, "could not be read"));
  edge.modes_unreadable = false;
  auto anonymous = head;
  anonymous.head.reset();
  EXPECT_TRUE(fails_with(change(anonymous, mode(1280, 720)), dt::error_code_e::missing_identity, "not a virtual display head"));
  EXPECT_TRUE(fails_with(change(head, mode(0, 720)), dt::error_code_e::unsupported_mode, "empty"));
  EXPECT_TRUE(edge.plans.empty());
  EXPECT_TRUE(edge.mode_sets.empty());
}

TEST_F(WindowsNative, ChangeModeIsOnlyForAManagedReconfigure) {
  const auto head = created_head();
  for (const auto kind : { dt::command_kind_e::reconfigure, dt::command_kind_e::launch_prepare }) {
    with_capability(kind, [&](const auto &capability) {
      EXPECT_THROW((void) platform.change_head_mode(capability, head, mode(2560, 1440)), dt::capability_error_t);
    });
  }
  EXPECT_TRUE(edge.plans.empty());
  EXPECT_TRUE(edge.mode_sets.empty());
}

TEST_F(WindowsNative, TheExecutorResizesItsHeadAcrossARecreationAndStillRemovesIt) {
  memory_store_t store;
  dt::native_executor_t executor { store, platform, { next_uuid, 2, driver() } };
  dt::native_outcome_t prepared;
  with_capability(dt::command_kind_e::launch_prepare, [&](const auto &capability) {
    prepared = executor.prepare(capability, { dt::native_profile_t::source_e::virtual_head, {} });
  }, true);
  ASSERT_EQ(prepared.verdict, native_verdict_e::ok) << prepared.reason;
  dt::mode_change_outcome_t changed;
  with_capability(dt::command_kind_e::reconfigure, [&](const auto &capability) {
    changed = executor.change_mode(capability, { 2560, 1440 });
  }, true);
  ASSERT_EQ(changed.verdict, dt::mode_change_verdict_e::applied) << changed.reason;
  EXPECT_EQ(changed.display, dt::os_display_id_t { "vdd-r1" });
  dt::native_outcome_t restored;
  with_capability(dt::command_kind_e::native_restore, [&](const auto &capability) { restored = executor.restore(capability); }, true);
  EXPECT_EQ(restored.verdict, native_verdict_e::ok) << restored.reason;
  EXPECT_EQ(edge.destroys, 1);
  EXPECT_FALSE(store.bytes);
}

TEST_F(WindowsNative, TheOptionsMustNameADriverAndAVirtualName) {
  auto no_driver = options();
  no_driver.driver = {};
  EXPECT_THROW((dt::windows_native_platform_t { edge, no_driver }), std::invalid_argument);
  auto no_name = options();
  no_name.virtual_name.clear();
  EXPECT_THROW((dt::windows_native_platform_t { edge, no_name }), std::invalid_argument);
  auto no_poll = options();
  no_poll.poll = 0ms;
  EXPECT_THROW((dt::windows_native_platform_t { edge, no_poll }), std::invalid_argument);
}

// --- Executor over the Windows platform ----------------------------------------------

TEST_F(WindowsNative, TheExecutorOwnsAndRemovesOnlyTheHeadCarryingItsIntent) {
  memory_store_t store;
  dt::native_executor_t executor { store, platform, { next_uuid, 2, driver() } };
  dt::native_outcome_t prepared;
  with_capability(dt::command_kind_e::launch_prepare, [&](const auto &capability) {
    prepared = executor.prepare(capability, { dt::native_profile_t::source_e::virtual_head, {} });
  });
  ASSERT_EQ(prepared.verdict, native_verdict_e::ok) << prepared.reason;
  ASSERT_TRUE(prepared.target);
  EXPECT_EQ(prepared.target->display, dt::os_display_id_t { "vdd-1" });
  EXPECT_TRUE(prepared.target->virtual_head);
  ASSERT_EQ(edge.creates.size(), 1u);
  const auto record = store.record();
  ASSERT_TRUE(record);
  ASSERT_EQ(record->owned_heads.size(), 1u);
  EXPECT_EQ(record->owned_heads.front().identity, (dt::head_identity_t { driver(), edge.creates.front(), dt::head_incarnation_t { 501 } }));

  dt::native_outcome_t restored;
  with_capability(dt::command_kind_e::native_restore, [&](const auto &capability) { restored = executor.restore(capability); });
  EXPECT_EQ(restored.verdict, native_verdict_e::ok) << restored.reason;
  EXPECT_EQ(edge.destroys, 1);
  EXPECT_FALSE(store.bytes);
}

TEST_F(WindowsNative, TheExecutorLeavesAForeignHeadAlone) {
  edge.on_create = [&](const dt::head_guid_t &) {
    edge.displays.push_back(zako_head("vdd-foreign", id<dt::head_guid_t>(0x5F).value, 9));
    return dt::edge_send_t { edge_delivery_e::delivered, {} };
  };
  memory_store_t store;
  dt::native_executor_t executor { store, platform, { next_uuid, 2, driver() } };
  dt::native_outcome_t prepared;
  with_capability(dt::command_kind_e::launch_prepare, [&](const auto &capability) {
    prepared = executor.prepare(capability, { dt::native_profile_t::source_e::virtual_head, {} });
  });
  EXPECT_EQ(prepared.verdict, native_verdict_e::conflict) << prepared.reason;
  EXPECT_TRUE(mentions(prepared.reason, "identity"));
  EXPECT_EQ(edge.destroys, 0);
  ASSERT_TRUE(store.record());
  EXPECT_TRUE(store.record()->owned_heads.empty());
}

// --- Head activation (A4) -------------------------------------------------------------

namespace {
  dt::result_t<void>
  activate_with(dt::windows_native_platform_t &platform, const dt::observed_display_t &head, dt::command_kind_e kind = dt::command_kind_e::launch_prepare) {
    std::optional<dt::result_t<void>> result;
    with_capability(kind, [&](const auto &capability) { result = platform.activate_head(capability, head); }, true);
    return *result;
  }

  bool
  on_desktop(const dt::edge_display_t &display) {
    return display.layout && !display.info.display_name.empty() && display.info.device_state != state_e::inactive;
  }
}  // namespace

TEST_F(WindowsNative, ActivateAddsAHeadThatArrivedOffTheDesktopAndWaitsForIt) {
  edge.create_inactive = true;
  const auto head = created_head();
  ASSERT_FALSE(on_desktop(the_head()));
  ASSERT_TRUE(activate_with(platform, head));
  ASSERT_EQ(edge.activations.size(), 1u);
  EXPECT_EQ(edge.activations[0], dt::os_display_id_t { "vdd-1" });
  EXPECT_TRUE(on_desktop(the_head()));
  const auto inventory = platform.inventory();
  ASSERT_TRUE(find(inventory, "vdd-1"));
  EXPECT_EQ(find(inventory, "vdd-1")->info.display_name, "\\\\.\\DISPLAY-vdd-1");
}

TEST_F(WindowsNative, ActivateLeavesAHeadOnTheDesktopAlone) {
  const auto head = created_head();
  ASSERT_TRUE(activate_with(platform, head));
  EXPECT_TRUE(edge.activations.empty());
}

TEST_F(WindowsNative, ActivateDeliveryMapsToTheContract) {
  edge.create_inactive = true;
  const auto head = created_head();
  edge.on_activate = [](const auto &) { return dt::edge_send_t { edge_delivery_e::not_sent, "no session" }; };
  EXPECT_TRUE(fails_with(activate_with(platform, head), dt::error_code_e::resource_unavailable, "no session"));
  edge.on_activate = [](const auto &) { return dt::edge_send_t { edge_delivery_e::uncertain, "SetDisplayConfig failed" }; };
  EXPECT_TRUE(fails_with(activate_with(platform, head), dt::error_code_e::unknown_applied_state, "SetDisplayConfig failed"));
  edge.on_activate = [](const auto &) { return dt::edge_send_t { edge_delivery_e::delivered, {} }; };
  const auto sleeps = edge.sleeps;
  EXPECT_TRUE(fails_with(activate_with(platform, head), dt::error_code_e::unknown_applied_state, "not on the desktop"));
  EXPECT_GT(edge.sleeps, sleeps);
  EXPECT_FALSE(on_desktop(the_head()));
}

TEST_F(WindowsNative, ActivateNeedsAListedHeadIdentity) {
  edge.create_inactive = true;
  const auto head = created_head();
  auto anonymous = head;
  anonymous.head.reset();
  EXPECT_TRUE(fails_with(activate_with(platform, anonymous), dt::error_code_e::missing_identity, "not a virtual display head"));
  edge.drop_heads();
  EXPECT_TRUE(fails_with(activate_with(platform, head), dt::error_code_e::unknown_applied_state, "no longer listed"));
  EXPECT_TRUE(edge.activations.empty());
}

TEST_F(WindowsNative, ActivateIsOnlyForAManagedPrepareOrReconfigure) {
  edge.create_inactive = true;
  const auto head = created_head();
  for (const auto kind : { dt::command_kind_e::launch_prepare, dt::command_kind_e::reconfigure }) {
    with_capability(kind, [&](const auto &capability) {
      EXPECT_THROW((void) platform.activate_head(capability, head), dt::capability_error_t);
    });
  }
  with_capability(dt::command_kind_e::native_restore, [&](const auto &capability) {
    EXPECT_THROW((void) platform.activate_head(capability, head), dt::capability_error_t);
  }, true);
  EXPECT_TRUE(edge.activations.empty());
  EXPECT_TRUE(activate_with(platform, head, dt::command_kind_e::reconfigure));
}

TEST_F(WindowsNative, ChangeModeAddsAHeadRecreatedOffTheDesktopBackBeforeApplyingTheMode) {
  const auto head = created_head();
  edge.recreate_inactive = true;
  ASSERT_TRUE(change(head, mode(2560, 1440)));
  ASSERT_EQ(edge.activations.size(), 1u);
  EXPECT_EQ(edge.activations[0], dt::os_display_id_t { "vdd-r1" });
  ASSERT_EQ(edge.mode_sets.size(), 1u);
  EXPECT_EQ(edge.mode_sets[0].first, dt::os_display_id_t { "vdd-r1" });
  EXPECT_TRUE(on_desktop(the_head()));
  EXPECT_EQ(the_head().layout->mode.resolution.width, 2560u);
}

TEST_F(WindowsNative, ChangeModeStopsWhenARecreatedHeadCannotBeAddedBack) {
  const auto head = created_head();
  edge.recreate_inactive = true;
  edge.on_activate = [](const auto &) { return dt::edge_send_t { edge_delivery_e::uncertain, "SetDisplayConfig failed" }; };
  EXPECT_TRUE(fails_with(change(head, mode(2560, 1440)), dt::error_code_e::unknown_applied_state, "SetDisplayConfig failed"));
  EXPECT_TRUE(edge.mode_sets.empty());
}

TEST_F(WindowsNative, TheExecutorAddsAColdCreatedHeadToTheDesktopAndStillRemovesIt) {
  edge.create_inactive = true;
  memory_store_t store;
  dt::native_executor_t executor { store, platform, { next_uuid, 2, driver() } };
  dt::native_outcome_t prepared;
  with_capability(dt::command_kind_e::launch_prepare, [&](const auto &capability) {
    prepared = executor.prepare(capability, { dt::native_profile_t::source_e::virtual_head, {} });
  }, true);
  ASSERT_EQ(prepared.verdict, native_verdict_e::ok) << prepared.reason;
  ASSERT_TRUE(prepared.target);
  EXPECT_EQ(prepared.target->display, dt::os_display_id_t { "vdd-1" });
  ASSERT_EQ(edge.activations.size(), 1u);
  EXPECT_TRUE(on_desktop(the_head()));
  const auto record = store.record();
  ASSERT_TRUE(record);
  ASSERT_EQ(record->owned_heads.size(), 1u);
  ASSERT_TRUE(record->steps.back().observed);
  const auto *journaled = find(*record->steps.back().observed, "vdd-1");
  ASSERT_TRUE(journaled);
  EXPECT_FALSE(journaled->info.display_name.empty());

  dt::native_outcome_t restored;
  with_capability(dt::command_kind_e::native_restore, [&](const auto &capability) { restored = executor.restore(capability); }, true);
  EXPECT_EQ(restored.verdict, native_verdict_e::ok) << restored.reason;
  EXPECT_EQ(edge.destroys, 1);
  EXPECT_FALSE(store.bytes);
}
// --- Placing displays (kit-3 run 20261106-193211) ---------------------------------------

namespace {
  dt::result_t<void>
  arrange_with(dt::windows_native_platform_t &platform, const std::vector<dt::placement_t> &plan, dt::command_kind_e kind = dt::command_kind_e::launch_prepare,
    const std::vector<dt::os_display_id_t> &off = {}) {
    std::optional<dt::result_t<void>> result;
    with_capability(kind, [&](const auto &capability) { result = platform.arrange(capability, plan, off); }, true);
    return *result;
  }

  const std::vector<dt::placement_t> swapped {
    { dt::os_display_id_t { "os-B" }, { 0, 0 } },
    { dt::os_display_id_t { "os-A" }, { 1920, 0 } },
  };
}  // namespace

TEST_F(WindowsNative, ArrangeMovesEveryDisplayInOneWriteAndWaitsForThem) {
  ASSERT_TRUE(arrange_with(platform, swapped));
  ASSERT_EQ(edge.arrangements.size(), 1u);
  EXPECT_EQ(edge.arrangements[0], swapped);
  const auto inventory = platform.inventory();
  EXPECT_EQ(find(inventory, "os-B")->layout->position, (dt::position_t { 0, 0 }));
  EXPECT_EQ(find(inventory, "os-A")->layout->position, (dt::position_t { 1920, 0 }));
}

TEST_F(WindowsNative, ArrangeOfNothingWritesNothing) {
  ASSERT_TRUE(arrange_with(platform, {}));
  EXPECT_TRUE(edge.arrangements.empty());
}

TEST_F(WindowsNative, ArrangeDeliveryMapsToTheContract) {
  edge.on_arrange = [](const auto &) { return dt::edge_send_t { edge_delivery_e::not_sent, "a clone" }; };
  EXPECT_TRUE(fails_with(arrange_with(platform, swapped), dt::error_code_e::resource_unavailable, "a clone"));
  edge.on_arrange = [](const auto &) { return dt::edge_send_t { edge_delivery_e::uncertain, "SetDisplayConfig failed" }; };
  EXPECT_TRUE(fails_with(arrange_with(platform, swapped), dt::error_code_e::unknown_applied_state, "SetDisplayConfig failed"));
  edge.on_arrange = [](const auto &) { return dt::edge_send_t { edge_delivery_e::delivered, {} }; };
  const auto sleeps = edge.sleeps;
  EXPECT_TRUE(fails_with(arrange_with(platform, swapped), dt::error_code_e::unknown_applied_state, "not at their places"));
  EXPECT_GT(edge.sleeps, sleeps);
}

TEST_F(WindowsNative, ArrangeWaitsForAnOffDesktopDisplayToBeAtItsPlace) {
  edge.on_arrange = [&](const auto &plan) {
    for (auto &display : edge.displays) {
      if (display.id.value == "os-B") {
        display.layout->position = plan[0].position;
        display.info.device_state = state_e::inactive;
      }
    }
    return dt::edge_send_t { edge_delivery_e::delivered, {} };
  };
  EXPECT_TRUE(fails_with(arrange_with(platform, { swapped[0] }), dt::error_code_e::unknown_applied_state, "not at their places"));
}

TEST_F(WindowsNative, ArrangeTakesTheOffDisplaysOffInTheSameWriteAndWaitsForThem) {
  const std::vector<dt::os_display_id_t> off { dt::os_display_id_t { "os-B" } };
  const std::vector<dt::placement_t> origin { { dt::os_display_id_t { "os-A" }, { 0, 0 } } };
  ASSERT_TRUE(arrange_with(platform, origin, dt::command_kind_e::native_restore, off));
  ASSERT_EQ(edge.arrangements.size(), 1u) << "one write";
  EXPECT_EQ(edge.offs[0], off);
  const auto inventory = platform.inventory();
  EXPECT_EQ(find(inventory, "os-B")->info.device_state, state_e::inactive);
  EXPECT_EQ(find(inventory, "os-A")->layout->position, (dt::position_t { 0, 0 }));
}

TEST_F(WindowsNative, ArrangeWaitsForAnOffDisplayToLeaveTheDesktop) {
  edge.on_arrange = [](const auto &) { return dt::edge_send_t { edge_delivery_e::delivered, {} }; };
  const std::vector<dt::placement_t> stay { { dt::os_display_id_t { "os-A" }, find(platform.inventory(), "os-A")->layout->position } };
  EXPECT_TRUE(fails_with(arrange_with(platform, stay, dt::command_kind_e::native_restore, { dt::os_display_id_t { "os-B" } }),
    dt::error_code_e::unknown_applied_state, "not at their places"));
}

TEST_F(WindowsNative, ArrangeOfOnlyAnOffDisplayIsAWrite) {
  ASSERT_TRUE(arrange_with(platform, {}, dt::command_kind_e::native_restore, { dt::os_display_id_t { "os-B" } }));
  EXPECT_EQ(edge.arrangements.size(), 1u);
}

TEST_F(WindowsNative, ArrangeIsOnlyForAPrepareOrRestore) {
  with_capability(dt::command_kind_e::reconfigure, [&](const auto &capability) {
    EXPECT_THROW((void) platform.arrange(capability, swapped), dt::capability_error_t);
  });
  EXPECT_TRUE(edge.arrangements.empty());
  EXPECT_TRUE(arrange_with(platform, swapped, dt::command_kind_e::native_restore));
}

// --- Slow head arrival (gh-all run 20261107-232556, Punktfunk-only desktop) ------------------

namespace {
  /** Production timing: the driver's defaults, as topology_runtime builds the platform. */
  dt::windows_native_options_t
  production_options() {
    return { driver(), zako };
  }

  /** Delivers each create at once but lists its head only `delay` later on the edge clock. */
  void
  arrive_late(fake_edge_t &edge, std::vector<std::chrono::milliseconds> delays, std::function<void(std::size_t, std::chrono::milliseconds)> meanwhile = {}) {
    struct pending_t {
      dt::head_guid_t intent;
      std::chrono::steady_clock::time_point due;
      std::chrono::steady_clock::time_point sent;
      bool listed {};
    };
    auto pending = std::make_shared<std::vector<pending_t>>();
    edge.on_create = [&edge, pending, delays](const dt::head_guid_t &intent) {
      const auto index = pending->size();
      const auto delay = index < delays.size() ? delays[index] : delays.back();
      pending->push_back({ intent, edge.clock + delay, edge.clock });
      return dt::edge_send_t { edge_delivery_e::delivered, {} };
    };
    edge.before_snapshot = [&edge, pending, meanwhile](int) {
      for (std::size_t index = 0; index < pending->size(); ++index) {
        auto &head = (*pending)[index];
        if (meanwhile) meanwhile(index, std::chrono::duration_cast<std::chrono::milliseconds>(edge.clock - head.sent));
        if (head.listed || edge.clock < head.due) continue;
        head.listed = true;
        edge.displays.push_back(zako_head("vdd-" + std::to_string(index + 1), head.intent.value, 500 + index + 1));
      }
    };
  }

  dt::native_outcome_t
  prepare_with(dt::native_executor_t &executor, std::uint8_t heads = 1) {
    dt::native_outcome_t prepared;
    with_capability(dt::command_kind_e::launch_prepare, [&](const auto &capability) {
      prepared = executor.prepare(capability, { dt::native_profile_t::source_e::virtual_head, {}, heads });
    }, true);
    return prepared;
  }

  dt::native_outcome_t
  restore_with(dt::native_executor_t &executor) {
    dt::native_outcome_t restored;
    with_capability(dt::command_kind_e::native_restore, [&](const auto &capability) { restored = executor.restore(capability); }, true);
    return restored;
  }
}  // namespace

TEST_F(WindowsNative, AHeadListedSixSecondsAfterItsCreationIsWaitedForAndOwned) {
  dt::windows_native_platform_t slow { edge, production_options() };
  arrive_late(edge, { 6000ms });
  memory_store_t store;
  dt::native_executor_t executor { store, slow, { next_uuid, 2, driver() } };
  const auto prepared = prepare_with(executor);
  ASSERT_EQ(prepared.verdict, native_verdict_e::ok) << prepared.reason;
  ASSERT_TRUE(prepared.target);
  EXPECT_EQ(prepared.target->display, dt::os_display_id_t { "vdd-1" });
  EXPECT_EQ(restore_with(executor).verdict, native_verdict_e::ok);
  EXPECT_EQ(edge.destroys, 1);
  EXPECT_FALSE(store.bytes);
}

TEST_F(WindowsNative, AHeadThatIsNeverListedIsRemovedAndRefusedNotLeftUnresolved) {
  dt::windows_native_platform_t slow { edge, production_options() };
  arrive_late(edge, { std::chrono::hours { 1 } });
  memory_store_t store;
  dt::native_executor_t executor { store, slow, { next_uuid, 2, driver() } };
  const auto prepared = prepare_with(executor);
  EXPECT_EQ(prepared.verdict, native_verdict_e::refused) << prepared.reason;
  EXPECT_TRUE(mentions(prepared.reason, "no virtual display appeared")) << prepared.reason;
  EXPECT_EQ(edge.destroys, 1) << "the driver's own list is cleared, so the late head never lands";
  EXPECT_FALSE(store.bytes);
  EXPECT_FALSE(executor.health().in_conflict);
}

TEST_F(WindowsNative, AFirstHeadReEnumeratedWhileTheSecondIsLateIsFollowedNotAConflict) {
  dt::windows_native_platform_t slow { edge, production_options() };
  bool re_enumerated = false;
  arrive_late(edge, { 0ms, 6000ms }, [&](std::size_t index, std::chrono::milliseconds since) {
    if (index != 1 || re_enumerated || since < 2000ms) return;
    for (auto &display : edge.displays) {
      if (display.id != dt::os_display_id_t { "vdd-1" }) continue;
      display.id = dt::os_display_id_t { "vdd-1b" };
      display.info.display_name = "\\\\.\\DISPLAY-vdd-1b";
      display.arrival += 100;
      re_enumerated = true;
    }
  });
  memory_store_t store;
  dt::native_executor_t executor { store, slow, { next_uuid, 2, driver() } };
  const auto prepared = prepare_with(executor, 2);
  ASSERT_EQ(prepared.verdict, native_verdict_e::ok) << prepared.reason;
  ASSERT_TRUE(re_enumerated);
  ASSERT_EQ(prepared.targets.size(), 2u);
  EXPECT_EQ(prepared.targets[0].display, dt::os_display_id_t { "vdd-1b" });
  EXPECT_EQ(prepared.targets[1].display, dt::os_display_id_t { "vdd-2" });
  EXPECT_EQ(restore_with(executor).verdict, native_verdict_e::ok);
  EXPECT_EQ(edge.destroys, 1);
  EXPECT_FALSE(store.bytes);
}

TEST_F(WindowsNative, CreationNotesWhenItsHeadWasListedAndSettled) {
  auto timed = production_options();
  std::vector<std::string> notes;
  timed.note = [&](const std::string &line) { notes.push_back(line); };
  dt::windows_native_platform_t slow { edge, timed };
  arrive_late(edge, { 6000ms });
  std::optional<dt::result_t<delivery_e>> created;
  with_capability(dt::command_kind_e::launch_prepare, [&](const auto &capability) { created = slow.create_head(capability, id<dt::head_guid_t>(0x51)); });
  ASSERT_TRUE(created && *created);
  ASSERT_EQ(notes.size(), 1u);
  EXPECT_TRUE(mentions(notes[0], "listed 6000 ms")) << notes[0];
  EXPECT_TRUE(mentions(notes[0], "settled")) << notes[0];
}

TEST_F(WindowsNative, AFirstHeadThatVanishesAsTheSecondIsListedIsWaitedForUntilTheHeadsSettle) {
  dt::windows_native_platform_t slow { edge, production_options() };
  std::optional<dt::edge_display_t> away;
  bool back = false;
  arrive_late(edge, { 0ms, 6000ms }, [&](std::size_t index, std::chrono::milliseconds since) {
    if (index != 1) return;
    if (!away && since >= 6000ms) {
      const auto first = std::ranges::find(edge.displays, dt::os_display_id_t { "vdd-1" }, &dt::edge_display_t::id);
      if (first == edge.displays.end()) return;
      away = *first;
      edge.displays.erase(first);
    }
    if (away && !back && since >= 6500ms) {
      auto returned = *away;
      returned.id = dt::os_display_id_t { "vdd-1b" };
      returned.info.display_name = "\\\\.\\DISPLAY-vdd-1b";
      returned.arrival += 100;
      edge.displays.push_back(returned);
      back = true;
    }
  });
  memory_store_t store;
  dt::native_executor_t executor { store, slow, { next_uuid, 2, driver() } };
  const auto prepared = prepare_with(executor, 2);
  ASSERT_EQ(prepared.verdict, native_verdict_e::ok) << prepared.reason;
  ASSERT_TRUE(back);
  ASSERT_EQ(prepared.targets.size(), 2u);
  EXPECT_EQ(prepared.targets[0].display, dt::os_display_id_t { "vdd-1b" });
  EXPECT_EQ(restore_with(executor).verdict, native_verdict_e::ok);
  EXPECT_FALSE(store.bytes);
}

TEST_F(WindowsNative, AHeadThatLandsAfterItsRollbackIsNeverAdoptedByTheNextPrepare) {
  // Whether DESTROYALL also cancels a creation still on its way is a hardware unknown. This fake lets the
  // head land anyway, 40 s after it was asked for: the journal is already clear and nothing adopts it.
  dt::windows_native_platform_t slow { edge, production_options() };
  arrive_late(edge, { 40000ms });
  memory_store_t store;
  dt::native_executor_t executor { store, slow, { next_uuid, 2, driver() } };
  ASSERT_EQ(prepare_with(executor).verdict, native_verdict_e::refused);
  edge.sleep(20000ms);
  (void) slow.inventory();
  ASSERT_EQ(std::ranges::count(edge.displays, head_class_e::driver_head, &dt::edge_display_t::kind), 1);
  EXPECT_FALSE(store.bytes);
  EXPECT_FALSE(executor.health().in_conflict);
  EXPECT_EQ(restore_with(executor).verdict, native_verdict_e::ok);
  EXPECT_EQ(edge.destroys, 1) << "a restore with a clear journal removes nothing";
  const auto again = prepare_with(executor);
  EXPECT_EQ(again.verdict, native_verdict_e::refused) << again.reason;
  EXPECT_TRUE(mentions(again.reason, "already exists")) << again.reason;
  EXPECT_EQ(edge.creates.size(), 1u);
}

// --- ZakoVDD v0.18 slots (driver source CommandHandlers.cpp / MonitorLifecycle.cpp; gh-all runs 20261108-165856/-170253) ---

namespace {
  /**
   * ZakoVDD v0.18.2 as its source behaves: CREATEMONITOR puts the request's GUIDs on slots 0..count-1 (a slot without one
   * gets a random ContainerId), a create for an occupied slot is a silent success, and DESTROYMONITOR frees a slot only
   * when its departure succeeds.
   */
  struct zako_v018_t {
    fake_edge_t &edge;
    std::uint32_t count { 1 };
    bool departure_fails {};
    std::map<std::uint32_t, dt::head_guid_t> slots;
    std::uint8_t random {};

    void
    install() {
      edge.on_create = [this](const dt::head_guid_t &intent) {
        for (std::uint32_t slot = 0; slot < count; ++slot) {
          if (slots.contains(slot)) continue;
          const auto guid = slot == 0 ? intent : id<dt::head_guid_t>(static_cast<std::uint8_t>(0xA0 + ++random));
          slots.emplace(slot, guid);
          edge.displays.push_back(zako_head("vdd-slot" + std::to_string(slot) + "-" + std::to_string(edge.creates.size()), guid.value, 700 + edge.creates.size() * 10 + slot));
        }
        return dt::edge_send_t { edge_delivery_e::delivered, {} };
      };
      edge.on_destroy = [this] {
        if (!departure_fails) {
          slots.clear();
          edge.drop_heads();
        }
        return dt::edge_send_t { edge_delivery_e::delivered, {} };
      };
    }
  };

  dt::windows_native_options_t
  with_capacity(std::optional<std::uint32_t> capacity) {
    auto options = production_options();
    options.head_capacity = [capacity] { return capacity; };
    return options;
  }
}  // namespace

TEST(VddDisplayCount, IsReadAsTheDriverReadsIt) {
  const std::string installed =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n<vdd_settings>\r\n  <monitors>\r\n    <count>1</count>\r\n  </monitors>\r\n"
    "  <gpu>\r\n    <friendlyname>default</friendlyname>\r\n  </gpu>\r\n</vdd_settings>\r\n";
  EXPECT_EQ(dt::parse_vdd_display_count(installed), 1u);
  EXPECT_EQ(dt::parse_vdd_display_count("<monitors><count>2</count></monitors>"), 2u);
  EXPECT_EQ(dt::parse_vdd_display_count("<count> 3 </count>"), 3u);
  EXPECT_EQ(dt::parse_vdd_display_count("<count>2x</count>"), 2u) << "stoi stops at the first non-digit";
  EXPECT_EQ(dt::parse_vdd_display_count("<count>16</count>"), 16u);
  for (const auto *text : { "<count>abc</count>", "<count>0</count>", "<count>17</count>", "<count>-2</count>", "<count></count>", "<count>99999999999999999999</count>" }) {
    EXPECT_EQ(dt::parse_vdd_display_count(text), 1u) << text;
  }
  EXPECT_EQ(dt::parse_vdd_display_count("<count>1</count><count>4</count>"), 4u) << "the last element wins";
  EXPECT_EQ(dt::parse_vdd_display_count("<vdd_settings><gpu/></vdd_settings>"), std::nullopt);
  EXPECT_EQ(dt::parse_vdd_display_count(""), std::nullopt);
}

TEST_F(WindowsNative, AV018DriverWithOneSlotDropsTheSecondCreateSilently) {
  // Run 20261108-165856 with the count unknown: head 1 listed at once, head 2 never, a 30 s wait, then a rollback.
  zako_v018_t zako { edge };
  zako.install();
  dt::windows_native_platform_t slow { edge, with_capacity(std::nullopt) };
  memory_store_t store;
  dt::native_executor_t executor { store, slow, { next_uuid, 2, driver() } };
  const auto start = edge.clock;
  const auto prepared = prepare_with(executor, 2);
  EXPECT_EQ(prepared.verdict, native_verdict_e::refused) << prepared.reason;
  EXPECT_TRUE(mentions(prepared.reason, "no virtual display appeared")) << prepared.reason;
  EXPECT_EQ(edge.creates.size(), 2u);
  EXPECT_GE(edge.clock - start, 30000ms);
  EXPECT_EQ(edge.destroys, 1);
  EXPECT_TRUE(zako.slots.empty());
  EXPECT_FALSE(store.bytes);
}

TEST_F(WindowsNative, AGroupLargerThanTheDriversCountIsRefusedBeforeAnyCreate) {
  zako_v018_t zako { edge };
  zako.install();
  dt::windows_native_platform_t counted { edge, with_capacity(1) };
  memory_store_t store;
  dt::native_executor_t executor { store, counted, { next_uuid, 2, driver() } };
  const auto start = edge.clock;
  const auto prepared = prepare_with(executor, 2);
  EXPECT_EQ(prepared.verdict, native_verdict_e::refused);
  EXPECT_EQ(prepared.reason, "ZakoVDD is configured for 1 display(s); two-display groups need the ZakoVDD multi-monitor change");
  EXPECT_TRUE(edge.creates.empty());
  EXPECT_EQ(edge.destroys, 0);
  EXPECT_EQ(edge.clock, start);
  EXPECT_FALSE(store.bytes);
  EXPECT_FALSE(executor.health().in_conflict);
}

TEST_F(WindowsNative, ASingleHeadWithinTheDriversCountIsCreatedAsBefore) {
  zako_v018_t zako { edge };
  zako.install();
  dt::windows_native_platform_t counted { edge, with_capacity(1) };
  memory_store_t store;
  dt::native_executor_t executor { store, counted, { next_uuid, 2, driver() } };
  const auto prepared = prepare_with(executor);
  ASSERT_EQ(prepared.verdict, native_verdict_e::ok) << prepared.reason;
  EXPECT_EQ(restore_with(executor).verdict, native_verdict_e::ok);
  EXPECT_TRUE(zako.slots.empty());
}

TEST_F(WindowsNative, AGroupWithinTheDriversCountIsNotRefusedForIt) {
  // count 2 with v0.18: the first create already fills both slots, one with a random ContainerId, so the executor
  // still refuses the group - but on what it saw, not on the count.
  zako_v018_t zako { edge, 2 };
  zako.install();
  dt::windows_native_platform_t counted { edge, with_capacity(2) };
  memory_store_t store;
  dt::native_executor_t executor { store, counted, { next_uuid, 2, driver() } };
  const auto prepared = prepare_with(executor, 2);
  EXPECT_FALSE(mentions(prepared.reason, "is configured for")) << prepared.reason;
  EXPECT_EQ(edge.creates.size(), 1u);
}

TEST_F(WindowsNative, AFailedDepartureKeepsTheSlotAndIsNeverReportedAsRemoved) {
  zako_v018_t zako { edge };
  zako.install();
  dt::windows_native_platform_t counted { edge, with_capacity(1) };
  memory_store_t store;
  dt::native_executor_t executor { store, counted, { next_uuid, 2, driver() } };
  ASSERT_EQ(prepare_with(executor).verdict, native_verdict_e::ok);
  zako.departure_fails = true;
  const auto restored = restore_with(executor);
  EXPECT_EQ(restored.verdict, native_verdict_e::conflict) << restored.reason;
  EXPECT_EQ(zako.slots.size(), 1u);
  EXPECT_TRUE(store.bytes);
  const auto again = prepare_with(executor);
  EXPECT_NE(again.verdict, native_verdict_e::ok) << again.reason;
  EXPECT_EQ(edge.creates.size(), 1u) << "no create is sent into an occupied slot";
}

TEST_F(WindowsNative, ANotListedNoteSaysWhetherTheDriverCreatedTheMonitorOrWindowsHidIt) {
  const auto run = [&](std::function<void(fake_edge_t &, const dt::head_guid_t &)> created) {
    fake_edge_t local;
    local.on_create = [&](const dt::head_guid_t &intent) {
      created(local, intent);
      return dt::edge_send_t { edge_delivery_e::delivered, {} };
    };
    auto timed = production_options();
    std::vector<std::string> notes;
    timed.note = [&](const std::string &line) { notes.push_back(line); };
    dt::windows_native_platform_t local_platform { local, timed };
    with_capability(dt::command_kind_e::launch_prepare, [&](const auto &capability) { (void) local_platform.create_head(capability, id<dt::head_guid_t>(0x61)); });
    EXPECT_EQ(local.described.size(), 1u);
    return notes.size() == 1 ? notes[0] : std::string {};
  };
  const auto hidden = run([](fake_edge_t &e, const dt::head_guid_t &intent) { e.hidden_monitors.push_back(intent); });
  EXPECT_TRUE(mentions(hidden, "the driver created its monitor, but Windows shows no display path for it")) << hidden;
  EXPECT_TRUE(mentions(hidden, "[fake monitors=1]")) << hidden;
  const auto dropped = run([](fake_edge_t &, const dt::head_guid_t &) {});
  EXPECT_TRUE(mentions(dropped, "the driver did not create a monitor for it")) << dropped;
  const auto unknown = run([](fake_edge_t &e, const dt::head_guid_t &) { e.describes = false; });
  EXPECT_TRUE(mentions(unknown, "could not be determined")) << unknown;
}

TEST(MissingHeadExplanation, NamesADisabledMonitorAndTheProgramThatDoesIt) {
  EXPECT_EQ(dt::missing_head_explanation({}), "");
  EXPECT_NE(dt::missing_head_explanation({ false, std::nullopt, "" }).find("did not create a monitor"), std::string::npos);
  EXPECT_NE(dt::missing_head_explanation({ true, std::nullopt, "" }).find("no display path"), std::string::npos);
  EXPECT_NE(dt::missing_head_explanation({ true, 28u, "" }).find("device problem 28"), std::string::npos);
  const auto disabled = dt::missing_head_explanation({ true, 22u, "" });
  EXPECT_NE(disabled.find("CM_PROB_DISABLED"), std::string::npos) << disabled;
  EXPECT_NE(disabled.find("pnp_disable_monitors"), std::string::npos) << disabled;
}

TEST_F(WindowsNative, AHeadWhoseMonitorIsDisabledIsRefusedNamingTheDisabledDevice) {
  // gh-all run 20261109-174516, stream stage: the monitor instance arrived already disabled (problem 22).
  edge.on_create = [&](const dt::head_guid_t &intent) {
    edge.disabled_monitors.push_back(intent);
    return dt::edge_send_t { edge_delivery_e::delivered, {} };
  };
  memory_store_t store;
  dt::native_executor_t executor { store, platform, { next_uuid, 2, driver() } };
  const auto prepared = prepare_with(executor);
  EXPECT_EQ(prepared.verdict, native_verdict_e::refused) << prepared.reason;
  EXPECT_TRUE(mentions(prepared.reason, "no virtual display appeared: its monitor device is disabled (CM_PROB_DISABLED)")) << prepared.reason;
  EXPECT_EQ(edge.destroys, 1);
  EXPECT_FALSE(store.bytes);
}
