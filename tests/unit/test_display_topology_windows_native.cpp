/**
 * @file tests/unit/test_display_topology_windows_native.cpp
 * @brief The P1 Windows native platform against a fake CCD/SetupAPI/IOCTL edge.
 */
#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

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
      displays.push_back(zako_head("vdd-" + std::to_string(creates.size()), intent.value, 500 + creates.size()));
      return { edge_delivery_e::delivered, {} };
    }

    dt::edge_send_t
    send_destroy_all(const dt::mutation_capability_t &) override {
      ++destroys;
      if (on_destroy) return on_destroy();
      drop_heads();
      return { edge_delivery_e::delivered, {} };
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
  };

  dt::windows_native_options_t
  options() {
    return { driver(), zako, 500ms, 100ms };
  }

  void
  with_capability(dt::command_kind_e kind, const std::function<void(const dt::mutation_capability_t &)> &body) {
    dt::coordinator_options_t coordinator_options;
    coordinator_options.probes.startup_restore_quiescent = [] { return true; };
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
