#pragma once

#include <gtest/gtest.h>

#include "src/display_device/topology.h"

namespace display_topology_test {
  namespace dt = display_device::topology;

  template <class T>
  ::testing::AssertionResult
  error_is(const dt::result_t<T> &result, dt::error_code_e expected) {
    if (result) return ::testing::AssertionFailure() << "Expected typed refusal, but the operation succeeded";
    if (result.error().code != expected) {
      return ::testing::AssertionFailure() << "Expected error " << static_cast<int>(expected)
                                           << ", received " << static_cast<int>(result.error().code) << ": " << result.error().detail;
    }
    return ::testing::AssertionSuccess();
  }

  template <class Id>
  Id
  id(std::uint8_t last) {
    dt::uuid_bytes_t bytes { 0x12, 0x34, 0x56, 0x78 };
    bytes.back() = last;
    return Id { bytes };
  }

  inline dt::limits_t
  limits() {
    return { 8, 8, { 8192, 8192 }, 1'000'000'000, 4'000'000'000 };
  }

  inline dt::layout_t
  layout(std::int32_t x, std::int32_t y = 0, unsigned width = 1920, unsigned height = 1080) {
    return { { x, y }, { { width, height }, { 60, 1 } }, dt::rotation_e::identity, 100, display_device::hdr_state_e::disabled };
  }

  inline dt::permissions_t
  edit_permissions() {
    return { true, true, true, true, true, true, true, true, true, true };
  }

  struct fixture_t {
    dt::inventory_t inventory;
    dt::profile_t profile;
  };

  inline fixture_t
  physical() {
    fixture_t fixture;
    auto &inventory = fixture.inventory;
    inventory.available = true;
    inventory.revision = dt::inventory_revision_t { 7 };
    inventory.audio_endpoint = "original-endpoint";
    inventory.clone_groups = { { "os-A" }, { "os-B" } };
    inventory.displays = {
      { dt::os_display_id_t { "os-A" }, { "DISPLAY1", "Identical name", display_device::device_state_e::active, display_device::hdr_state_e::disabled },
        layout(-1920), dt::adapter_luid_t { 7 }, dt::source_incarnation_t { 1 }, {}, {} },
      { dt::os_display_id_t { "os-B" }, { "DISPLAY2", "Identical name", display_device::device_state_e::primary, display_device::hdr_state_e::disabled },
        layout(0), dt::adapter_luid_t { 7 }, dt::source_incarnation_t { 2 }, {}, {} }
    };
    for (auto &display : inventory.displays) display.supported_modes = { display.layout->mode };
    auto &profile = fixture.profile;
    profile.id = id<dt::profile_id_t>(1);
    profile.name = "Two sources";
    profile.limits = limits();
    profile.entries = {
      { id<dt::monitor_id_t>(1), dt::physical_source_t { dt::os_display_id_t { "os-A" }, dt::adapter_luid_t { 7 } } },
      { id<dt::monitor_id_t>(2), dt::physical_source_t { dt::os_display_id_t { "os-B" }, dt::adapter_luid_t { 7 } } }
    };
    profile.order = { profile.entries[0].id, profile.entries[1].id };
    profile.fallback = profile.entries[0].id;
    profile.preferred = profile.entries[1].id;
    return fixture;
  }

  inline fixture_t
  mixed() {
    auto fixture = physical();
    const auto owner = id<dt::owner_id_t>(50);
    const auto driver = id<dt::driver_instance_id_t>(60);
    fixture.inventory.displays.resize(1);
    fixture.profile.entries.resize(1);
    fixture.profile.order.resize(1);
    fixture.inventory.clone_groups = { { "os-A" } };
    fixture.inventory.drivers = { { driver, dt::adapter_luid_t { 7 }, 3 } };
    for (std::uint8_t i = 0; i < 2; ++i) {
      const dt::head_identity_t head { driver, id<dt::head_guid_t>(i + 10), dt::head_incarnation_t { std::uint64_t(100) + i } };
      const dt::os_display_id_t display { i == 0 ? "os-V1" : "os-V2" };
      fixture.inventory.heads.push_back({ head, owner, dt::adapter_luid_t { 7 }, i, display });
      fixture.inventory.displays.push_back({ display,
        { "Same display name", "Identical name", i == 0 ? display_device::device_state_e::primary : display_device::device_state_e::active, display_device::hdr_state_e::disabled },
        layout(i * 1920), dt::adapter_luid_t { 7 }, dt::source_incarnation_t { std::uint64_t(200) + i }, head, {} });
      fixture.inventory.clone_groups.push_back({ display.value });
      fixture.profile.entries.push_back({ id<dt::monitor_id_t>(i + 2), dt::virtual_source_t { head.guid, owner, dt::adapter_luid_t { 7 } } });
      fixture.profile.order.push_back(fixture.profile.entries.back().id);
    }
    fixture.profile.preferred = fixture.profile.entries[1].id;
    return fixture;
  }

  inline dt::requested_topology_t
  request(const fixture_t &fixture) {
    return { id<dt::owner_id_t>(50), id<dt::request_id_t>(70), dt::topology_revision_t { 10 }, fixture.inventory.revision, fixture.profile };
  }

  inline dt::applied_topology_t
  applied(const fixture_t &fixture) {
    const auto requested = request(fixture);
    dt::applied_topology_t result { requested.owner, requested.request, requested.base, dt::topology_revision_t { 11 }, {} };
    const auto bindings = dt::resolve_profile(fixture.profile, fixture.inventory);
    for (std::size_t i = 0; i < bindings.size(); ++i) {
      result.monitors.push_back({ bindings[i].monitor, dt::applied_state_e::applied, bindings[i].binding,
        fixture.inventory.displays[i].layout, fixture.inventory.displays[i].info.device_state == display_device::device_state_e::primary, {} });
    }
    return result;
  }

  struct fake_clock_t {
    dt::monotonic_time_t now {};

    void
    advance(dt::monotonic_time_t elapsed) {
      now += elapsed;
    }
  };
}  // namespace display_topology_test
