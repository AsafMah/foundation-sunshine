/**
 * @file tests/unit/platform/windows/test_gh_hardware.cpp
 * @brief GH: one real ZakoVDD head through the Windows native platform, read back, removed.
 *
 * Skipped unless SUNSHINE_GH_HARDWARE=1. It writes to the real VDD driver, so it needs an
 * elevated process in the console session, and it refuses to touch anything if a driver
 * head already exists. The display baseline is captured and restored outside this test.
 */
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <random>
#include <sstream>
#include <stdexcept>

#include <gtest/gtest.h>

#include "src/display_device/topology_coordinator.h"
#include "src/display_device/topology_journal_store.h"
#include "src/display_device/topology_native.h"
#include "src/display_device/topology_windows_native.h"
#include "src/globals.h"
#include "src/platform/windows/display_device/native_edge.h"

namespace dt = display_device::topology;

namespace {
  constexpr const char *default_guid = "{6A1E5C3B-0D2F-4B8E-9C71-5F3A2E8D4B10}";

  bool
  enabled() {
    const char *flag = std::getenv("SUNSHINE_GH_HARDWARE");
    return flag && std::string_view { flag } == "1";
  }

  std::string_view
  kind_name(dt::head_class_e kind) {
    switch (kind) {
      case dt::head_class_e::physical:
        return "physical";
      case dt::head_class_e::driver_head:
        return "driver_head";
      default:
        return "unknown";
    }
  }

  std::size_t
  print(const char *label, const dt::edge_snapshot_t &snapshot) {
    std::size_t heads {};
    std::cout << "GH " << label << " available=" << snapshot.available << " displays=" << snapshot.displays.size() << '\n';
    for (const auto &display : snapshot.displays) {
      heads += display.kind == dt::head_class_e::driver_head;
      std::cout << "GH   id=" << display.id.value << " friendly=" << display.info.friendly_name
                << " kind=" << kind_name(display.kind)
                << " active=" << display.layout.has_value()
                << " container=" << (display.container ? dt::format_guid(*display.container) : std::string { "none" })
                << " arrival=" << display.arrival;
      if (display.layout) {
        std::cout << " mode=" << display.layout->mode.resolution.width << 'x' << display.layout->mode.resolution.height
                  << " pos=" << display.layout->position.x << ',' << display.layout->position.y;
      }
      std::cout << '\n';
    }
    std::cout.flush();
    return heads;
  }

  /** Runs one managed coordinator command other than a launch prepare. */
  void
  in_command(dt::command_kind_e kind, const std::function<void(const dt::mutation_capability_t &)> &body) {
    dt::coordinator_options_t options;
    options.managed_mode = true;
    options.probes.startup_restore_quiescent = [] { return true; };
    options.probes.active_streams = [] { return std::size_t {}; };
    options.native_restore = [](const auto &, const auto &) {};
    dt::coordinator_t coordinator { options };
    std::exception_ptr thrown;
    bool ran = false;
    coordinator.start([](const dt::mutation_capability_t &, const dt::command_context_t &) {}).get();
    coordinator.run(kind, [&](const dt::mutation_capability_t &capability, const dt::command_context_t &) {
      ran = true;
      try {
        body(capability);
      }
      catch (...) {
        thrown = std::current_exception();
      }
    });
    coordinator.stop();
    if (thrown) {
      std::rethrow_exception(thrown);
    }
    if (!ran) {
      throw std::runtime_error { "the GH command body did not run" };
    }
  }

  void
  in_launch_prepare(const std::function<void(const dt::mutation_capability_t &)> &body) {
    dt::coordinator_options_t options;
    options.managed_mode = true;
    options.probes.startup_restore_quiescent = [] { return true; };
    options.probes.active_streams = [] { return std::size_t {}; };
    options.native_restore = [](const auto &, const auto &) {};
    dt::coordinator_t coordinator { options };
    std::exception_ptr thrown;
    bool ran = false;
    coordinator.start([](const dt::mutation_capability_t &, const dt::command_context_t &) {}).get();
    const auto admission = coordinator.admit_launch({ 1, "cert:gh-hardware" });
    if (!admission) {
      coordinator.stop();
      throw std::runtime_error { "the GH launch was not admitted" };
    }
    coordinator.run_launch_prepare(*admission.token, [&](const dt::mutation_capability_t &capability, const dt::command_context_t &) {
      ran = true;
      try {
        body(capability);
      }
      catch (...) {
        thrown = std::current_exception();
      }
    });
    coordinator.abort_launch(*admission.token, {});
    coordinator.stop();
    if (thrown) {
      std::rethrow_exception(thrown);
    }
    if (!ran) {
      throw std::runtime_error { "the GH body did not run" };
    }
  }
}  // namespace

TEST(GhHardware, NativeCreateReadbackDestroy) {
  if (!enabled()) {
    GTEST_SKIP() << "set SUNSHINE_GH_HARDWARE=1 to write to the real VDD driver";
  }
  const char *requested = std::getenv("SUNSHINE_GH_GUID");
  const auto parsed = dt::parse_guid(requested ? requested : default_guid);
  ASSERT_TRUE(parsed) << "SUNSHINE_GH_GUID is not a braced GUID";
  const dt::head_guid_t intent { *parsed };
  std::cout << "GH intent=" << dt::format_guid(intent.value) << '\n';

  dt::win32_edge::win32_native_edge_t edge;
  dt::windows_native_platform_t platform { edge, { dt::zako_vdd_driver, ZAKO_NAME } };

  const auto before = edge.snapshot();
  const auto heads_before = print("before", before);
  ASSERT_TRUE(before.available) << "the baseline inventory is unavailable; nothing was written";
  ASSERT_EQ(heads_before, 0u) << "a driver head already exists; nothing was written";

  bool created = false;
  bool removed = false;
  bool container_matches = false;
  std::uint64_t arrival {};
  bool inventory_available = false;
  std::size_t inventory_matches {};
  std::size_t heads_after_create {};
  std::size_t heads_after_remove = 99;
  in_launch_prepare([&](const dt::mutation_capability_t &capability) {
    // Remove on every exit path once a create may have reached the driver.
    struct remover_t {
      std::function<void()> run;
      ~remover_t() {
        run();
      }
    } remover { [&] {
      try {
        const auto remove = platform.remove_all_heads(capability);
        removed = remove.has_value();
        if (!remove) {
          std::cout << "GH remove failed: " << remove.error().detail << '\n';
        }
      }
      catch (const std::exception &error) {
        std::cout << "GH remove threw: " << error.what() << '\n';
      }
      heads_after_remove = print("after-remove", edge.snapshot());
    } };

    const auto create = platform.create_head(capability, intent);
    if (!create) {
      std::cout << "GH create refused: " << create.error().detail << '\n';
    }
    else {
      created = *create == dt::creation_delivery_e::accepted;
      std::cout << "GH create delivery=" << (created ? "accepted" : "uncertain") << '\n';
    }

    const auto during = edge.snapshot();
    heads_after_create = print("after-create", during);
    for (const auto &display : during.displays) {
      if (display.kind == dt::head_class_e::driver_head) {
        container_matches = display.container && *display.container == intent.value;
        arrival = display.arrival;
        std::cout << "GH readback container_equals_intent=" << container_matches << " arrival=" << display.arrival << '\n';
      }
    }
    const auto inventory = platform.inventory();
    inventory_available = inventory.available;
    std::cout << "GH inventory available=" << inventory.available << " heads=" << inventory.heads.size() << '\n';
    for (const auto &head : inventory.heads) {
      inventory_matches += head.identity.guid == intent && head.identity.incarnation.value != 0;
      std::cout << "GH   head guid=" << dt::format_guid(head.identity.guid.value)
                << " incarnation=" << head.identity.incarnation.value
                << " display=" << (head.display ? head.display->value : std::string { "none" }) << '\n';
    }
    std::cout.flush();
  });

  EXPECT_TRUE(created);
  EXPECT_EQ(heads_after_create, 1u);
  EXPECT_TRUE(container_matches) << "G1 assumption: ContainerId == CREATEMONITOR GUID";
  EXPECT_NE(arrival, 0u) << "G1 assumption: LastArrivalDate is readable";
  EXPECT_TRUE(inventory_available);
  EXPECT_EQ(inventory_matches, 1u) << "the platform inventory must list exactly one head with the intent GUID";
  EXPECT_TRUE(removed);
  EXPECT_EQ(heads_after_remove, 0u);
}

/**
 * Kit 2: the managed executor's own path on the real driver. Prepare a virtual head (create and
 * extend it onto the desktop), resize it as a client reconfigure would, then restore. The other
 * displays must keep their mode and position after the restore. Skipped unless
 * SUNSHINE_GH_ACTIVATE=1; the kit captures and restores the CCD baseline around it.
 */
TEST(GhHardware, NativeActivateModeRestore) {
  const char *flag = std::getenv("SUNSHINE_GH_ACTIVATE");
  if (!flag || std::string_view { flag } != "1") {
    GTEST_SKIP() << "set SUNSHINE_GH_ACTIVATE=1 to activate and resize a real VDD head";
  }
  dt::win32_edge::win32_native_edge_t edge;
  dt::windows_native_platform_t platform { edge, { dt::zako_vdd_driver, ZAKO_NAME } };
  const auto journal = std::filesystem::temp_directory_path() / "sunshine-gh-activate-journal.json";
  std::filesystem::remove(journal);
  dt::file_journal_store_t store { journal };
  std::random_device random;
  dt::native_executor_t executor {
    store,
    platform,
    {
      [&] {
        dt::uuid_bytes_t bytes;
        for (auto &byte : bytes) {
          byte = static_cast<std::uint8_t>(random());
        }
        return bytes;
      },
      2,
      dt::zako_vdd_driver,
    },
  };

  const auto before = edge.snapshot();
  ASSERT_TRUE(before.available) << "the baseline inventory is unavailable; nothing was written";
  ASSERT_EQ(print("before", before), 0u) << "a driver head already exists; nothing was written";

  const auto others = [](const dt::edge_snapshot_t &snapshot) {
    std::map<std::string, std::string> layouts;
    for (const auto &display : snapshot.displays) {
      if (display.kind == dt::head_class_e::driver_head) {
        continue;
      }
      std::ostringstream out;
      if (display.layout) {
        out << display.layout->mode.resolution.width << 'x' << display.layout->mode.resolution.height
            << '@' << display.layout->position.x << ',' << display.layout->position.y;
      }
      else {
        out << "inactive";
      }
      layouts[display.id.value] = out.str();
    }
    return layouts;
  };
  const auto active_head = [](const dt::edge_snapshot_t &snapshot) -> const dt::edge_display_t * {
    for (const auto &display : snapshot.displays) {
      if (display.kind == dt::head_class_e::driver_head) {
        return &display;
      }
    }
    return nullptr;
  };

  std::uint32_t width = 1920;
  std::uint32_t height = 1080;
  if (const char *mode = std::getenv("SUNSHINE_GH_MODE")) {
    std::istringstream in { mode };
    char x {};
    in >> width >> x >> height;
  }

  dt::native_outcome_t prepared;
  in_launch_prepare([&](const auto &capability) { prepared = executor.prepare(capability, { dt::native_profile_t::source_e::virtual_head, {} }); });
  std::cout << "GH prepare verdict=" << dt::to_string(prepared.verdict) << " reason=" << prepared.reason
            << " target=" << (prepared.target ? prepared.target->display.value : std::string { "none" }) << '\n';
  const auto prepared_snapshot = edge.snapshot();
  print("after-prepare", prepared_snapshot);
  const auto *prepared_head = active_head(prepared_snapshot);
  const bool prepared_active = prepared_head && prepared_head->layout && !prepared_head->info.display_name.empty();
  std::cout << "GH prepare_active=" << prepared_active
            << " gdi=" << (prepared_head ? prepared_head->info.display_name : std::string {}) << '\n';
  const bool others_kept_on_prepare = others(prepared_snapshot) == others(before);
  std::cout << "GH prepare_others_unchanged=" << others_kept_on_prepare << '\n';

  dt::mode_change_outcome_t changed;
  if (prepared.verdict == dt::native_verdict_e::ok) {
    in_command(dt::command_kind_e::reconfigure, [&](const auto &capability) { changed = executor.change_mode(capability, { width, height }); });
  }
  std::cout << "GH change verdict=" << dt::to_string(changed.verdict) << " reason=" << changed.reason
            << " display=" << (changed.display ? changed.display->value : std::string { "none" }) << '\n';
  const auto changed_snapshot = edge.snapshot();
  print("after-change", changed_snapshot);
  const auto *changed_head = active_head(changed_snapshot);
  const bool changed_mode = changed_head && changed_head->layout &&
                            changed_head->layout->mode.resolution.width == width &&
                            changed_head->layout->mode.resolution.height == height;
  std::cout << "GH change_active_with_mode=" << changed_mode << '\n';

  dt::native_outcome_t restored;
  in_command(dt::command_kind_e::native_restore, [&](const auto &capability) { restored = executor.restore(capability); });
  std::cout << "GH restore verdict=" << dt::to_string(restored.verdict) << " reason=" << restored.reason << '\n';
  const auto after = edge.snapshot();
  const auto heads_after = print("after-restore", after);
  const bool others_restored = others(after) == others(before);
  std::cout << "GH restore_others_unchanged=" << others_restored << " journal_left=" << std::filesystem::exists(journal) << '\n';
  std::cout.flush();

  EXPECT_EQ(prepared.verdict, dt::native_verdict_e::ok) << prepared.reason;
  EXPECT_TRUE(prepared_active) << "hardware unknown: CCD extend adds a cold-created head to the desktop";
  EXPECT_TRUE(others_kept_on_prepare) << "extend-only must leave the other displays' mode and position";
  EXPECT_EQ(changed.verdict, dt::mode_change_verdict_e::applied) << changed.reason;
  EXPECT_TRUE(changed_mode) << "hardware unknown: a SETMODES-recreated head is back on the desktop at the new mode";
  EXPECT_EQ(restored.verdict, dt::native_verdict_e::ok) << restored.reason;
  EXPECT_EQ(heads_after, 0u);
  EXPECT_TRUE(others_restored) << "removing the head must leave the other displays as they were";
  EXPECT_FALSE(std::filesystem::exists(journal));
}

/** Read-only: list the edge classification; the kit parses `GH driver_heads=`. */
TEST(GhHardware, ProbeDriverHeads) {
  const char *flag = std::getenv("SUNSHINE_GH_PROBE");
  if (!flag || std::string_view { flag } != "1") {
    GTEST_SKIP() << "set SUNSHINE_GH_PROBE=1 to list the real display classification";
  }
  dt::win32_edge::win32_native_edge_t edge;
  const auto snapshot = edge.snapshot();
  const auto heads = print("probe", snapshot);
  std::cout << "GH driver_heads=" << (snapshot.available ? std::to_string(heads) : std::string { "unavailable" }) << '\n';
  EXPECT_TRUE(snapshot.available);
}

/** Recovery only: remove every driver head, for when the run above could not. */
TEST(GhHardware, CleanupRemoveAllHeads) {
  const char *flag = std::getenv("SUNSHINE_GH_CLEANUP");
  if (!flag || std::string_view { flag } != "1") {
    GTEST_SKIP() << "set SUNSHINE_GH_CLEANUP=1 to remove every real VDD head";
  }
  dt::win32_edge::win32_native_edge_t edge;
  dt::windows_native_platform_t platform { edge, { dt::zako_vdd_driver, ZAKO_NAME } };
  print("cleanup-before", edge.snapshot());
  bool removed = false;
  in_launch_prepare([&](const dt::mutation_capability_t &capability) {
    const auto remove = platform.remove_all_heads(capability);
    removed = remove.has_value();
    if (!remove) {
      std::cout << "GH cleanup remove failed: " << remove.error().detail << '\n';
    }
  });
  const auto heads = print("cleanup-after", edge.snapshot());
  EXPECT_TRUE(removed);
  EXPECT_EQ(heads, 0u);
}
