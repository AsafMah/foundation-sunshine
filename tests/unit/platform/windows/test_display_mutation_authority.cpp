// Real display mutators, with unavailable inventory and a hard fake at the CCD edge.
#include "src/logging.h"
#include "src/platform/windows/display_device/windows_utils.h"
#include "tests/unit/topology_test_worker.h"

#include <algorithm>
#include <array>
#include <future>

namespace dd = display_device;
namespace topology = dd::topology;

namespace {
  int queries {};
  int writes {};

  LONG
  fake_set_display_config(UINT32, DISPLAYCONFIG_PATH_INFO *, UINT32, DISPLAYCONFIG_MODE_INFO *, UINT32) {
    ++writes;
    ADD_FAILURE() << "unavailable inventory must never reach a CCD write";
    return ERROR_GEN_FAILURE;
  }
}  // namespace

#define SetDisplayConfig fake_set_display_config
#include "src/platform/windows/display_device/device_hdr_states.cpp"
#include "src/platform/windows/display_device/device_modes.cpp"
#include "src/platform/windows/display_device/device_topology.cpp"
#include "src/platform/windows/display_device/general_functions.cpp"
#undef SetDisplayConfig

boost::log::sources::severity_logger<int> verbose, debug, info, warning, error, fatal;
const std::string ZAKO_NAME { "test-vdd" };
const std::string VDD_NAME { "test-vdd-alias" };

namespace display_device::w_utils {
  boost::optional<path_and_mode_data_t>
  query_display_config(bool) {
    ++queries;
    return boost::none;
  }

  std::string
  get_error_string(LONG) { return "fake CCD failure"; }
  bool
  is_primary(const DISPLAYCONFIG_SOURCE_MODE &) {
    ADD_FAILURE();
    return false;
  }
  bool
  are_modes_duplicated(const DISPLAYCONFIG_SOURCE_MODE &, const DISPLAYCONFIG_SOURCE_MODE &) {
    ADD_FAILURE();
    return false;
  }
  bool
  is_active(const DISPLAYCONFIG_PATH_INFO &) {
    ADD_FAILURE();
    return false;
  }
  void
  set_active(DISPLAYCONFIG_PATH_INFO &) { ADD_FAILURE(); }
  std::string
  get_device_id(const DISPLAYCONFIG_PATH_INFO &) {
    ADD_FAILURE();
    return {};
  }
  std::string
  get_display_name(const DISPLAYCONFIG_PATH_INFO &) {
    ADD_FAILURE();
    return {};
  }
  std::string
  get_friendly_name(const DISPLAYCONFIG_PATH_INFO &) {
    ADD_FAILURE();
    return {};
  }
  hdr_state_e
  get_hdr_state(const DISPLAYCONFIG_PATH_INFO &) {
    ADD_FAILURE();
    return hdr_state_e::unknown;
  }
  bool
  set_hdr_state(const topology::mutation_capability_t &, const DISPLAYCONFIG_PATH_INFO &, bool) {
    ++writes;
    ADD_FAILURE() << "unavailable inventory must never reach HDR mutation";
    return false;
  }
  boost::optional<UINT32>
  get_source_index(const DISPLAYCONFIG_PATH_INFO &, const std::vector<DISPLAYCONFIG_MODE_INFO> &) {
    ADD_FAILURE();
    return boost::none;
  }
  void
  set_source_index(DISPLAYCONFIG_PATH_INFO &, const boost::optional<UINT32> &) { ADD_FAILURE(); }
  void
  set_target_index(DISPLAYCONFIG_PATH_INFO &, const boost::optional<UINT32> &) { ADD_FAILURE(); }
  void
  set_desktop_index(DISPLAYCONFIG_PATH_INFO &, const boost::optional<UINT32> &) { ADD_FAILURE(); }
  void
  set_clone_group_id(DISPLAYCONFIG_PATH_INFO &, const boost::optional<UINT32> &) { ADD_FAILURE(); }
  const DISPLAYCONFIG_SOURCE_MODE *
  get_source_mode(const boost::optional<UINT32> &, const std::vector<DISPLAYCONFIG_MODE_INFO> &) {
    ADD_FAILURE();
    return nullptr;
  }
  DISPLAYCONFIG_SOURCE_MODE *
  get_source_mode(const boost::optional<UINT32> &, std::vector<DISPLAYCONFIG_MODE_INFO> &) {
    ADD_FAILURE();
    return nullptr;
  }
  boost::optional<device_info_t>
  get_device_info_for_valid_path(const DISPLAYCONFIG_PATH_INFO &, bool) {
    ADD_FAILURE();
    return boost::none;
  }
  const DISPLAYCONFIG_PATH_INFO *
  get_active_path(const std::string &, const std::vector<DISPLAYCONFIG_PATH_INFO> &) {
    ADD_FAILURE();
    return nullptr;
  }
  DISPLAYCONFIG_PATH_INFO *
  get_active_path(const std::string &, std::vector<DISPLAYCONFIG_PATH_INFO> &) {
    ADD_FAILURE();
    return nullptr;
  }
}  // namespace display_device::w_utils

namespace {
  using cap = topology::mutation_capability_t;
  using op = topology::operation_e;
  struct mutation_t {
    const char *name;
    op operation;
    bool (*call)(const cap &);
  };
  const std::array<mutation_t, 8> mutations { {
    { "topology", op::write_topology, [](const cap &c) { return dd::set_topology(c, { { "display" } }); } },
    { "topology rollback", op::write_topology, [](const cap &c) { return dd::do_set_topology(c, op::write_topology, { { "display" } }); } },
    { "activate", op::native_activate, [](const cap &c) { return dd::extend_topology(c, "display"); } },
    { "modes", op::write_display_modes, [](const cap &c) { return dd::set_display_modes(c, { { "display", { { 1920, 1080 }, { 60, 1 } } } }); } },
    { "modes rollback", op::write_display_modes, [](const cap &c) { return dd::do_set_modes(c, { { "display", { { 1920, 1080 }, { 60, 1 } } } }, false); } },
    { "hdr", op::write_hdr, [](const cap &c) { return dd::set_hdr_states(c, { { "display", dd::hdr_state_e::enabled } }); } },
    { "hdr rollback", op::write_hdr, [](const cap &c) { return dd::do_set_states(c, { { "display", dd::hdr_state_e::enabled } }); } },
    { "primary", op::write_primary, [](const cap &c) { return dd::set_as_primary_device(c, "display"); } },
  } };

  TEST(DisplayMutationAuthority, CommandAndModeAreCheckedBeforeAnyInventoryQuery) {
    using kind = topology::command_kind_e;
    struct row_t {
      kind command;
      bool managed;
      bool restoration;
      bool topology_only;
    };
    const std::array rows {
      row_t { kind::startup_recovery, false, true, false },
      row_t { kind::startup_recovery, true, true, false },
      row_t { kind::legacy_startup_restore, false, true, false },
      row_t { kind::legacy_startup_restore, true, true, false },
      row_t { kind::launch_prepare, false, true, false },
      row_t { kind::launch_prepare, true, false, false },
      row_t { kind::legacy_retry_tick, false, true, false },
      row_t { kind::legacy_retry_tick, true, false, false },
      row_t { kind::legacy_unlock_restore, false, true, false },
      row_t { kind::legacy_unlock_restore, true, false, false },
      row_t { kind::vdd_toggle_followup, false, false, true },
      row_t { kind::vdd_toggle_followup, true, false, false },
      row_t { kind::rotate, false, false, false },
      row_t { kind::scale, false, false, false },
    };
    for (const auto &row : rows) {
      topology_test::with_capability(row.command, row.managed, [&](const cap &capability) {
        SCOPED_TRACE(std::string { topology::to_string(row.command) } + (row.managed ? "/managed" : "/legacy"));
        for (const auto &mutation : mutations) {
          SCOPED_TRACE(mutation.name);
          queries = writes = 0;
          const bool allowed = mutation.operation == op::native_activate ?
                                 row.command == kind::launch_prepare && row.managed :
                                 row.restoration || (row.topology_only && mutation.operation == op::write_topology);
          if (allowed) {
            EXPECT_FALSE(mutation.call(capability)) << "inventory failure must remain a failure";
          }
          else {
            EXPECT_THROW((void) mutation.call(capability), topology::capability_error_t);
          }
          EXPECT_EQ(queries, allowed ? 1 : 0);
          EXPECT_EQ(writes, 0);
        }
      });
    }
  }

  TEST(DisplayMutationAuthority, WorkerAuthorityCannotBeUsedFromAnotherThread) {
    topology_test::with_capability(topology::command_kind_e::launch_prepare, false, [&](const cap &capability) {
      for (const auto &mutation : mutations) {
        SCOPED_TRACE(mutation.name);
        queries = writes = 0;
        auto elsewhere = std::async(std::launch::async, [&] { return mutation.call(capability); });
        EXPECT_THROW((void) elsewhere.get(), topology::capability_error_t);
        EXPECT_EQ(queries, 0);
        EXPECT_EQ(writes, 0);
      }
    });
  }

  TEST(DisplayMutationAuthority, AuthorityPrecedesNoOpAndInvalidInput) {
    topology_test::with_capability(topology::command_kind_e::rotate, false, [&](const cap &capability) {
      queries = writes = 0;
      EXPECT_THROW((void) dd::set_topology(capability, {}), topology::capability_error_t);
      EXPECT_THROW((void) dd::set_display_modes(capability, {}), topology::capability_error_t);
      EXPECT_THROW((void) dd::set_hdr_states(capability, {}), topology::capability_error_t);
      EXPECT_THROW((void) dd::set_as_primary_device(capability, {}), topology::capability_error_t);
      EXPECT_EQ(queries, 0);
      EXPECT_EQ(writes, 0);
    });
  }

  template <class T>
  concept ungated_topology = requires(const T &v) { dd::set_topology(v); };
  template <class T>
  concept ungated_modes = requires(const T &v) { dd::set_display_modes(v); };
  template <class T>
  concept ungated_hdr = requires(const T &v) { dd::set_hdr_states(v); };
  template <class T>
  concept ungated_primary = requires(const T &v) { dd::set_as_primary_device(v); };

  TEST(DisplayMutationAuthority, NoUngatedDisplaySetterRemains) {
    EXPECT_FALSE(ungated_topology<dd::active_topology_t>);
    EXPECT_FALSE(ungated_modes<dd::device_display_mode_map_t>);
    EXPECT_FALSE(ungated_hdr<dd::hdr_state_map_t>);
    EXPECT_FALSE(ungated_primary<std::string>);
  }
}  // namespace
