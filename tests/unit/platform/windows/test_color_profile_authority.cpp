// Actual color-profile policy with fake inventory, impersonation and dynamically loaded APIs.
#include "src/logging.h"
#include "src/platform/windows/display_device/windows_utils.h"
#include "src/platform/windows/misc.h"
#include "tests/unit/topology_test_worker.h"

#include <array>
#include <bit>
#include <future>
#include <icm.h>
#include <string_view>

namespace {
  int queries {};
  HRESULT write_result { S_OK };
  std::vector<std::wstring> writes;
  std::vector<bool> defaults;

  HRESULT WINAPI
  fake_add(WCS_PROFILE_MANAGEMENT_SCOPE scope, PCWSTR profile, LUID adapter, UINT32 source, BOOL make_default, BOOL advanced) {
    EXPECT_EQ(scope, WCS_PROFILE_MANAGEMENT_SCOPE_CURRENT_USER);
    EXPECT_EQ(adapter.LowPart, 12u);
    EXPECT_EQ(source, 7u);
    EXPECT_TRUE(advanced);
    writes.push_back(L"add:" + std::wstring { profile });
    defaults.push_back(make_default);
    return write_result;
  }
  HRESULT WINAPI
  fake_remove(WCS_PROFILE_MANAGEMENT_SCOPE scope, PCWSTR profile, LUID adapter, UINT32 source, BOOL advanced) {
    EXPECT_EQ(scope, WCS_PROFILE_MANAGEMENT_SCOPE_CURRENT_USER);
    EXPECT_EQ(adapter.LowPart, 12u);
    EXPECT_EQ(source, 7u);
    EXPECT_TRUE(advanced);
    writes.push_back(L"remove:" + std::wstring { profile });
    return write_result;
  }
  HRESULT WINAPI
  fake_default(WCS_PROFILE_MANAGEMENT_SCOPE, PCWSTR profile, COLORPROFILETYPE, COLORPROFILESUBTYPE, LUID, UINT32) {
    writes.push_back(L"default:" + std::wstring { profile });
    return write_result;
  }
  HRESULT WINAPI
  fake_list(WCS_PROFILE_MANAGEMENT_SCOPE, LUID, UINT32, LPWSTR **, PDWORD) {
    ADD_FAILURE();
    return E_FAIL;
  }
  HRESULT WINAPI
  fake_get_default(WCS_PROFILE_MANAGEMENT_SCOPE, LUID, UINT32, COLORPROFILETYPE, COLORPROFILESUBTYPE, LPWSTR *) {
    ADD_FAILURE();
    return E_FAIL;
  }
  HRESULT WINAPI
  fake_scope(LUID, UINT32, WCS_PROFILE_MANAGEMENT_SCOPE *) {
    ADD_FAILURE();
    return E_FAIL;
  }

  HMODULE
  fake_load_library(LPCWSTR, HANDLE, DWORD) {
    return reinterpret_cast<HMODULE>(static_cast<std::uintptr_t>(1));
  }
  BOOL
  fake_free_library(HMODULE) { return TRUE; }
  FARPROC
  fake_get_proc_address(HMODULE, LPCSTR name) {
    const std::string_view function { name };
    if (function == "ColorProfileAddDisplayAssociation") return std::bit_cast<FARPROC>(&fake_add);
    if (function == "ColorProfileRemoveDisplayAssociation") return std::bit_cast<FARPROC>(&fake_remove);
    if (function == "ColorProfileSetDisplayDefaultAssociation") return std::bit_cast<FARPROC>(&fake_default);
    if (function == "ColorProfileGetDisplayList") return std::bit_cast<FARPROC>(&fake_list);
    if (function == "ColorProfileGetDisplayDefault") return std::bit_cast<FARPROC>(&fake_get_default);
    if (function == "ColorProfileGetDisplayUserScope") return std::bit_cast<FARPROC>(&fake_scope);
    ADD_FAILURE() << name;
    return nullptr;
  }
}  // namespace

#define LoadLibraryExW fake_load_library
#define FreeLibrary fake_free_library
#define GetProcAddress fake_get_proc_address
#include "src/platform/windows/display_device/color_profile.cpp"
#undef LoadLibraryExW
#undef FreeLibrary
#undef GetProcAddress

boost::log::sources::severity_logger<int> verbose, debug, info, warning, error, fatal;

namespace platf {
  void
  print_status(const std::string_view &, HRESULT) {}
  bool
  is_running_as_system() { return false; }
  HANDLE
  retrieve_users_token(bool) {
    ADD_FAILURE();
    return nullptr;
  }
  std::error_code
  impersonate_current_user(HANDLE, std::function<void()>) {
    ADD_FAILURE();
    return std::make_error_code(std::errc::permission_denied);
  }
  std::string
  to_utf8(const std::wstring &value) { return { value.begin(), value.end() }; }
  std::wstring
  from_utf8(const std::string &value) { return { value.begin(), value.end() }; }
}  // namespace platf

namespace display_device::w_utils {
  boost::optional<path_and_mode_data_t>
  query_display_config(bool active_only) {
    ++queries;
    EXPECT_EQ(active_only, ACTIVE_ONLY_DEVICES);
    path_and_mode_data_t data;
    DISPLAYCONFIG_PATH_INFO path {};
    path.targetInfo.adapterId.LowPart = 12;
    path.sourceInfo.id = 7;
    data.paths.push_back(path);
    return data;
  }
  const DISPLAYCONFIG_PATH_INFO *
  get_active_path(const std::string &id, const std::vector<DISPLAYCONFIG_PATH_INFO> &paths) {
    return id == "display" && !paths.empty() ? &paths.front() : nullptr;
  }
}  // namespace display_device::w_utils

namespace {
  namespace color = display_device::win_color_profile;
  namespace topology = display_device::topology;
  using cap = topology::mutation_capability_t;
  using kind = topology::command_kind_e;

  color::state_t
  state() {
    color::state_t result;
    result.device_id = "display";
    result.applied_profile = "test.icc";
    return result;
  }

  struct mutation_t {
    const char *name;
    std::function<void(const cap &)> call;
  };
  const std::array<mutation_t, 4> mutations { {
    { "apply", [](const cap &c) { (void) color::apply(c, state()); } },
    { "restore", [](const cap &c) { (void) color::restore(c, state()); } },
    { "remove association", [](const cap &c) {
       (void) color::remove_advanced_association(c, color::color_profile_api(), { { 12, 0 }, 7 }, L"test.icc");
     } },
    { "restore association", [](const cap &c) {
       (void) color::restore_association_without_default(c, color::color_profile_api(), { { 12, 0 }, 7 }, L"test.icc");
     } },
  } };

  class ColorProfileAuthority: public ::testing::Test {
  protected:
    void
    SetUp() override {
      queries = 0;
      writes.clear();
      defaults.clear();
      write_result = S_OK;
    }
  };

  TEST_F(ColorProfileAuthority, WrongCommandAndOffWorkerCannotReachInventoryOrAssociations) {
    for (bool managed : { false, true }) {
      topology_test::with_capability(kind::rotate, managed, [&](const cap &capability) {
        for (const auto &mutation : mutations) {
          SCOPED_TRACE(mutation.name);
          EXPECT_THROW(mutation.call(capability), topology::capability_error_t);
        }
      });
    }
    topology_test::with_capability(kind::launch_prepare, false, [&](const cap &capability) {
      for (const auto &mutation : mutations) {
        SCOPED_TRACE(mutation.name);
        auto elsewhere = std::async(std::launch::async, [&] { mutation.call(capability); });
        EXPECT_THROW(elsewhere.get(), topology::capability_error_t);
      }
    });
    EXPECT_EQ(queries, 0);
    EXPECT_TRUE(writes.empty());
  }

  TEST_F(ColorProfileAuthority, AuthorityPrecedesInvalidState) {
    topology_test::with_capability(kind::rotate, false, [&](const cap &capability) {
      EXPECT_THROW((void) color::apply(capability, {}), topology::capability_error_t);
      EXPECT_THROW((void) color::restore(capability, {}), topology::capability_error_t);
    });
    EXPECT_EQ(queries, 0);
    EXPECT_TRUE(writes.empty());
  }

  TEST_F(ColorProfileAuthority, RestrictedStartupRestoreCanReassociateWithoutConfigurePermission) {
    topology_test::with_capability(kind::legacy_startup_restore, true, [&](const cap &capability) {
      EXPECT_THROW(capability.require(topology::operation_e::configure_display), topology::capability_error_t);
      auto saved = state();
      saved.applied_profile_was_associated = true;
      EXPECT_TRUE(color::restore(capability, saved));
    });
    EXPECT_EQ(queries, 1);
    EXPECT_EQ(writes, (std::vector<std::wstring> { L"remove:test.icc", L"add:test.icc" }));
    EXPECT_EQ(defaults, (std::vector<bool> { false }));
  }

  TEST_F(ColorProfileAuthority, ApplyAndRestorePreserveDefaultPolicyAndFailures) {
    auto saved = state();
    topology_test::with_capability(kind::launch_prepare, false, [&](const cap &capability) {
      EXPECT_TRUE(color::apply(capability, saved));
    });
    EXPECT_EQ(writes, (std::vector<std::wstring> { L"add:test.icc" }));
    EXPECT_EQ(defaults, (std::vector<bool> { true }));

    writes.clear();
    saved.original_scope_was_current_user = true;
    saved.original_default_present = true;
    saved.original_default_profile = "original.icc";
    topology_test::with_capability(kind::stream_restore, false, [&](const cap &capability) {
      EXPECT_TRUE(color::restore(capability, saved));
    });
    EXPECT_EQ(writes, (std::vector<std::wstring> { L"default:original.icc", L"remove:test.icc" }));

    writes.clear();
    saved.original_scope_was_current_user = false;
    saved.applied_profile_was_associated = true;
    write_result = E_ACCESSDENIED;
    topology_test::with_capability(kind::stream_restore, false, [&](const cap &capability) {
      EXPECT_FALSE(color::restore(capability, saved));
    });
    EXPECT_EQ(writes, (std::vector<std::wstring> { L"remove:test.icc" }));
  }

  template <class T>
  concept ungated_apply = requires(const T &v) { color::apply(v); };
  template <class T>
  concept ungated_restore = requires(const T &v) { color::restore(v); };

  TEST_F(ColorProfileAuthority, NoUngatedColorMutationRemains) {
    EXPECT_FALSE(ungated_apply<color::state_t>);
    EXPECT_FALSE(ungated_restore<color::state_t>);
  }
}  // namespace
