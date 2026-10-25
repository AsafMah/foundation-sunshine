/**
 * @file tests/unit/test_display_topology_native.cpp
 * @brief P1 managed (native) path against fakes: profile parsing, executor, recovery and the journal store.
 */
#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "display_topology_fixtures.h"
#include "src/display_device/topology_capability.h"
#include "src/display_device/topology_coordinator.h"
#include "src/display_device/topology_journal.h"
#include "src/display_device/topology_journal_store.h"
#include "src/display_device/topology_native.h"

namespace {
  namespace dt = display_device::topology;
  using display_topology_test::error_is;
  using display_topology_test::id;
  using dt::native_verdict_e;
  using state_e = display_device::device_state_e;
  using step_e = dt::journal_step_state_e;

  ::testing::AssertionResult
  mentions(const std::string &text, const std::string &needle) {
    if (text.find(needle) != std::string::npos) {
      return ::testing::AssertionSuccess();
    }
    return ::testing::AssertionFailure() << "\"" << text << "\" does not mention \"" << needle << "\"";
  }

  ::testing::AssertionResult
  verdict_is(const dt::native_outcome_t &outcome, native_verdict_e expected) {
    if (outcome.verdict == expected) {
      return ::testing::AssertionSuccess();
    }
    return ::testing::AssertionFailure() << "expected " << dt::to_string(expected) << ", got "
                                         << dt::to_string(outcome.verdict) << ": " << outcome.reason;
  }

  ::testing::AssertionResult
  healthy(const dt::native_health_t &health) {
    if (!health.journal_present && !health.in_conflict && health.reason.empty()) {
      return ::testing::AssertionSuccess();
    }
    return ::testing::AssertionFailure() << "expected clear health, got journal_present=" << health.journal_present
                                         << " in_conflict=" << health.in_conflict << " reason=\"" << health.reason << "\"";
  }

  ::testing::AssertionResult
  in_conflict(const dt::native_health_t &health) {
    if (health.journal_present && health.in_conflict && !health.reason.empty()) {
      return ::testing::AssertionSuccess();
    }
    return ::testing::AssertionFailure() << "expected In conflict, got journal_present=" << health.journal_present
                                         << " in_conflict=" << health.in_conflict << " reason=\"" << health.reason << "\"";
  }

  /** Unique across every executor in the process, so owners never collide by accident. */
  dt::uuid_bytes_t
  next_uuid() {
    static std::atomic<std::uint32_t> counter { 0 };
    const auto value = ++counter;
    dt::uuid_bytes_t bytes { 0x4e };
    bytes[12] = static_cast<std::uint8_t>(value >> 24);
    bytes[13] = static_cast<std::uint8_t>(value >> 16);
    bytes[14] = static_cast<std::uint8_t>(value >> 8);
    bytes[15] = static_cast<std::uint8_t>(value);
    return bytes;
  }

  dt::driver_instance_id_t
  driver() {
    return id<dt::driver_instance_id_t>(0x60);
  }

  display_device::display_mode_t
  mode(unsigned width, unsigned height) {
    return { { width, height }, { 60, 1 } };
  }

  dt::observed_display_t
  monitor(const std::string &name, std::int32_t x, state_e state = state_e::active) {
    dt::observed_display_t display;
    display.id = dt::os_display_id_t { name };
    display.info = { "\\\\.\\DISPLAY-" + name, "Panel " + name, state, display_device::hdr_state_e::disabled };
    display.layout = display_topology_test::layout(x);
    display.adapter = dt::adapter_luid_t { 7 };
    display.incarnation = dt::source_incarnation_t { 1 };
    display.supported_modes = { display.layout->mode, mode(2560, 1440) };
    return display;
  }

  dt::observed_display_t
  virtual_display(const std::string &name, std::int32_t x) {
    auto display = monitor(name, x);
    display.info.friendly_name = "ZakoVDD";
    return display;
  }

  /** os-A (primary, x=0) and os-B (x=1920); no virtual display. */
  dt::inventory_t
  host_inventory() {
    dt::inventory_t inventory;
    inventory.available = true;
    inventory.revision = dt::inventory_revision_t { 1 };
    inventory.displays = { monitor("os-A", 0, state_e::primary), monitor("os-B", 1920) };
    return inventory;
  }

  template <class Inventory>
  auto &
  find(Inventory &inventory, const std::string &name) {
    for (auto &display : inventory.displays) {
      if (display.id.value == name) {
        return display;
      }
    }
    throw std::runtime_error { "fixture has no display " + name };
  }

  struct event_log_t {
    std::vector<std::string> events;

    void
    add(std::string event) {
      events.push_back(std::move(event));
    }

    bool
    contains(const std::string &event) const {
      return std::find(events.begin(), events.end(), event) != events.end();
    }

    /** Platform writes only (not journal saves or clears). */
    std::size_t
    host_writes() const {
      return static_cast<std::size_t>(std::count_if(events.begin(), events.end(), [](const std::string &event) {
        return event == "create_head" || event == "remove_all_heads" || event.starts_with("apply_fields:");
      }));
    }
  };

  struct not_a_std_exception_t {};

  class memory_store_t final: public dt::journal_store_t {
  public:
    explicit memory_store_t(event_log_t &log):
        log_(log) {}

    dt::journal_load_t
    load() override {
      ++loads;
      if (throw_on_load) {
        throw std::runtime_error { "load exploded" };
      }
      if (!bytes) {
        return {};
      }
      auto record = dt::deserialize_journal(*bytes);
      if (!record) {
        return { record.error().code == dt::error_code_e::unsupported_version ? dt::journal_load_status_e::unsupported_version :
                                                                                dt::journal_load_status_e::invalid,
          std::nullopt, record.error().detail };
      }
      return { dt::journal_load_status_e::loaded, std::move(*record), {} };
    }

    dt::result_t<void>
    save(const dt::journal_record_t &record) override {
      ++saves;
      log_.add("save");
      if (fail_save_call == saves) {
        return std::unexpected(dt::error_t { dt::error_code_e::resource_unavailable, "disk full", {} });
      }
      auto serialized = dt::serialize_journal(record);
      if (!serialized) {
        log_.add("save-invalid");
        return std::unexpected(serialized.error());
      }
      bytes = *serialized;
      history.push_back(record);
      return {};
    }

    dt::result_t<void>
    clear() override {
      log_.add("clear");
      if (throw_on_clear) {
        throw not_a_std_exception_t {};
      }
      if (throw_std_on_clear) {
        throw std::runtime_error { "clear exploded" };
      }
      if (fail_clear) {
        return std::unexpected(dt::error_t { dt::error_code_e::resource_unavailable, "journal locked", {} });
      }
      bytes.reset();
      return {};
    }

    void
    put(const dt::journal_record_t &record) {
      const auto serialized = dt::serialize_journal(record);
      if (!serialized) {
        throw std::runtime_error { "fixture journal is invalid: " + serialized.error().detail };
      }
      bytes = *serialized;
    }

    std::optional<dt::journal_record_t>
    record() const {
      if (!bytes) {
        return std::nullopt;
      }
      auto record = dt::deserialize_journal(*bytes);
      return record ? std::optional { std::move(*record) } : std::nullopt;
    }

    std::optional<std::string> bytes;
    std::vector<dt::journal_record_t> history;
    int loads {};
    int saves {};
    /** 1-based save call that fails; 0 never fails. */
    int fail_save_call {};
    bool fail_clear {};
    bool throw_on_clear {};
    bool throw_std_on_clear {};
    bool throw_on_load {};

  private:
    event_log_t &log_;
  };

  dt::result_t<void>
  failure(const std::string &detail) {
    return std::unexpected(dt::error_t { dt::error_code_e::resource_unavailable, detail, {} });
  }

  /** os-A and os-B by default; ZakoVDD heads are recognised by friendly name. */
  class fake_platform_t final: public dt::native_platform_t {
  public:
    explicit fake_platform_t(event_log_t &log):
        host(host_inventory()),
        log_(log) {}

    dt::inventory_t
    inventory() override {
      ++reads;
      if (throw_on_inventory) {
        throw std::runtime_error { "inventory exploded" };
      }
      auto snapshot = host;
      snapshot.revision = dt::inventory_revision_t { static_cast<std::uint64_t>(reads) };
      if (tamper) {
        tamper(reads, snapshot);
      }
      return snapshot;
    }

    bool
    is_virtual_head(const dt::observed_display_t &display) const override {
      return display.info.friendly_name == "ZakoVDD";
    }

    dt::result_t<void>
    create_head(const dt::head_guid_t &intent) override {
      log_.add("create_head");
      intents.push_back(intent);
      if (on_create) {
        return on_create(intents.size());
      }
      add_head();
      return {};
    }

    dt::result_t<void>
    remove_all_heads() override {
      log_.add("remove_all_heads");
      if (on_remove) {
        return on_remove();
      }
      drop_heads();
      return {};
    }

    dt::result_t<void>
    apply_fields(const dt::os_display_id_t &display, const dt::managed_values_t &values) override {
      log_.add("apply_fields:" + display.value);
      applied.emplace_back(display, values);
      if (apply_error) {
        return failure("apply refused");
      }
      if (!ignore_apply) {
        auto &target = find(host, display.value);
        auto &geometry = *target.layout;
        if (values.position) geometry.position = *values.position;
        if (values.mode) geometry.mode = *values.mode;
        if (values.rotation) geometry.rotation = *values.rotation;
        if (values.scale_percent) geometry.scale_percent = values.scale_percent;
        if (values.hdr) geometry.hdr = *values.hdr;
        if (values.primary) target.info.device_state = *values.primary ? state_e::primary : state_e::active;
      }
      if (after_apply) {
        after_apply();
      }
      return {};
    }

    std::string
    add_head() {
      auto name = "vdd-" + std::to_string(++created);
      host.displays.push_back(virtual_display(name, 3840));
      return name;
    }

    void
    drop_heads() {
      std::erase_if(host.displays, [this](const dt::observed_display_t &display) { return is_virtual_head(display); });
    }

    std::size_t
    head_count() const {
      return static_cast<std::size_t>(std::count_if(host.displays.begin(), host.displays.end(),
        [this](const dt::observed_display_t &display) { return is_virtual_head(display); }));
    }

    dt::inventory_t host;
    int reads {};
    int created {};
    std::vector<dt::head_guid_t> intents;
    std::vector<std::pair<dt::os_display_id_t, dt::managed_values_t>> applied;
    /** Replaces the default create (one new head, success); receives the 1-based attempt. */
    std::function<dt::result_t<void>(std::size_t)> on_create;
    /** Replaces the default removal (every head goes, success). */
    std::function<dt::result_t<void>()> on_remove;
    std::function<void()> after_apply;
    /** Edits one snapshot; receives the 1-based read number. */
    std::function<void(int, dt::inventory_t &)> tamper;
    bool throw_on_inventory {};
    bool apply_error {};
    bool ignore_apply {};

  private:
    event_log_t &log_;
  };

  /** Run `body` on the worker with a real capability of `kind`, minted by a legacy-mode coordinator. */
  void
  with_capability(dt::command_kind_e kind, const std::function<void(const dt::mutation_capability_t &)> &body) {
    dt::coordinator_t coordinator;
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
    if (kind == dt::command_kind_e::startup_recovery) {
      coordinator.start(wrapped).get();
    }
    else {
      coordinator.start([](const dt::mutation_capability_t &, const dt::command_context_t &) {}).get();
      if (kind == dt::command_kind_e::launch_prepare) {
        const auto admission = coordinator.admit_launch({ 1, "cert:native-test" });
        if (!admission) {
          throw std::runtime_error { "the test launch was not admitted" };
        }
        coordinator.run_launch_prepare(*admission.token, wrapped);
        coordinator.abort_launch(*admission.token, {});
      }
      else {
        coordinator.run(kind, wrapped);
      }
    }
    coordinator.stop();
    // runtime_error, not logic_error, so it is never mistaken for a capability_error_t.
    if (!ran) {
      throw std::runtime_error { "the command body did not run" };
    }
    if (thrown) {
      std::rethrow_exception(thrown);
    }
  }

  dt::native_health_t
  recover_with(dt::native_executor_t &executor) {
    dt::native_health_t health;
    with_capability(dt::command_kind_e::startup_recovery, [&](const auto &capability) { health = executor.recover(capability); });
    return health;
  }

  dt::native_outcome_t
  prepare_with(dt::native_executor_t &executor, const dt::native_profile_t &profile) {
    dt::native_outcome_t outcome;
    with_capability(dt::command_kind_e::launch_prepare, [&](const auto &capability) { outcome = executor.prepare(capability, profile); });
    return outcome;
  }

  dt::native_outcome_t
  restore_with(dt::native_executor_t &executor) {
    dt::native_outcome_t outcome;
    with_capability(dt::command_kind_e::native_restore, [&](const auto &capability) { outcome = executor.restore(capability); });
    return outcome;
  }

  dt::native_health_t
  reset_with(dt::native_executor_t &executor) {
    dt::native_health_t health;
    with_capability(dt::command_kind_e::reset_persistence, [&](const auto &capability) { health = executor.reset(capability); });
    return health;
  }

  dt::native_profile_t
  virtual_profile() {
    return { dt::native_profile_t::source_e::virtual_head, {} };
  }

  dt::native_profile_t
  physical_profile(const std::string &display) {
    return { dt::native_profile_t::source_e::physical, dt::os_display_id_t { display } };
  }

  dt::managed_values_t
  values_of(const dt::observed_display_t &display) {
    const auto &geometry = *display.layout;
    return { geometry.position, geometry.mode, geometry.rotation, geometry.scale_percent,
      geometry.hdr == display_device::hdr_state_e::unknown ? std::nullopt : std::optional { geometry.hdr },
      display.info.device_state == state_e::primary };
  }

  /** A managed display of the default host whose baseline holds the intended fields' values. */
  dt::managed_display_t
  managed(const std::string &name, const dt::managed_values_t &intended, std::optional<dt::managed_values_t> confirmed = std::nullopt) {
    const auto baseline = host_inventory();
    const auto &display = find(baseline, name);
    const auto actual = values_of(display);
    dt::managed_values_t base;
    if (intended.position) base.position = actual.position;
    if (intended.mode) base.mode = actual.mode;
    if (intended.rotation) base.rotation = actual.rotation;
    if (intended.scale_percent) base.scale_percent = actual.scale_percent;
    if (intended.hdr) base.hdr = actual.hdr;
    if (intended.primary) base.primary = actual.primary;
    return { { display.id, *display.adapter, display.incarnation, std::nullopt }, base, intended, std::move(confirmed) };
  }

  dt::journal_record_t
  foreign_journal() {
    dt::journal_record_t record;
    record.owner = id<dt::owner_id_t>(0x51);
    record.transaction = id<dt::transaction_id_t>(0x52);
    record.request = id<dt::request_id_t>(0x53);
    record.baseline = host_inventory();
    return record;
  }

  /** Field steps of another run over the default host, one per (display, state), in order. */
  dt::journal_record_t
  field_journal(std::vector<dt::managed_display_t> displays, const std::vector<std::pair<std::string, step_e>> &steps) {
    auto record = foreign_journal();
    record.managed = std::move(displays);
    std::uint32_t sequence = 0;
    for (const auto &[display, state] : steps) {
      record.steps.push_back({ ++sequence, dt::journal_operation_e::apply_fields, dt::os_display_id_t { display }, std::nullopt, state,
        state == step_e::observed ? std::optional { host_inventory() } : std::nullopt,
        state == step_e::failed || state == step_e::conflict ? "fixture" : "" });
    }
    return record;
  }

  /** One create step of another run in `state`; an observed step names vdd-1 and owns its head. */
  dt::journal_record_t
  head_journal(step_e state) {
    auto record = foreign_journal();
    const auto intent = id<dt::head_guid_t>(0x54);
    dt::journal_step_t step { 1, dt::journal_operation_e::create_head, std::nullopt, intent, state, std::nullopt, {} };
    if (state == step_e::observed) {
      auto observed = host_inventory();
      observed.displays.push_back(virtual_display("vdd-1", 3840));
      step.display = dt::os_display_id_t { "vdd-1" };
      step.observed = observed;
      record.owned_heads.push_back({ { driver(), intent, dt::head_incarnation_t { 1 } }, record.owner, dt::adapter_luid_t { 7 }, 0 });
    }
    if (state == step_e::failed || state == step_e::conflict) {
      step.detail = "fixture";
    }
    record.steps.push_back(step);
    return record;
  }

  class NativeExecutor: public ::testing::Test {
  protected:
    dt::native_executor_t
    make_executor(std::uint32_t attempts = 2) {
      return dt::native_executor_t { store, platform, { next_uuid, attempts, driver() } };
    }

    event_log_t log;
    memory_store_t store { log };
    fake_platform_t platform { log };
    dt::native_executor_t executor { make_executor() };
  };

}  // namespace

// --- Profile setting -------------------------------------------------------------

TEST(NativeProfile, BlankSettingMeansLegacyMode) {
  for (const std::string text : { "", " ", " \t\r\n " }) {
    const auto parsed = dt::parse_native_profile(text);
    ASSERT_TRUE(parsed) << parsed.error().detail;
    EXPECT_FALSE(parsed->has_value()) << '"' << text << '"';
  }
}

TEST(NativeProfile, PhysicalDisplayIdIsKeptExactly) {
  const auto parsed = dt::parse_native_profile(
    R"({"version":1,"monitors":[{"source":"physical","display":"\\\\?\\DISPLAY#DEL41F2#5&1a2B&0&UID4353"}]})");
  ASSERT_TRUE(parsed) << parsed.error().detail;
  ASSERT_TRUE(parsed->has_value());
  EXPECT_TRUE(parsed->value().source == dt::native_profile_t::source_e::physical);
  EXPECT_EQ(parsed->value().display.value, R"(\\?\DISPLAY#DEL41F2#5&1a2B&0&UID4353)");
}

TEST(NativeProfile, VirtualProfileIgnoresKeyOrderAndWhitespace) {
  const auto parsed = dt::parse_native_profile(" { \"monitors\" : [ { \"source\" : \"virtual\" } ] , \"version\" : 1 } \n");
  ASSERT_TRUE(parsed) << parsed.error().detail;
  ASSERT_TRUE(parsed->has_value());
  EXPECT_TRUE(parsed->value() == virtual_profile());
}

TEST(NativeProfile, RejectsAnythingItCannotHonourExactly) {
  using enum dt::error_code_e;
  const std::vector<std::pair<std::string, dt::error_code_e>> cases {
    { "not json", invalid_profile },
    { R"([{"source":"virtual"}])", invalid_profile },
    { R"({"version":1,"monitors":[{"source":"virtual"}],"fallback":"primary"})", invalid_profile },
    { R"({"monitors":[{"source":"virtual"}]})", invalid_profile },
    { R"({"version":"1","monitors":[{"source":"virtual"}]})", invalid_profile },
    { R"({"version":-1,"monitors":[{"source":"virtual"}]})", invalid_profile },
    { R"({"version":1.0,"monitors":[{"source":"virtual"}]})", invalid_profile },
    { R"({"version":0,"monitors":[{"source":"virtual"}]})", unsupported_version },
    { R"({"version":2,"monitors":[{"source":"virtual"}]})", unsupported_version },
    { R"({"version":1})", invalid_profile },
    { R"({"version":1,"monitors":{"source":"virtual"}})", invalid_profile },
    { R"({"version":1,"monitors":[]})", invalid_profile },
    { R"({"version":1,"monitors":[{"source":"virtual"},{"source":"physical","display":"os-A"}]})", limit_exceeded },
    { R"({"version":1,"monitors":["virtual"]})", invalid_profile },
    { R"({"version":1,"monitors":[{}]})", invalid_profile },
    { R"({"version":1,"monitors":[{"source":1}]})", invalid_profile },
    { R"({"version":1,"monitors":[{"source":"primary"}]})", invalid_profile },
    { R"({"version":1,"monitors":[{"source":"Virtual"}]})", invalid_profile },
    { R"({"version":1,"monitors":[{"source":"virtual","display":"os-A"}]})", invalid_profile },
    { R"({"version":1,"monitors":[{"source":"virtual","adapter":7}]})", invalid_profile },
    { R"({"version":1,"monitors":[{"source":"physical"}]})", invalid_profile },
    { R"({"version":1,"monitors":[{"source":"Physical","display":"os-A"}]})", invalid_profile },
    { R"({"version":1,"monitors":[{"source":"physical","display":""}]})", invalid_profile },
    { R"({"version":1,"monitors":[{"source":"physical","display":7}]})", invalid_profile },
    { R"({"version":1,"monitors":[{"source":"physical","display":"os-A","primary":true}]})", invalid_profile },
  };
  for (const auto &[text, code] : cases) {
    const auto parsed = dt::parse_native_profile(text);
    EXPECT_TRUE(error_is(parsed, code)) << text;
    if (!parsed) {
      EXPECT_TRUE(parsed.error().detail.starts_with("display_topology_profile: ")) << parsed.error().detail;
    }
  }
}

TEST(NativeProfile, SettingIsLimitedTo4096Bytes) {
  std::string text = R"({"version":1,"monitors":[{"source":"virtual"}]})";
  text.resize(4096, ' ');
  const auto longest = dt::parse_native_profile(text);
  ASSERT_TRUE(longest) << longest.error().detail;
  EXPECT_TRUE(longest->has_value());

  text.push_back(' ');
  const auto too_long = dt::parse_native_profile(text);
  ASSERT_TRUE(error_is(too_long, dt::error_code_e::invalid_profile));
  EXPECT_TRUE(mentions(too_long.error().detail, "4096"));

  // Oversize is refused even when it is all whitespace, never read as "legacy mode".
  const auto blank = dt::parse_native_profile(std::string(5000, ' '));
  ASSERT_TRUE(error_is(blank, dt::error_code_e::invalid_profile));
  EXPECT_TRUE(mentions(blank.error().detail, "4096"));
}

// --- Executor construction and capabilities ---------------------------------------

TEST_F(NativeExecutor, ConstructionNeedsItsDependencies) {
  const auto construct = [&](dt::native_executor_options_t options) {
    const dt::native_executor_t candidate { store, platform, std::move(options) };
  };
  EXPECT_THROW(construct({ {}, 2, driver() }), std::invalid_argument);
  EXPECT_THROW(construct({ next_uuid, 0, driver() }), std::invalid_argument);
  EXPECT_THROW(construct({ next_uuid, 2, {} }), std::invalid_argument);
  EXPECT_THROW(construct({ [] { return dt::uuid_bytes_t {}; }, 2, driver() }), std::runtime_error);
  EXPECT_NO_THROW(construct({ next_uuid, 1, driver() }));
}

TEST_F(NativeExecutor, EveryEntryPointRequiresItsOwnCapability) {
  const auto refused = [&](dt::command_kind_e kind, const std::function<void(const dt::mutation_capability_t &)> &body) {
    EXPECT_THROW(with_capability(kind, body), dt::capability_error_t) << dt::to_string(kind);
  };
  store.put(head_journal(step_e::attempted));
  refused(dt::command_kind_e::rotate, [&](const auto &capability) { executor.prepare(capability, virtual_profile()); });
  refused(dt::command_kind_e::rotate, [&](const auto &capability) { executor.prepare(capability, physical_profile("os-A")); });
  refused(dt::command_kind_e::launch_prepare, [&](const auto &capability) { executor.restore(capability); });
  refused(dt::command_kind_e::stream_restore, [&](const auto &capability) { executor.recover(capability); });
  refused(dt::command_kind_e::stream_restore, [&](const auto &capability) { executor.reset(capability); });
  EXPECT_EQ(store.loads, 0);
  EXPECT_EQ(platform.reads, 0);
  EXPECT_TRUE(log.events.empty());
  EXPECT_TRUE(store.bytes.has_value());
}

// --- Physical profile ---------------------------------------------------------------

TEST_F(NativeExecutor, PhysicalProfileCapturesTheExactDisplayWithoutWriting) {
  const auto outcome = prepare_with(executor, physical_profile("os-B"));
  ASSERT_TRUE(verdict_is(outcome, native_verdict_e::ok));
  ASSERT_TRUE(outcome.target);
  EXPECT_TRUE((*outcome.target == dt::native_target_t { dt::os_display_id_t { "os-B" }, false }));
  EXPECT_EQ(store.saves, 0);
  EXPECT_EQ(log.host_writes(), 0u);
  EXPECT_FALSE(store.bytes);
  EXPECT_TRUE(healthy(executor.health()));
}

TEST_F(NativeExecutor, PhysicalProfileNeverFallsBackToAnotherDisplay) {
  for (const std::string display : { "os-C", "OS-B", "os-b", "os-B " }) {
    const auto outcome = prepare_with(executor, physical_profile(display));
    EXPECT_TRUE(verdict_is(outcome, native_verdict_e::refused)) << display;
    EXPECT_FALSE(outcome.target) << display;
    EXPECT_TRUE(mentions(outcome.reason, "Display " + display + ":")) << display;
  }
  EXPECT_EQ(store.saves, 0);
  EXPECT_EQ(log.host_writes(), 0u);
  EXPECT_TRUE(healthy(executor.health()));
}

TEST_F(NativeExecutor, PhysicalProfileRefusesAnInactiveOrUnobservableDisplay) {
  find(platform.host, "os-B").info.device_state = state_e::inactive;
  EXPECT_TRUE(verdict_is(prepare_with(executor, physical_profile("os-B")), native_verdict_e::refused));
  platform.host = host_inventory();
  platform.host.available = false;
  EXPECT_TRUE(verdict_is(prepare_with(executor, physical_profile("os-B")), native_verdict_e::refused));
  EXPECT_EQ(store.saves, 0);
  EXPECT_EQ(log.host_writes(), 0u);
}

TEST_F(NativeExecutor, PhysicalProfileRefusesAVirtualDisplay) {
  const auto head = platform.add_head();
  const auto outcome = prepare_with(executor, physical_profile(head));
  EXPECT_TRUE(verdict_is(outcome, native_verdict_e::refused));
  EXPECT_TRUE(mentions(outcome.reason, "is a virtual display"));
  EXPECT_EQ(platform.head_count(), 1u);
  EXPECT_EQ(log.host_writes(), 0u);
}

TEST_F(NativeExecutor, PrepareFailsClosedWhileAnyJournalExists) {
  store.put(head_journal(step_e::attempted));
  for (const auto &profile : { virtual_profile(), physical_profile("os-A") }) {
    EXPECT_TRUE(verdict_is(prepare_with(executor, profile), native_verdict_e::conflict));
    EXPECT_TRUE(in_conflict(executor.health()));
  }
  store.bytes = "{not json";
  EXPECT_TRUE(verdict_is(prepare_with(executor, virtual_profile()), native_verdict_e::conflict));
  store.bytes.reset();
  store.throw_on_load = true;
  EXPECT_TRUE(verdict_is(prepare_with(executor, virtual_profile()), native_verdict_e::conflict));
  EXPECT_EQ(platform.reads, 0);
  EXPECT_EQ(store.saves, 0);
  EXPECT_EQ(log.host_writes(), 0u);
}

// --- Virtual profile ----------------------------------------------------------------

TEST_F(NativeExecutor, VirtualDisplayIsJournaledBeforeItIsCreated) {
  bool journaled_first = false;
  platform.on_create = [&](std::size_t) -> dt::result_t<void> {
    const auto record = store.record();
    journaled_first = record && record->steps.size() == 1 && record->steps[0].operation == dt::journal_operation_e::create_head &&
                      record->steps[0].state == step_e::attempted && record->steps[0].head_intent == platform.intents.back() &&
                      record->owned_heads.empty() && record->baseline.displays == host_inventory().displays;
    platform.add_head();
    return {};
  };
  const auto outcome = prepare_with(executor, virtual_profile());
  ASSERT_TRUE(verdict_is(outcome, native_verdict_e::ok));
  EXPECT_TRUE(journaled_first);
  ASSERT_TRUE(outcome.target);
  EXPECT_TRUE((*outcome.target == dt::native_target_t { dt::os_display_id_t { "vdd-1" }, true }));
  EXPECT_EQ(log.events, (std::vector<std::string> { "save", "create_head", "save" }));

  const auto &health = executor.health();
  EXPECT_TRUE(health.journal_present);
  EXPECT_FALSE(health.in_conflict);
  EXPECT_TRUE(health.reason.empty());

  const auto record = store.record();
  ASSERT_TRUE(record);
  ASSERT_EQ(record->owned_heads.size(), 1u);
  const auto &owned = record->owned_heads.front();
  EXPECT_TRUE(owned.owner == record->owner);
  EXPECT_TRUE(owned.identity.guid == platform.intents.front());
  EXPECT_TRUE(owned.identity.driver == driver());
  ASSERT_EQ(record->steps.size(), 1u);
  EXPECT_TRUE(record->steps[0].state == step_e::observed);
  EXPECT_TRUE(record->steps[0].display == dt::os_display_id_t { "vdd-1" });
}

TEST_F(NativeExecutor, AnExistingVirtualDisplayIsNeverAdopted) {
  platform.add_head();
  auto outcome = prepare_with(executor, virtual_profile());
  EXPECT_TRUE(verdict_is(outcome, native_verdict_e::refused));
  EXPECT_TRUE(mentions(outcome.reason, "already exists"));

  find(platform.host, "vdd-1").info.device_state = state_e::inactive;
  outcome = prepare_with(executor, virtual_profile());
  EXPECT_TRUE(verdict_is(outcome, native_verdict_e::refused));
  EXPECT_TRUE(mentions(outcome.reason, "already exists"));
  EXPECT_EQ(store.saves, 0);
  EXPECT_EQ(log.host_writes(), 0u);
  EXPECT_TRUE(healthy(executor.health()));
}

TEST_F(NativeExecutor, AnUnknownInventoryCreatesNothing) {
  platform.host.available = false;
  const auto outcome = prepare_with(executor, virtual_profile());
  EXPECT_TRUE(verdict_is(outcome, native_verdict_e::refused));
  EXPECT_TRUE(mentions(outcome.reason, "The display inventory is unavailable"));
  EXPECT_EQ(store.saves, 0);
  EXPECT_EQ(log.host_writes(), 0u);
}

TEST_F(NativeExecutor, RetriesKeepTheFirstBaselineAndUseFreshIdentities) {
  platform.on_create = [&](std::size_t attempt) -> dt::result_t<void> {
    if (attempt == 1) {
      return failure("driver busy");
    }
    platform.add_head();
    return {};
  };
  // Geometry is outside the "other displays" check, so a moved display does not stop the
  // retry; it must not leak into the journaled baseline either.
  platform.tamper = [](int read, dt::inventory_t &inventory) {
    if (read > 1) {
      find(inventory, "os-B").layout->position = { 2000, 0 };
    }
  };
  const auto outcome = prepare_with(executor, virtual_profile());
  ASSERT_TRUE(verdict_is(outcome, native_verdict_e::ok));
  ASSERT_EQ(platform.intents.size(), 2u);
  EXPECT_FALSE(platform.intents[0] == platform.intents[1]);

  const auto record = store.record();
  ASSERT_TRUE(record);
  EXPECT_EQ(record->baseline.revision.value, 1u);
  EXPECT_TRUE(find(record->baseline, "os-B").layout->position == (dt::position_t { 1920, 0 }));
  ASSERT_EQ(record->steps.size(), 2u);
  EXPECT_TRUE(record->steps[0].state == step_e::failed);
  EXPECT_EQ(record->steps[0].detail, "driver busy");
  EXPECT_TRUE(record->steps[0].head_intent == platform.intents[0]);
  EXPECT_TRUE(record->steps[1].state == step_e::observed);
  EXPECT_TRUE(record->steps[1].head_intent == platform.intents[1]);
  ASSERT_EQ(record->owned_heads.size(), 1u);
  EXPECT_TRUE(record->owned_heads[0].identity.guid == platform.intents[1]);
}

TEST_F(NativeExecutor, ExhaustedAttemptsLeaveNoJournal) {
  platform.on_create = [](std::size_t) { return failure("driver busy"); };
  const auto outcome = prepare_with(executor, virtual_profile());
  EXPECT_TRUE(verdict_is(outcome, native_verdict_e::refused));
  EXPECT_TRUE(mentions(outcome.reason, "driver busy"));
  EXPECT_EQ(platform.intents.size(), 2u);
  EXPECT_EQ(log.events, (std::vector<std::string> { "save", "create_head", "save", "create_head", "clear" }));
  EXPECT_FALSE(store.bytes);
  ASSERT_EQ(store.history.size(), 2u);
  ASSERT_EQ(store.history.back().steps.size(), 2u);
  EXPECT_TRUE(store.history.back().steps[0].state == step_e::failed);
  EXPECT_TRUE(healthy(executor.health()));
}

TEST_F(NativeExecutor, ADriverErrorWithoutDetailIsStillExplained) {
  platform.on_create = [](std::size_t) { return failure(""); };
  const auto outcome = prepare_with(executor, virtual_profile());
  EXPECT_TRUE(verdict_is(outcome, native_verdict_e::refused));
  EXPECT_TRUE(mentions(outcome.reason, "the driver reported an error"));
  EXPECT_EQ(platform.intents.size(), 2u);
  ASSERT_EQ(store.history.size(), 2u);
  ASSERT_FALSE(store.history.back().steps.empty());
  EXPECT_EQ(store.history.back().steps[0].detail, "the driver reported an error");
  EXPECT_FALSE(store.bytes);
}

TEST_F(NativeExecutor, ARefusedRequestThatDisturbedOtherDisplaysIsNotRetried) {
  platform.on_create = [&](std::size_t) {
    find(platform.host, "os-B").info.device_state = state_e::inactive;
    return failure("driver busy");
  };
  const auto outcome = prepare_with(executor, virtual_profile());
  EXPECT_TRUE(verdict_is(outcome, native_verdict_e::refused));
  EXPECT_TRUE(mentions(outcome.reason, "Other displays changed"));
  EXPECT_EQ(platform.intents.size(), 1u);
  EXPECT_FALSE(store.bytes);
}

TEST_F(NativeExecutor, AnAcceptedRequestWithoutADisplayStaysUnresolvedUntilReset) {
  platform.on_create = [](std::size_t) -> dt::result_t<void> { return {}; };
  const auto outcome = prepare_with(executor, virtual_profile());
  EXPECT_TRUE(verdict_is(outcome, native_verdict_e::conflict));
  EXPECT_TRUE(mentions(outcome.reason, "accepted the request"));
  EXPECT_EQ(platform.intents.size(), 1u);
  EXPECT_TRUE(in_conflict(executor.health()));
  const auto record = store.record();
  ASSERT_TRUE(record);
  ASSERT_EQ(record->steps.size(), 1u);
  EXPECT_TRUE(record->steps[0].state == step_e::conflict);
  EXPECT_TRUE(mentions(record->steps[0].detail, "accepted the request"));

  // No display is listed, yet one may still appear: neither this run nor a restart may
  // conclude that nothing was created.
  const auto reads = platform.reads;
  EXPECT_TRUE(verdict_is(restore_with(executor), native_verdict_e::conflict));
  EXPECT_TRUE(in_conflict(recover_with(executor)));
  auto restarted = make_executor();
  EXPECT_TRUE(in_conflict(recover_with(restarted)));
  EXPECT_TRUE(verdict_is(prepare_with(restarted, virtual_profile()), native_verdict_e::conflict));
  EXPECT_EQ(platform.reads, reads);
  EXPECT_EQ(log.host_writes(), 1u);
  EXPECT_TRUE(store.bytes);

  EXPECT_TRUE(healthy(reset_with(restarted)));
  EXPECT_FALSE(store.bytes);
  EXPECT_EQ(platform.reads, reads);
}

TEST_F(NativeExecutor, AnUnknownInventoryAfterCreatingIsUnresolved) {
  platform.tamper = [](int read, dt::inventory_t &inventory) {
    if (read == 2) {
      inventory.available = false;
    }
  };
  const auto outcome = prepare_with(executor, virtual_profile());
  EXPECT_TRUE(verdict_is(outcome, native_verdict_e::conflict));
  EXPECT_EQ(platform.head_count(), 1u);
  EXPECT_FALSE(log.contains("remove_all_heads"));
  const auto record = store.record();
  ASSERT_TRUE(record);
  EXPECT_TRUE(record->steps.back().state == step_e::conflict);

  // Even once no display is listed, only a reset clears it.
  platform.drop_heads();
  const auto reads = platform.reads;
  EXPECT_TRUE(verdict_is(restore_with(executor), native_verdict_e::conflict));
  EXPECT_EQ(platform.reads, reads);
  EXPECT_TRUE(store.bytes);
}

TEST_F(NativeExecutor, TwoNewVirtualDisplaysAreLeftInPlace) {
  platform.on_create = [&](std::size_t) -> dt::result_t<void> {
    platform.add_head();
    platform.add_head();
    return {};
  };
  EXPECT_TRUE(verdict_is(prepare_with(executor, virtual_profile()), native_verdict_e::conflict));
  EXPECT_TRUE(verdict_is(restore_with(executor), native_verdict_e::conflict));
  EXPECT_EQ(platform.head_count(), 2u);
  EXPECT_FALSE(log.contains("remove_all_heads"));
  EXPECT_TRUE(store.bytes);
}

TEST_F(NativeExecutor, ADisplayDespiteADriverErrorIsRolledBack) {
  platform.on_create = [&](std::size_t) {
    platform.add_head();
    return failure("timed out");
  };
  const auto outcome = prepare_with(executor, virtual_profile());
  EXPECT_TRUE(verdict_is(outcome, native_verdict_e::refused));
  EXPECT_TRUE(mentions(outcome.reason, "timed out"));
  EXPECT_EQ(log.events, (std::vector<std::string> { "save", "create_head", "remove_all_heads", "clear" }));
  EXPECT_EQ(platform.head_count(), 0u);
  EXPECT_FALSE(store.bytes);
  EXPECT_TRUE(healthy(executor.health()));
}

TEST_F(NativeExecutor, ADisplayThatDisturbedOtherDisplaysIsRolledBack) {
  platform.on_create = [&](std::size_t) -> dt::result_t<void> {
    platform.add_head();
    find(platform.host, "os-B").info.device_state = state_e::inactive;
    return {};
  };
  platform.on_remove = [&]() -> dt::result_t<void> {
    platform.drop_heads();
    find(platform.host, "os-B").info.device_state = state_e::active;
    return {};
  };
  const auto outcome = prepare_with(executor, virtual_profile());
  EXPECT_TRUE(verdict_is(outcome, native_verdict_e::refused));
  EXPECT_TRUE(mentions(outcome.reason, "changed other displays"));
  EXPECT_EQ(platform.head_count(), 0u);
  EXPECT_FALSE(store.bytes);
}

TEST_F(NativeExecutor, ARollbackThatLeavesOtherDisplaysChangedIsKeptUntilTheyReturn) {
  platform.on_create = [&](std::size_t) -> dt::result_t<void> {
    platform.add_head();
    find(platform.host, "os-B").info.device_state = state_e::inactive;
    return {};
  };
  const auto outcome = prepare_with(executor, virtual_profile());
  EXPECT_TRUE(verdict_is(outcome, native_verdict_e::conflict));
  EXPECT_TRUE(mentions(outcome.reason, "still differ"));
  EXPECT_EQ(platform.head_count(), 0u);
  EXPECT_TRUE(store.bytes);

  // Nothing is left to remove, but the journal stays while other displays differ.
  EXPECT_TRUE(verdict_is(restore_with(executor), native_verdict_e::conflict));
  EXPECT_TRUE(store.bytes);
  find(platform.host, "os-B").info.device_state = state_e::active;
  EXPECT_TRUE(verdict_is(restore_with(executor), native_verdict_e::ok));
  EXPECT_FALSE(store.bytes);
  EXPECT_TRUE(healthy(executor.health()));
}

TEST_F(NativeExecutor, AFailedOrUnconfirmedRollbackIsInConflict) {
  platform.on_create = [&](std::size_t) {
    platform.add_head();
    return failure("timed out");
  };
  platform.on_remove = [] { return failure("remove refused"); };
  auto outcome = prepare_with(executor, virtual_profile());
  EXPECT_TRUE(verdict_is(outcome, native_verdict_e::conflict));
  EXPECT_TRUE(mentions(outcome.reason, "remove refused"));
  EXPECT_TRUE(store.bytes);

  ASSERT_TRUE(healthy(reset_with(executor)));
  platform.drop_heads();
  platform.on_remove = [] { return dt::result_t<void> {}; };
  outcome = prepare_with(executor, virtual_profile());
  EXPECT_TRUE(verdict_is(outcome, native_verdict_e::conflict));
  EXPECT_TRUE(mentions(outcome.reason, "could not be confirmed"));
  EXPECT_TRUE(store.bytes);

  // No display is listed, but the listing itself could not be read.
  ASSERT_TRUE(healthy(reset_with(executor)));
  platform.drop_heads();
  platform.on_remove = [&] {
    platform.drop_heads();
    platform.host.available = false;
    return dt::result_t<void> {};
  };
  outcome = prepare_with(executor, virtual_profile());
  EXPECT_TRUE(verdict_is(outcome, native_verdict_e::conflict));
  EXPECT_TRUE(mentions(outcome.reason, "could not be confirmed"));
  EXPECT_TRUE(store.bytes);
}

TEST_F(NativeExecutor, NothingIsCreatedUnlessTheAttemptIsJournaled) {
  store.fail_save_call = 1;
  const auto outcome = prepare_with(executor, virtual_profile());
  EXPECT_TRUE(verdict_is(outcome, native_verdict_e::refused));
  EXPECT_TRUE(mentions(outcome.reason, "disk full"));
  EXPECT_EQ(log.events, (std::vector<std::string> { "save", "clear" }));
  EXPECT_TRUE(healthy(executor.health()));
}

TEST_F(NativeExecutor, AnUnrecordedDisplayIsRolledBack) {
  store.fail_save_call = 2;
  const auto outcome = prepare_with(executor, virtual_profile());
  EXPECT_TRUE(verdict_is(outcome, native_verdict_e::refused));
  EXPECT_TRUE(mentions(outcome.reason, "disk full"));
  EXPECT_EQ(log.events, (std::vector<std::string> { "save", "create_head", "save", "remove_all_heads", "clear" }));
  EXPECT_EQ(platform.head_count(), 0u);
  EXPECT_FALSE(store.bytes);
}

TEST_F(NativeExecutor, AnExceptionAfterJournalingKeepsTheJournal) {
  platform.on_create = [](std::size_t) -> dt::result_t<void> { throw std::runtime_error { "driver crashed" }; };
  const auto outcome = prepare_with(executor, virtual_profile());
  EXPECT_TRUE(verdict_is(outcome, native_verdict_e::conflict));
  EXPECT_TRUE(mentions(outcome.reason, "driver crashed"));
  EXPECT_TRUE(store.bytes);
  EXPECT_TRUE(in_conflict(executor.health()));

  ASSERT_TRUE(healthy(reset_with(executor)));
  platform.on_create = [](std::size_t) -> dt::result_t<void> { throw not_a_std_exception_t {}; };
  const auto unknown = prepare_with(executor, virtual_profile());
  EXPECT_TRUE(verdict_is(unknown, native_verdict_e::conflict));
  EXPECT_EQ(unknown.reason, "Native prepare failed");
  EXPECT_TRUE(store.bytes);
  EXPECT_TRUE(in_conflict(executor.health()));
}

TEST_F(NativeExecutor, AnExceptionBeforeJournalingRefuses) {
  platform.throw_on_inventory = true;
  const auto outcome = prepare_with(executor, virtual_profile());
  EXPECT_TRUE(verdict_is(outcome, native_verdict_e::refused));
  EXPECT_TRUE(mentions(outcome.reason, "inventory exploded"));
  EXPECT_FALSE(store.bytes);
  EXPECT_TRUE(healthy(executor.health()));
}

// --- Restore at stream end ----------------------------------------------------------

namespace {
  void
  remove_display(dt::inventory_t &inventory, const std::string &name) {
    std::erase_if(inventory.displays, [&](const dt::observed_display_t &display) { return display.id.value == name; });
  }

  const dt::managed_values_t moved { .position = dt::position_t { 2560, 0 } };
}  // namespace

TEST_F(NativeExecutor, RestoreRemovesTheDisplayThisRunCreated) {
  ASSERT_TRUE(verdict_is(prepare_with(executor, virtual_profile()), native_verdict_e::ok));
  const auto outcome = restore_with(executor);
  ASSERT_TRUE(verdict_is(outcome, native_verdict_e::ok));
  EXPECT_FALSE(outcome.target);
  EXPECT_EQ(log.events, (std::vector<std::string> { "save", "create_head", "save", "remove_all_heads", "clear" }));
  EXPECT_EQ(platform.head_count(), 0u);
  EXPECT_FALSE(store.bytes);
  EXPECT_TRUE(healthy(executor.health()));

  // The next session starts over.
  ASSERT_TRUE(verdict_is(prepare_with(executor, virtual_profile()), native_verdict_e::ok));
  EXPECT_TRUE(verdict_is(restore_with(executor), native_verdict_e::ok));
  EXPECT_EQ(platform.head_count(), 0u);
  EXPECT_FALSE(store.bytes);
}

TEST_F(NativeExecutor, RestoreWithoutAJournalTouchesNothing) {
  EXPECT_TRUE(verdict_is(restore_with(executor), native_verdict_e::ok));
  EXPECT_EQ(platform.reads, 0);
  ASSERT_TRUE(verdict_is(prepare_with(executor, physical_profile("os-B")), native_verdict_e::ok));
  const auto reads = platform.reads;
  EXPECT_TRUE(verdict_is(restore_with(executor), native_verdict_e::ok));
  EXPECT_EQ(platform.reads, reads);
  EXPECT_EQ(store.saves, 0);
  EXPECT_EQ(log.host_writes(), 0u);
  EXPECT_TRUE(healthy(executor.health()));
}

TEST_F(NativeExecutor, OnlyTheExecutorThatCreatedTheDisplayRemovesIt) {
  ASSERT_TRUE(verdict_is(prepare_with(executor, virtual_profile()), native_verdict_e::ok));
  auto other = make_executor();
  const auto outcome = restore_with(other);
  EXPECT_TRUE(verdict_is(outcome, native_verdict_e::conflict));
  EXPECT_TRUE(mentions(outcome.reason, "earlier run"));
  EXPECT_FALSE(log.contains("remove_all_heads"));
  EXPECT_EQ(platform.head_count(), 1u);
  EXPECT_TRUE(store.bytes);

  EXPECT_TRUE(verdict_is(restore_with(executor), native_verdict_e::ok));
  EXPECT_EQ(platform.head_count(), 0u);
  EXPECT_FALSE(store.bytes);
}

TEST_F(NativeExecutor, AnyRunThatFindsNoJournalForgetsWhatItCreated) {
  // Once a run finds no journal, the same journal turning up again is an earlier run's,
  // so the display it names is left in place.
  const std::vector<std::pair<std::string, std::function<void()>>> forgets {
    { "restored, then the same display returned", [&] {
       const auto head = platform.host.displays.back().id.value;
       EXPECT_TRUE(verdict_is(restore_with(executor), native_verdict_e::ok));
       platform.host.displays.push_back(virtual_display(head, 3840));
     } },
    { "journal gone at restore", [&] {
       store.bytes.reset();
       EXPECT_TRUE(verdict_is(restore_with(executor), native_verdict_e::ok));
     } },
    { "journal gone at recovery", [&] {
       store.bytes.reset();
       EXPECT_TRUE(healthy(recover_with(executor)));
     } },
    { "journal gone at prepare", [&] {
       store.bytes.reset();
       const auto outcome = prepare_with(executor, virtual_profile());
       EXPECT_TRUE(verdict_is(outcome, native_verdict_e::refused));
       EXPECT_TRUE(mentions(outcome.reason, "already exists"));
     } },
  };
  for (const auto &[name, forget] : forgets) {
    SCOPED_TRACE(name);
    platform.host = host_inventory();
    ASSERT_TRUE(verdict_is(prepare_with(executor, virtual_profile()), native_verdict_e::ok));
    const auto bytes = store.bytes;
    forget();
    store.bytes = bytes;
    const auto outcome = restore_with(executor);
    EXPECT_TRUE(verdict_is(outcome, native_verdict_e::conflict));
    EXPECT_TRUE(mentions(outcome.reason, "earlier run"));
    EXPECT_EQ(platform.head_count(), 1u);
    ASSERT_TRUE(healthy(reset_with(executor)));
  }
}

TEST_F(NativeExecutor, ARewrittenJournalIsNotTrustedToRemoveTheDisplay) {
  const std::vector<std::pair<std::string, std::function<void(dt::journal_record_t &)>>> rewrites {
    { "another owner", [](dt::journal_record_t &record) {
       record.owner = id<dt::owner_id_t>(0x57);
       record.owned_heads.front().owner = record.owner;
     } },
    { "another transaction", [](dt::journal_record_t &record) { record.transaction = id<dt::transaction_id_t>(0x58); } },
    { "no owned display", [](dt::journal_record_t &record) { record.owned_heads.clear(); } },
  };
  for (const auto &[name, rewrite] : rewrites) {
    ASSERT_TRUE(verdict_is(prepare_with(executor, virtual_profile()), native_verdict_e::ok)) << name;
    auto record = store.record().value();
    rewrite(record);
    store.put(record);
    EXPECT_TRUE(verdict_is(restore_with(executor), native_verdict_e::conflict)) << name;
    EXPECT_FALSE(log.contains("remove_all_heads")) << name;
    EXPECT_EQ(platform.head_count(), 1u) << name;
    ASSERT_TRUE(healthy(reset_with(executor))) << name;
    platform.drop_heads();
  }
}

TEST_F(NativeExecutor, ADisplayThatIsNoLongerTheCreatedOneIsLeftInPlace) {
  ASSERT_TRUE(verdict_is(prepare_with(executor, virtual_profile()), native_verdict_e::ok));
  platform.add_head();
  auto outcome = restore_with(executor);
  EXPECT_TRUE(verdict_is(outcome, native_verdict_e::conflict));
  EXPECT_TRUE(mentions(outcome.reason, "no longer match"));

  // Only the second display remains: a replacement, not the one this run created.
  remove_display(platform.host, "vdd-1");
  outcome = restore_with(executor);
  EXPECT_TRUE(verdict_is(outcome, native_verdict_e::conflict));
  EXPECT_TRUE(mentions(outcome.reason, "no longer match"));
  EXPECT_FALSE(log.contains("remove_all_heads"));
  EXPECT_EQ(platform.head_count(), 1u);
  EXPECT_TRUE(store.bytes);
}

TEST_F(NativeExecutor, ADisplayAlreadyGoneIsNotRemovedAgain) {
  ASSERT_TRUE(verdict_is(prepare_with(executor, virtual_profile()), native_verdict_e::ok));
  platform.drop_heads();
  EXPECT_TRUE(verdict_is(restore_with(executor), native_verdict_e::ok));
  EXPECT_FALSE(log.contains("remove_all_heads"));
  EXPECT_FALSE(store.bytes);
}

TEST_F(NativeExecutor, AFailedOrUnconfirmedRemovalKeepsTheJournal) {
  ASSERT_TRUE(verdict_is(prepare_with(executor, virtual_profile()), native_verdict_e::ok));
  platform.on_remove = [] { return failure("remove refused"); };
  auto outcome = restore_with(executor);
  EXPECT_TRUE(verdict_is(outcome, native_verdict_e::conflict));
  EXPECT_TRUE(mentions(outcome.reason, "remove refused"));
  EXPECT_TRUE(store.bytes);

  platform.on_remove = [] { return dt::result_t<void> {}; };
  outcome = restore_with(executor);
  EXPECT_TRUE(verdict_is(outcome, native_verdict_e::conflict));
  EXPECT_TRUE(mentions(outcome.reason, "still present after its removal"));
  EXPECT_TRUE(store.bytes);

  platform.on_remove = [&]() -> dt::result_t<void> {
    platform.drop_heads();
    platform.host.available = false;
    return {};
  };
  outcome = restore_with(executor);
  EXPECT_TRUE(verdict_is(outcome, native_verdict_e::conflict));
  EXPECT_TRUE(mentions(outcome.reason, "unavailable after restoring"));
  EXPECT_TRUE(store.bytes);

  platform.host.available = true;
  EXPECT_TRUE(verdict_is(restore_with(executor), native_verdict_e::ok));
  EXPECT_FALSE(store.bytes);
  EXPECT_TRUE(healthy(executor.health()));
}

TEST_F(NativeExecutor, ADisplayChangedDuringTheSessionKeepsTheJournalAfterRemoval) {
  ASSERT_TRUE(verdict_is(prepare_with(executor, virtual_profile()), native_verdict_e::ok));
  find(platform.host, "os-B").info.device_state = state_e::inactive;
  const auto outcome = restore_with(executor);
  EXPECT_TRUE(verdict_is(outcome, native_verdict_e::conflict));
  EXPECT_TRUE(mentions(outcome.reason, "Other displays differ from the baseline after restoring"));
  EXPECT_EQ(platform.head_count(), 0u);
  EXPECT_TRUE(store.bytes);

  find(platform.host, "os-B").info.device_state = state_e::active;
  EXPECT_TRUE(verdict_is(restore_with(executor), native_verdict_e::ok));
  EXPECT_FALSE(store.bytes);
}

TEST_F(NativeExecutor, AJournalThatCannotBeRemovedStaysInConflict) {
  ASSERT_TRUE(verdict_is(prepare_with(executor, virtual_profile()), native_verdict_e::ok));
  store.fail_clear = true;
  const auto outcome = restore_with(executor);
  EXPECT_TRUE(verdict_is(outcome, native_verdict_e::conflict));
  EXPECT_TRUE(mentions(outcome.reason, "the native journal could not be removed: journal locked"));
  EXPECT_EQ(platform.head_count(), 0u);
  EXPECT_TRUE(in_conflict(executor.health()));
}

TEST_F(NativeExecutor, ARefusalWhoseJournalCannotBeRemovedIsInConflict) {
  platform.on_create = [](std::size_t) { return failure("driver busy"); };
  store.fail_clear = true;
  const auto outcome = prepare_with(executor, virtual_profile());
  EXPECT_TRUE(verdict_is(outcome, native_verdict_e::conflict));
  EXPECT_TRUE(mentions(outcome.reason, "No virtual display was created: driver busy; "));
  EXPECT_TRUE(mentions(outcome.reason, "journal locked"));
  EXPECT_TRUE(store.bytes);
  EXPECT_TRUE(in_conflict(executor.health()));
}

TEST_F(NativeExecutor, AnUnresolvedMarkerThatCannotBeSavedIsReported) {
  platform.on_create = [](std::size_t) -> dt::result_t<void> { return {}; };
  store.fail_save_call = 2;
  const auto outcome = prepare_with(executor, virtual_profile());
  EXPECT_TRUE(verdict_is(outcome, native_verdict_e::conflict));
  EXPECT_TRUE(mentions(outcome.reason, "accepted the request"));
  EXPECT_TRUE(mentions(outcome.reason, "could not record it: disk full"));
  EXPECT_TRUE(in_conflict(executor.health()));
}

// --- Startup recovery ---------------------------------------------------------------

TEST_F(NativeExecutor, RecoveryWithoutAJournalIsHealthyAndReadsNothing) {
  EXPECT_TRUE(healthy(recover_with(executor)));
  EXPECT_EQ(store.loads, 1);
  EXPECT_EQ(platform.reads, 0);
  EXPECT_TRUE(log.events.empty());
}

TEST_F(NativeExecutor, RecoveryNeverRemovesADisplayFromAnEarlierRun) {
  store.put(head_journal(step_e::observed));
  platform.add_head();
  const auto health = recover_with(executor);
  EXPECT_TRUE(in_conflict(health));
  EXPECT_TRUE(mentions(health.reason, "earlier run"));
  EXPECT_EQ(platform.head_count(), 1u);
  EXPECT_TRUE(store.bytes);

  const auto reads = platform.reads;
  EXPECT_TRUE(verdict_is(prepare_with(executor, virtual_profile()), native_verdict_e::conflict));
  EXPECT_EQ(platform.reads, reads);
  EXPECT_TRUE(verdict_is(restore_with(executor), native_verdict_e::conflict));
  EXPECT_EQ(platform.head_count(), 1u);
  EXPECT_EQ(log.host_writes(), 0u);
}

TEST_F(NativeExecutor, RecoveryClearsAnAttemptThatLeftNothingBehind) {
  // The OS may list the same displays in another order.
  std::reverse(platform.host.displays.begin(), platform.host.displays.end());
  for (const auto state : { step_e::planned, step_e::attempted, step_e::failed }) {
    store.put(head_journal(state));
    EXPECT_TRUE(healthy(recover_with(executor))) << static_cast<int>(state);
    EXPECT_FALSE(store.bytes) << static_cast<int>(state);
  }
  EXPECT_EQ(log.host_writes(), 0u);
  EXPECT_TRUE(verdict_is(prepare_with(executor, virtual_profile()), native_verdict_e::ok));
}

TEST_F(NativeExecutor, AVirtualDisplayStepThatNeverStartedClaimsNothing) {
  store.put(head_journal(step_e::planned));
  platform.add_head();
  EXPECT_TRUE(healthy(recover_with(executor)));
  EXPECT_FALSE(store.bytes);
  EXPECT_EQ(platform.head_count(), 1u);
  EXPECT_EQ(log.host_writes(), 0u);
}

TEST_F(NativeExecutor, RecoveryKeepsAnAttemptThatMayHaveLeftSomething) {
  // A display is present: it may be the attempt's, so it is neither removed nor adopted.
  store.put(head_journal(step_e::attempted));
  platform.add_head();
  auto health = recover_with(executor);
  EXPECT_TRUE(in_conflict(health));
  EXPECT_TRUE(mentions(health.reason, "earlier run"));
  EXPECT_EQ(platform.head_count(), 1u);

  // No display, but another display changed state: the attempt may have caused it.
  platform.drop_heads();
  find(platform.host, "os-B").info.device_state = state_e::inactive;
  health = recover_with(executor);
  EXPECT_TRUE(in_conflict(health));
  EXPECT_TRUE(mentions(health.reason, "Other displays differ from the baseline, so the native journal is kept"));

  platform.host.available = false;
  health = recover_with(executor);
  EXPECT_TRUE(in_conflict(health));
  EXPECT_TRUE(mentions(health.reason, "inventory is unavailable"));
  EXPECT_EQ(log.host_writes(), 0u);
  EXPECT_TRUE(store.bytes);
}

TEST_F(NativeExecutor, AnUnresolvedStepFromAnEarlierRunNeedsAReset) {
  store.put(head_journal(step_e::conflict));
  const auto health = recover_with(executor);
  EXPECT_TRUE(in_conflict(health));
  EXPECT_TRUE(mentions(health.reason, "left unresolved (fixture)"));
  EXPECT_TRUE(healthy(reset_with(executor)));
  EXPECT_FALSE(store.bytes);
  EXPECT_EQ(platform.reads, 0);
  EXPECT_EQ(log.host_writes(), 0u);
}

TEST_F(NativeExecutor, AnUnusableJournalIsKeptAndNothingIsRead) {
  store.bytes = "{not json";
  auto health = recover_with(executor);
  EXPECT_TRUE(in_conflict(health));
  EXPECT_TRUE(mentions(health.reason, "The native journal is invalid"));
  EXPECT_EQ(store.bytes.value_or(""), "{not json");
  EXPECT_TRUE(verdict_is(restore_with(executor), native_verdict_e::conflict));
  EXPECT_EQ(store.bytes.value_or(""), "{not json");

  auto bytes = dt::serialize_journal(head_journal(step_e::attempted)).value();
  const auto at = bytes.rfind("\"version\":1");
  ASSERT_NE(at, std::string::npos);
  bytes.replace(at, 11, "\"version\":2");
  store.bytes = bytes;
  health = recover_with(executor);
  EXPECT_TRUE(in_conflict(health));
  EXPECT_TRUE(mentions(health.reason, "The native journal is unsupported_version"));
  EXPECT_EQ(store.bytes.value_or(""), bytes);

  store.throw_on_load = true;
  health = recover_with(executor);
  EXPECT_TRUE(in_conflict(health));
  EXPECT_TRUE(mentions(health.reason, "Native recovery failed: load exploded"));
  EXPECT_TRUE(verdict_is(restore_with(executor), native_verdict_e::conflict));
  EXPECT_EQ(platform.reads, 0);
  EXPECT_EQ(log.host_writes(), 0u);
}

TEST_F(NativeExecutor, AnyExceptionDuringRecoveryOrRestoreKeepsTheJournal) {
  // The journal would be cleared, but clearing throws something that is not a std::exception.
  store.put(head_journal(step_e::attempted));
  store.throw_on_clear = true;
  const auto health = recover_with(executor);
  EXPECT_TRUE(in_conflict(health));
  EXPECT_EQ(health.reason, "Native recovery failed");
  const auto outcome = restore_with(executor);
  EXPECT_TRUE(verdict_is(outcome, native_verdict_e::conflict));
  EXPECT_EQ(outcome.reason, "Native restore failed");
  EXPECT_TRUE(store.bytes);
  EXPECT_EQ(log.host_writes(), 0u);
}

TEST_F(NativeExecutor, AConflictEndsOnceTheJournalTurnsOutToBeAbsent) {
  // Only loading failed; there was never a journal.
  store.throw_on_load = true;
  EXPECT_TRUE(in_conflict(recover_with(executor)));
  store.throw_on_load = false;
  EXPECT_TRUE(healthy(recover_with(executor)));

  store.throw_on_load = true;
  EXPECT_TRUE(verdict_is(restore_with(executor), native_verdict_e::conflict));
  EXPECT_TRUE(in_conflict(executor.health()));
  store.throw_on_load = false;
  EXPECT_TRUE(verdict_is(restore_with(executor), native_verdict_e::ok));
  EXPECT_TRUE(healthy(executor.health()));

  // A refusal also finds no journal.
  store.throw_on_load = true;
  EXPECT_TRUE(verdict_is(prepare_with(executor, physical_profile("os-B")), native_verdict_e::conflict));
  EXPECT_TRUE(in_conflict(executor.health()));
  store.throw_on_load = false;
  EXPECT_TRUE(verdict_is(prepare_with(executor, physical_profile("os-C")), native_verdict_e::refused));
  EXPECT_TRUE(healthy(executor.health()));
}

TEST_F(NativeExecutor, AJournalWithImpossibleRelationsIsNeverActedOn) {
  struct case_t {
    std::string name;
    dt::journal_record_t record;
    std::string problem;
  };
  std::vector<case_t> cases;
  {
    auto record = field_journal({ managed("os-B", moved) }, { { "os-B", step_e::attempted } });
    record.steps[0].display.reset();
    cases.push_back({ "field step without a display", record, "a field step does not name a managed display" });
  }
  cases.push_back({ "field step for an unmanaged display",
    field_journal({ managed("os-B", moved) }, { { "os-A", step_e::attempted } }), "a field step does not name a managed display" });
  {
    auto record = head_journal(step_e::attempted);
    record.steps[0].head_intent.reset();
    cases.push_back({ "create without a request identity", record, "a virtual display step has no request identity" });
  }
  {
    auto record = head_journal(step_e::observed);
    record.steps[0].display.reset();
    cases.push_back({ "observed create without a display", record, "an observed virtual display step names no display" });
  }
  {
    auto record = head_journal(step_e::attempted);
    record.steps.push_back({ 2, dt::journal_operation_e::remove_head, dt::os_display_id_t { "vdd-1" }, std::nullopt, step_e::attempted,
      std::nullopt, {} });
    cases.push_back({ "remove step", record, "it contains steps this version never writes" });
  }
  {
    auto record = field_journal({ managed("os-B", moved) }, { { "os-B", step_e::attempted } });
    record.steps.push_back({ 2, dt::journal_operation_e::restore_fields, dt::os_display_id_t { "os-B" }, std::nullopt,
      step_e::attempted, std::nullopt, {} });
    cases.push_back({ "restore step", record, "it contains steps this version never writes" });
  }
  {
    auto record = head_journal(step_e::attempted);
    record.owned_heads.push_back({ { driver(), id<dt::head_guid_t>(0x54), dt::head_incarnation_t { 1 } }, record.owner,
      dt::adapter_luid_t { 7 }, 0 });
    cases.push_back({ "owned display of an unobserved create", record, "an owned virtual display has no observed creation" });
  }
  {
    auto record = head_journal(step_e::observed);
    record.owned_heads.front().identity.guid = id<dt::head_guid_t>(0x55);
    cases.push_back({ "owned display of another request", record, "an owned virtual display has no observed creation" });
  }

  // Each journal would otherwise restore os-B or remove vdd-1.
  find(platform.host, "os-B").layout->position = { 2560, 0 };
  platform.add_head();
  for (const auto &[name, record, problem] : cases) {
    store.put(record);
    const auto health = recover_with(executor);
    EXPECT_TRUE(in_conflict(health)) << name;
    EXPECT_TRUE(mentions(health.reason, "cannot be reconciled: " + problem)) << name;
    EXPECT_TRUE(store.bytes) << name;
  }
  EXPECT_EQ(platform.reads, 0);
  EXPECT_EQ(log.host_writes(), 0u);
}

// --- Replay of field writes ---------------------------------------------------------

namespace {
  struct field_case_t {
    std::string field;
    dt::managed_values_t intended;
    /** Puts os-B at the intended value. */
    std::function<void(dt::observed_display_t &)> at_intended;
    /** Puts os-B at a value neither the baseline nor the intent holds; empty when none exists. */
    std::function<void(dt::observed_display_t &)> elsewhere;
  };

  std::vector<field_case_t>
  field_cases() {
    using hdr_e = display_device::hdr_state_e;
    return {
      { "position", { .position = dt::position_t { 2560, 0 } }, [](auto &display) { display.layout->position = { 2560, 0 }; },
        [](auto &display) { display.layout->position = { 3000, 0 }; } },
      { "mode", { .mode = mode(2560, 1440) }, [](auto &display) { display.layout->mode = mode(2560, 1440); },
        [](auto &display) { display.layout->mode = mode(1280, 720); } },
      { "rotation", { .rotation = dt::rotation_e::clockwise90 }, [](auto &display) { display.layout->rotation = dt::rotation_e::clockwise90; },
        [](auto &display) { display.layout->rotation = dt::rotation_e::clockwise180; } },
      { "scale", { .scale_percent = 150u }, [](auto &display) { display.layout->scale_percent = 150u; },
        [](auto &display) { display.layout->scale_percent = 125u; } },
      { "HDR", { .hdr = hdr_e::enabled }, [](auto &display) { display.layout->hdr = hdr_e::enabled; },
        [](auto &display) { display.layout->hdr = hdr_e::unknown; } },
      { "primary", { .primary = true }, [](auto &display) { display.info.device_state = state_e::primary; }, {} },
    };
  }

  /** The baseline value of the one field `intended` names, for os-B of the default host. */
  dt::managed_values_t
  baseline_of(const dt::managed_values_t &intended) {
    return managed("os-B", intended).baseline;
  }
}  // namespace

TEST_F(NativeExecutor, EachInterruptedFieldWriteIsUndoneExactly) {
  for (const auto &field : field_cases()) {
    platform.host = host_inventory();
    platform.applied.clear();
    store.put(field_journal({ managed("os-B", field.intended) }, { { "os-B", step_e::attempted } }));
    field.at_intended(find(platform.host, "os-B"));

    EXPECT_TRUE(healthy(recover_with(executor))) << field.field;
    ASSERT_EQ(platform.applied.size(), 1u) << field.field;
    EXPECT_EQ(platform.applied[0].first.value, "os-B") << field.field;
    EXPECT_TRUE(platform.applied[0].second == baseline_of(field.intended)) << field.field;
    EXPECT_TRUE(platform.host.displays == host_inventory().displays) << field.field;
    EXPECT_FALSE(store.bytes) << field.field;
  }
}

TEST_F(NativeExecutor, AFieldWriteThatNeverTookEffectWritesNothing) {
  for (const auto &field : field_cases()) {
    store.put(field_journal({ managed("os-B", field.intended) }, { { "os-B", step_e::attempted } }));
    EXPECT_TRUE(healthy(recover_with(executor))) << field.field;
    EXPECT_FALSE(store.bytes) << field.field;
  }
  EXPECT_EQ(log.host_writes(), 0u);
  EXPECT_EQ(platform.reads, static_cast<int>(field_cases().size()));
}

TEST_F(NativeExecutor, APartlyRevertedWriteRestoresOnlyWhatIsLeft) {
  const dt::managed_values_t intended { .position = dt::position_t { 2560, 0 }, .mode = mode(2560, 1440) };
  store.put(field_journal({ managed("os-B", intended) }, { { "os-B", step_e::attempted } }));
  find(platform.host, "os-B").layout->position = { 2560, 0 };
  EXPECT_TRUE(healthy(recover_with(executor)));
  ASSERT_EQ(platform.applied.size(), 1u);
  EXPECT_TRUE((platform.applied[0].second == dt::managed_values_t { .position = dt::position_t { 1920, 0 } }));
  EXPECT_TRUE(platform.host.displays == host_inventory().displays);
}

TEST_F(NativeExecutor, AnExternalEditStopsEveryWrite) {
  for (const auto &field : field_cases()) {
    if (!field.elsewhere) {
      continue;
    }
    platform.host = host_inventory();
    // os-A alone could be restored, but nothing is written once any field conflicts.
    const dt::managed_values_t bigger { .mode = mode(2560, 1440) };
    store.put(field_journal({ managed("os-A", bigger), managed("os-B", field.intended) },
      { { "os-A", step_e::attempted }, { "os-B", step_e::attempted } }));
    find(platform.host, "os-A").layout->mode = mode(2560, 1440);
    field.elsewhere(find(platform.host, "os-B"));

    const auto health = recover_with(executor);
    EXPECT_TRUE(in_conflict(health)) << field.field;
    EXPECT_TRUE(mentions(health.reason, "The " + field.field + " of display os-B was changed by something other than this host"))
      << field.field;
    EXPECT_TRUE(store.bytes) << field.field;
  }
  EXPECT_TRUE(platform.applied.empty());
  EXPECT_EQ(log.host_writes(), 0u);
}

TEST_F(NativeExecutor, AnEditToAnotherFieldOfTheSameDisplayStopsEveryWrite) {
  const dt::managed_values_t intended { .position = dt::position_t { 2560, 0 }, .mode = mode(2560, 1440) };
  store.put(field_journal({ managed("os-B", intended) }, { { "os-B", step_e::attempted } }));
  auto &os_b = find(platform.host, "os-B");
  os_b.layout->position = { 2560, 0 };
  os_b.layout->mode = mode(1280, 720);
  const auto health = recover_with(executor);
  EXPECT_TRUE(in_conflict(health));
  EXPECT_TRUE(mentions(health.reason, "The mode of display os-B"));
  EXPECT_EQ(log.host_writes(), 0u);
}

TEST_F(NativeExecutor, AConfirmedReadbackIsWhatThisHostLeft) {
  // The OS adjusted the requested position; the mode was never read back.
  const dt::managed_values_t intended { .position = dt::position_t { 2560, 0 }, .mode = mode(2560, 1440) };
  const dt::managed_values_t confirmed { .position = dt::position_t { 2600, 0 } };
  const auto record = field_journal({ managed("os-B", intended, confirmed) }, { { "os-B", step_e::observed } });
  store.put(record);
  auto &os_b = find(platform.host, "os-B");
  os_b.layout->position = { 2600, 0 };
  os_b.layout->mode = mode(2560, 1440);
  EXPECT_TRUE(healthy(recover_with(executor)));
  ASSERT_EQ(platform.applied.size(), 1u);
  EXPECT_TRUE(platform.applied[0].second == baseline_of(intended));
  EXPECT_TRUE(platform.host.displays == host_inventory().displays);

  // At the intended position instead: something moved it after this host read it back.
  store.put(record);
  find(platform.host, "os-B").layout->position = { 2560, 0 };
  find(platform.host, "os-B").layout->mode = mode(2560, 1440);
  const auto health = recover_with(executor);
  EXPECT_TRUE(in_conflict(health));
  EXPECT_TRUE(mentions(health.reason, "The position of display os-B was changed"));
  EXPECT_EQ(platform.applied.size(), 1u);
}

TEST_F(NativeExecutor, OnlyDisplaysWhoseWriteStartedAreRestored) {
  const dt::managed_values_t bigger { .mode = mode(2560, 1440) };
  store.put(field_journal({ managed("os-A", bigger), managed("os-B", moved) }, { { "os-A", step_e::failed }, { "os-B", step_e::planned } }));
  find(platform.host, "os-A").layout->mode = mode(2560, 1440);
  // This host never wrote os-B, so a change there is someone else's and is kept.
  find(platform.host, "os-B").layout->position = { 3000, 0 };
  find(platform.host, "os-B").info.device_state = state_e::inactive;
  EXPECT_TRUE(healthy(recover_with(executor)));
  ASSERT_EQ(platform.applied.size(), 1u);
  EXPECT_EQ(platform.applied[0].first.value, "os-A");
  EXPECT_TRUE(platform.applied[0].second == (dt::managed_values_t { .mode = mode(1920, 1080) }));
  EXPECT_TRUE(find(platform.host, "os-B").layout->position == (dt::position_t { 3000, 0 }));
  EXPECT_EQ(find(platform.host, "os-B").info.device_state, state_e::inactive);
  EXPECT_FALSE(store.bytes);
}

TEST_F(NativeExecutor, AFieldThisHostNeverAskedForIsLeftAlone) {
  const dt::managed_values_t bigger { .mode = mode(2560, 1440) };
  store.put(field_journal({ managed("os-B", bigger) }, { { "os-B", step_e::attempted } }));
  find(platform.host, "os-B").layout->mode = mode(2560, 1440);
  find(platform.host, "os-B").layout->position = { 3000, 0 };
  EXPECT_TRUE(healthy(recover_with(executor)));
  ASSERT_EQ(platform.applied.size(), 1u);
  EXPECT_TRUE(platform.applied[0].second == (dt::managed_values_t { .mode = mode(1920, 1080) }));
  EXPECT_TRUE(find(platform.host, "os-B").layout->position == (dt::position_t { 3000, 0 }));
}

TEST_F(NativeExecutor, AVirtualDisplayThisHostNeverAskedForDoesNotBlockFieldRestores) {
  store.put(field_journal({ managed("os-B", moved) }, { { "os-B", step_e::attempted } }));
  find(platform.host, "os-B").layout->position = { 2560, 0 };
  platform.add_head();
  EXPECT_TRUE(healthy(recover_with(executor)));
  EXPECT_EQ(log.events, (std::vector<std::string> { "apply_fields:os-B", "clear" }));
  EXPECT_EQ(platform.head_count(), 1u);
}

TEST_F(NativeExecutor, AManagedDisplayThatIsNoLongerTheSameStopsEveryWrite) {
  const std::vector<std::pair<std::string, std::function<void(dt::inventory_t &)>>> changes {
    { "missing", [](dt::inventory_t &inventory) { remove_display(inventory, "os-B"); } },
    { "duplicated", [](dt::inventory_t &inventory) {
       const auto copy = find(inventory, "os-B");
       inventory.displays.push_back(copy);
     } },
    { "reconnected", [](dt::inventory_t &inventory) { find(inventory, "os-B").incarnation = dt::source_incarnation_t { 2 }; } },
    { "another adapter", [](dt::inventory_t &inventory) { find(inventory, "os-B").adapter = dt::adapter_luid_t { 8 }; } },
    { "no adapter", [](dt::inventory_t &inventory) { find(inventory, "os-B").adapter.reset(); } },
    { "now a head", [](dt::inventory_t &inventory) {
       find(inventory, "os-B").head = dt::head_identity_t { driver(), id<dt::head_guid_t>(0x59), dt::head_incarnation_t { 1 } };
     } },
    { "no geometry", [](dt::inventory_t &inventory) {
       auto &display = find(inventory, "os-B");
       display.info.device_state = state_e::inactive;
       display.layout.reset();
     } },
  };
  const dt::managed_values_t bigger { .mode = mode(2560, 1440) };
  for (const auto &[name, change] : changes) {
    platform.host = host_inventory();
    store.put(field_journal({ managed("os-A", bigger), managed("os-B", moved) },
      { { "os-A", step_e::attempted }, { "os-B", step_e::attempted } }));
    find(platform.host, "os-A").layout->mode = mode(2560, 1440);
    find(platform.host, "os-B").layout->position = { 2560, 0 };
    change(platform.host);
    const auto health = recover_with(executor);
    EXPECT_TRUE(in_conflict(health)) << name;
    EXPECT_TRUE(mentions(health.reason, "Managed display os-B is missing or is no longer the display this host changed")) << name;
  }
  EXPECT_EQ(log.host_writes(), 0u);

  // A managed virtual display stays bound to the head it was when this host changed it.
  const dt::head_identity_t head { driver(), id<dt::head_guid_t>(0x5A), dt::head_incarnation_t { 1 } };
  const auto recover_head_bound = [&](const std::optional<dt::head_identity_t> &now) {
    platform.host = host_inventory();
    auto record = field_journal({ managed("os-B", moved) }, { { "os-B", step_e::attempted } });
    find(record.baseline, "os-B").head = head;
    record.managed.front().source.head = dt::owned_head_t { head, record.owner, dt::adapter_luid_t { 7 }, 0 };
    store.put(record);
    auto &display = find(platform.host, "os-B");
    display.layout->position = { 2560, 0 };
    display.head = now;
    return recover_with(executor);
  };
  const std::vector<std::pair<std::string, std::optional<dt::head_identity_t>>> heads {
    { "no longer a head", std::nullopt },
    { "another head", dt::head_identity_t { driver(), id<dt::head_guid_t>(0x5B), dt::head_incarnation_t { 1 } } },
    { "recreated head", dt::head_identity_t { driver(), id<dt::head_guid_t>(0x5A), dt::head_incarnation_t { 2 } } },
  };
  for (const auto &[name, now] : heads) {
    const auto health = recover_head_bound(now);
    EXPECT_TRUE(in_conflict(health)) << name;
    EXPECT_TRUE(mentions(health.reason, "Managed display os-B is missing or is no longer the display this host changed")) << name;
  }
  EXPECT_EQ(log.host_writes(), 0u);
  EXPECT_TRUE(healthy(recover_head_bound(head)));
  EXPECT_EQ(log.events, (std::vector<std::string> { "apply_fields:os-B", "clear" }));
}

TEST_F(NativeExecutor, ARestoreThatFailsOrDoesNotTakeIsInConflict) {
  struct case_t {
    std::string name;
    std::function<void()> arrange;
    std::string reason;
  };
  const std::vector<case_t> cases {
    { "refused", [&] { platform.apply_error = true; }, "Restoring display os-B failed: apply refused" },
    { "ignored", [&] { platform.ignore_apply = true; }, "The position of display os-B did not return to its baseline" },
    { "display lost", [&] { platform.after_apply = [&] { remove_display(platform.host, "os-B"); }; },
      "Display os-B is missing after it was restored" },
    { "inventory lost", [&] { platform.after_apply = [&] { platform.host.available = false; }; },
      "The display inventory is unavailable after restoring" },
    { "another display changed", [&] { platform.after_apply = [&] { find(platform.host, "os-A").info.device_state = state_e::inactive; }; },
      "Other displays differ from the baseline after restoring" },
  };
  for (const auto &[name, arrange, reason] : cases) {
    platform.host = host_inventory();
    platform.apply_error = platform.ignore_apply = false;
    platform.after_apply = nullptr;
    store.put(field_journal({ managed("os-B", moved) }, { { "os-B", step_e::attempted } }));
    find(platform.host, "os-B").layout->position = { 2560, 0 };
    arrange();
    const auto health = recover_with(executor);
    EXPECT_TRUE(in_conflict(health)) << name;
    EXPECT_TRUE(mentions(health.reason, reason)) << name;
    EXPECT_TRUE(store.bytes) << name;
  }
}

TEST_F(NativeExecutor, AForeignDisplayStopsFieldRestoresToo) {
  auto record = field_journal({ managed("os-B", moved) }, { { "os-B", step_e::attempted } });
  record.steps.push_back({ 2, dt::journal_operation_e::create_head, std::nullopt, id<dt::head_guid_t>(0x54), step_e::attempted,
    std::nullopt, {} });
  store.put(record);
  find(platform.host, "os-B").layout->position = { 2560, 0 };
  platform.add_head();
  const auto health = recover_with(executor);
  EXPECT_TRUE(in_conflict(health));
  EXPECT_TRUE(mentions(health.reason, "earlier run"));
  EXPECT_EQ(log.host_writes(), 0u);
}

namespace {
  /** What a later phase leaves after writing os-B for this run's virtual display. */
  void
  add_field_write(memory_store_t &store) {
    auto record = store.record().value();
    record.managed.push_back(managed("os-B", moved));
    record.steps.push_back({ record.steps.back().sequence + 1, dt::journal_operation_e::apply_fields, dt::os_display_id_t { "os-B" },
      std::nullopt, step_e::attempted, std::nullopt, {} });
    store.put(record);
  }
}  // namespace

TEST_F(NativeExecutor, SameRunFieldsAreRestoredBeforeItsDisplayIsRemoved) {
  ASSERT_TRUE(verdict_is(prepare_with(executor, virtual_profile()), native_verdict_e::ok));
  add_field_write(store);
  find(platform.host, "os-B").layout->position = { 2560, 0 };
  EXPECT_TRUE(verdict_is(restore_with(executor), native_verdict_e::ok));
  EXPECT_EQ(log.events,
    (std::vector<std::string> { "save", "create_head", "save", "apply_fields:os-B", "remove_all_heads", "clear" }));
  EXPECT_TRUE(platform.host.displays == host_inventory().displays);
  EXPECT_FALSE(store.bytes);
}

TEST_F(NativeExecutor, ADisplayNotProvenAfterRestoringFieldsIsLeftInPlace) {
  struct case_t {
    std::string name;
    std::function<void()> disturb;
    std::size_t heads;
  };
  const std::vector<case_t> disturbances {
    { "replaced", [&] {
       platform.drop_heads();
       platform.add_head();
     },
      1 },
    { "joined", [&] { platform.add_head(); }, 2 },
    { "inventory lost", [&] { platform.host.available = false; }, 1 },
  };
  for (const auto &[name, disturb, heads] : disturbances) {
    platform.host = host_inventory();
    platform.after_apply = nullptr;
    ASSERT_TRUE(verdict_is(prepare_with(executor, virtual_profile()), native_verdict_e::ok)) << name;
    add_field_write(store);
    find(platform.host, "os-B").layout->position = { 2560, 0 };
    platform.after_apply = disturb;
    const auto outcome = restore_with(executor);
    EXPECT_TRUE(verdict_is(outcome, native_verdict_e::conflict)) << name;
    EXPECT_TRUE(mentions(outcome.reason, "could not be proven to be this host's after restoring fields")) << name;
    EXPECT_FALSE(log.contains("remove_all_heads")) << name;
    EXPECT_EQ(platform.head_count(), heads) << name;
    ASSERT_TRUE(healthy(reset_with(executor))) << name;
  }
}

// --- Reset ---------------------------------------------------------------------------

TEST_F(NativeExecutor, ResetForgetsTheJournalWithoutTouchingTheHost) {
  ASSERT_TRUE(verdict_is(prepare_with(executor, virtual_profile()), native_verdict_e::ok));
  const auto created = store.bytes.value();
  const auto reads = platform.reads;
  EXPECT_TRUE(healthy(reset_with(executor)));
  EXPECT_TRUE(healthy(executor.health()));
  EXPECT_FALSE(store.bytes);
  EXPECT_EQ(platform.reads, reads);
  EXPECT_EQ(platform.head_count(), 1u);

  // Even the same journal, should it reappear, no longer proves the display is this run's. This runs
  // before any journal-less call, which would forget the display on its own and hide a reset that did not.
  store.bytes = created;
  const auto again = restore_with(executor);
  EXPECT_TRUE(verdict_is(again, native_verdict_e::conflict));
  EXPECT_TRUE(mentions(again.reason, "earlier run"));
  EXPECT_EQ(platform.head_count(), 1u);

  // The display is the user's now: the session end leaves it and a new session does not adopt it.
  EXPECT_TRUE(healthy(reset_with(executor)));
  EXPECT_TRUE(verdict_is(restore_with(executor), native_verdict_e::ok));
  EXPECT_EQ(platform.head_count(), 1u);
  const auto outcome = prepare_with(executor, virtual_profile());
  EXPECT_TRUE(verdict_is(outcome, native_verdict_e::refused));
  EXPECT_TRUE(mentions(outcome.reason, "A virtual display already exists"));
  EXPECT_EQ(log.host_writes(), 1u);
}

TEST_F(NativeExecutor, AResetThatCannotRemoveTheJournalStaysInConflict) {
  store.put(head_journal(step_e::conflict));
  store.fail_clear = true;
  auto health = reset_with(executor);
  EXPECT_TRUE(in_conflict(health));
  EXPECT_EQ(health.reason, "The native journal could not be removed: journal locked");

  store.fail_clear = false;
  store.throw_std_on_clear = true;
  health = reset_with(executor);
  EXPECT_TRUE(in_conflict(health));
  EXPECT_EQ(health.reason, "The native journal could not be removed: clear exploded");

  store.throw_std_on_clear = false;
  store.throw_on_clear = true;
  health = reset_with(executor);
  EXPECT_TRUE(in_conflict(health));
  EXPECT_EQ(health.reason, "The native journal could not be removed");
  EXPECT_TRUE(in_conflict(executor.health()));
  EXPECT_TRUE(store.bytes);
  EXPECT_EQ(platform.reads, 0);
}

// --- The journal file ---------------------------------------------------------------

namespace {
  namespace fs = std::filesystem;

  std::string
  read_file(const fs::path &file) {
    std::ifstream in(file, std::ios::binary);
    return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
  }

  void
  write_file(const fs::path &file, const std::string &bytes) {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << bytes;
    if (!out) {
      throw std::runtime_error { "cannot write fixture file " + file.string() };
    }
  }

  std::string
  serialized(const dt::journal_record_t &record) {
    return dt::serialize_journal(record).value();
  }

  class JournalFile: public ::testing::Test {
  protected:
    void
    SetUp() override {
      static std::atomic<int> counter { 0 };
      directory = fs::temp_directory_path() /
                  ("sunshine-native-journal-" + std::string { ::testing::UnitTest::GetInstance()->current_test_info()->name() } + "-" +
                    std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(++counter));
      fs::create_directories(directory);
      path = directory / "native_topology_journal.json";
    }

    void
    TearDown() override {
      // A test may leave a read-only file, which Windows will not delete.
      std::error_code error;
      for (fs::recursive_directory_iterator it(directory, error), end; !error && it != end; it.increment(error)) {
        std::error_code ignored;
        fs::permissions(it->path(), fs::perms::owner_write, fs::perm_options::add, ignored);
      }
      fs::remove_all(directory, error);
    }

    dt::native_executor_t
    executor_over(dt::journal_store_t &store) {
      return dt::native_executor_t { store, platform, { next_uuid, 2, driver() } };
    }

    fs::path directory;
    fs::path path;
    event_log_t log;
    fake_platform_t platform { log };
  };
}  // namespace

TEST_F(JournalFile, ASavedJournalLoadsBackExactly) {
  dt::file_journal_store_t store { path };
  EXPECT_EQ(store.path(), path);
  EXPECT_EQ(store.temp_path(), directory / "native_topology_journal.json.tmp");
  const auto record = head_journal(step_e::observed);
  const auto saved = store.save(record);
  ASSERT_TRUE(saved) << saved.error().detail;
  EXPECT_EQ(read_file(path), serialized(record));
  EXPECT_FALSE(fs::exists(store.temp_path()));

  const auto loaded = store.load();
  ASSERT_EQ(loaded.status, dt::journal_load_status_e::loaded) << loaded.detail;
  ASSERT_TRUE(loaded.record);
  EXPECT_EQ(serialized(*loaded.record), serialized(record));
  EXPECT_TRUE(loaded.detail.empty());
}

TEST_F(JournalFile, AMissingJournalIsAbsent) {
  dt::file_journal_store_t store { path };
  const auto loaded = store.load();
  EXPECT_EQ(loaded.status, dt::journal_load_status_e::absent);
  EXPECT_FALSE(loaded.present());
  EXPECT_FALSE(loaded.record);
  EXPECT_TRUE(store.clear());
  EXPECT_TRUE(fs::is_empty(directory));
}

TEST_F(JournalFile, AnUnusableJournalIsReportedAndKept) {
  dt::file_journal_store_t store { path };
  write_file(path, "{not json");
  auto loaded = store.load();
  EXPECT_EQ(loaded.status, dt::journal_load_status_e::invalid);
  EXPECT_TRUE(loaded.present());
  EXPECT_FALSE(loaded.record);
  EXPECT_FALSE(loaded.detail.empty());
  EXPECT_EQ(read_file(path), "{not json");

  auto bytes = serialized(head_journal(step_e::attempted));
  const auto at = bytes.rfind("\"version\":1");
  ASSERT_NE(at, std::string::npos);
  bytes.replace(at, 11, "\"version\":2");
  write_file(path, bytes);
  loaded = store.load();
  EXPECT_EQ(loaded.status, dt::journal_load_status_e::unsupported_version);
  EXPECT_EQ(read_file(path), bytes);

  // Valid JSON, but longer than any journal the store writes; it must be read far enough to tell.
  write_file(path, serialized(head_journal(step_e::attempted)) + std::string(1024 * 1024, ' '));
  loaded = store.load();
  EXPECT_EQ(loaded.status, dt::journal_load_status_e::invalid);
  EXPECT_TRUE(mentions(loaded.detail, "byte bound"));
}

TEST_F(JournalFile, ALeftoverTempFileIsAnUnfinishedSave) {
  dt::file_journal_store_t store { path };
  write_file(store.temp_path(), "half a jour");
  EXPECT_EQ(store.load().status, dt::journal_load_status_e::absent);

  const auto first = head_journal(step_e::attempted);
  ASSERT_TRUE(store.save(first));
  write_file(store.temp_path(), "half a jour");
  const auto loaded = store.load();
  ASSERT_EQ(loaded.status, dt::journal_load_status_e::loaded) << loaded.detail;
  EXPECT_EQ(serialized(*loaded.record), serialized(first));

  const auto second = head_journal(step_e::observed);
  ASSERT_TRUE(store.save(second));
  EXPECT_EQ(read_file(path), serialized(second));
  EXPECT_FALSE(fs::exists(store.temp_path()));
}

TEST_F(JournalFile, AnInvalidRecordNeverReplacesTheJournal) {
  dt::file_journal_store_t store { path };
  const auto first = head_journal(step_e::attempted);
  ASSERT_TRUE(store.save(first));
  auto invalid = head_journal(step_e::observed);
  invalid.owner = dt::owner_id_t {};
  EXPECT_TRUE(error_is(store.save(invalid), dt::error_code_e::invalid_journal));
  EXPECT_EQ(read_file(path), serialized(first));
  EXPECT_FALSE(fs::exists(store.temp_path()));
}

TEST_F(JournalFile, ClearRemovesTheJournalAndAnyTempFile) {
  dt::file_journal_store_t store { path };
  ASSERT_TRUE(store.save(head_journal(step_e::attempted)));
  write_file(store.temp_path(), "half a jour");
  EXPECT_TRUE(store.clear());
  EXPECT_TRUE(fs::is_empty(directory));
  EXPECT_EQ(store.load().status, dt::journal_load_status_e::absent);

  write_file(store.temp_path(), "half a jour");
  EXPECT_TRUE(store.clear());
  EXPECT_TRUE(fs::is_empty(directory));
}

TEST_F(JournalFile, SomethingThatIsNotAJournalFileIsUnreadable) {
  fs::create_directory(path);
  dt::file_journal_store_t store { path };
  const auto loaded = store.load();
  EXPECT_EQ(loaded.status, dt::journal_load_status_e::unreadable);
  EXPECT_TRUE(loaded.present());
  EXPECT_FALSE(loaded.record);
  EXPECT_TRUE(mentions(loaded.detail, "the journal file"));
}

TEST_F(JournalFile, ASaveThatCannotStartKeepsThePreviousJournal) {
  dt::file_journal_store_t store { path };
  const auto first = head_journal(step_e::attempted);
  ASSERT_TRUE(store.save(first));
  fs::create_directory(store.temp_path());
  const auto saved = store.save(head_journal(step_e::observed));
  ASSERT_FALSE(saved);
  EXPECT_TRUE(mentions(saved.error().detail, "Cannot create the journal temp file"));
  EXPECT_EQ(read_file(path), serialized(first));
}

TEST_F(JournalFile, AClearThatLeavesSomethingBehindFails) {
  dt::file_journal_store_t journal_blocked { directory / "a.json" };
  fs::create_directory(journal_blocked.path());
  write_file(journal_blocked.path() / "blocker", "x");
  auto cleared = journal_blocked.clear();
  ASSERT_FALSE(cleared);
  EXPECT_TRUE(mentions(cleared.error().detail, "Cannot remove the journal file: "));

  dt::file_journal_store_t temp_blocked { directory / "b.json" };
  ASSERT_TRUE(temp_blocked.save(head_journal(step_e::attempted)));
  fs::create_directory(temp_blocked.temp_path());
  write_file(temp_blocked.temp_path() / "blocker", "x");
  cleared = temp_blocked.clear();
  ASSERT_FALSE(cleared);
  EXPECT_TRUE(mentions(cleared.error().detail, "Cannot remove the journal temp file: "));
  EXPECT_FALSE(fs::exists(temp_blocked.path()));
}

#ifdef _WIN32
TEST_F(JournalFile, AJournalThatCannotBeReplacedIsKept) {
  dt::file_journal_store_t store { path };
  const auto first = head_journal(step_e::attempted);
  ASSERT_TRUE(store.save(first));
  fs::permissions(path, fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write, fs::perm_options::remove);
  const auto saved = store.save(head_journal(step_e::observed));
  ASSERT_FALSE(saved);
  EXPECT_TRUE(mentions(saved.error().detail, "Cannot replace the journal file"));
  EXPECT_EQ(read_file(path), serialized(first));
  EXPECT_FALSE(fs::exists(store.temp_path()));
}
#endif

TEST_F(JournalFile, AnUnusableFileStopsRecovery) {
  write_file(path, "{not json");
  dt::file_journal_store_t store { path };
  auto executor = executor_over(store);
  const auto health = recover_with(executor);
  EXPECT_TRUE(in_conflict(health));
  EXPECT_TRUE(mentions(health.reason, "The native journal is invalid"));
  EXPECT_EQ(read_file(path), "{not json");
  EXPECT_EQ(platform.reads, 0);
}

TEST_F(JournalFile, AVirtualSessionLeavesNoJournalBehind) {
  dt::file_journal_store_t store { path };
  auto executor = executor_over(store);
  ASSERT_TRUE(verdict_is(prepare_with(executor, virtual_profile()), native_verdict_e::ok));
  const auto loaded = store.load();
  ASSERT_EQ(loaded.status, dt::journal_load_status_e::loaded) << loaded.detail;
  EXPECT_EQ(loaded.record->owned_heads.size(), 1u);
  EXPECT_TRUE(verdict_is(restore_with(executor), native_verdict_e::ok));
  EXPECT_EQ(platform.head_count(), 0u);
  EXPECT_TRUE(fs::is_empty(directory));
}

TEST_F(JournalFile, ARestartFindsTheJournalAndLeavesTheDisplay) {
  std::string bytes;
  {
    dt::file_journal_store_t store { path };
    auto before = executor_over(store);
    ASSERT_TRUE(verdict_is(prepare_with(before, virtual_profile()), native_verdict_e::ok));
    bytes = read_file(path);
  }
  dt::file_journal_store_t store { path };
  auto after = executor_over(store);
  const auto health = recover_with(after);
  EXPECT_TRUE(in_conflict(health));
  EXPECT_TRUE(mentions(health.reason, "earlier run"));
  EXPECT_EQ(read_file(path), bytes);
  EXPECT_EQ(platform.head_count(), 1u);
}