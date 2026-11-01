// Actual launch policy and handle observation; process/token/kernel edges are fake.
#define WIN32_LEAN_AND_MEAN
#include <algorithm>
#include <functional>
#include <gtest/gtest.h>

#include "src/platform/windows/owned_process.h"
#include "src/platform/windows/run_command_helpers.h"
#include "src/logging.h"
#include "src/utility.h"

namespace fake_process {
  DWORD WINAPI attributes(LPCWSTR);
  HANDLE WINAPI current();
  BOOL WINAPI open_token(HANDLE, DWORD, PHANDLE);
  BOOL WINAPI close(HANDLE);
  DWORD WINAPI last_error();
  DWORD WINAPI wait(HANDLE, DWORD);
  BOOL WINAPI exit_code(HANDLE, LPDWORD);
  BOOL WINAPI create(LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL,
    DWORD, LPVOID, LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION);
  BOOL WINAPI create_as_user(HANDLE, LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL,
    DWORD, LPVOID, LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION);
}
#define GetFileAttributesW fake_process::attributes
#define GetCurrentProcess fake_process::current
#define OpenProcessToken fake_process::open_token
#define CloseHandle fake_process::close
#define GetLastError fake_process::last_error
#define WaitForSingleObject fake_process::wait
#define GetExitCodeProcess fake_process::exit_code
#define CreateProcessW fake_process::create
#define CreateProcessAsUserW fake_process::create_as_user
#include "src/platform/windows/run_command.cpp"
#undef GetFileAttributesW
#undef GetCurrentProcess
#undef OpenProcessToken
#undef CloseHandle
#undef GetLastError
#undef WaitForSingleObject
#undef GetExitCodeProcess
#undef CreateProcessW
#undef CreateProcessAsUserW

boost::log::sources::severity_logger<int> verbose, debug, info, warning, error, fatal;

namespace {
  char process_tag, thread_tag, token_tag, current_tag, attributes_tag;
  const HANDLE process_handle = &process_tag;
  const HANDLE thread_handle = &thread_tag;
  const HANDLE token_handle = &token_tag;
  const HANDLE current_handle = &current_tag;
  constexpr DWORD pid = 42;

  struct host_t {
    bool system {}, startup_fails {}, token_fails {}, merge_fails {}, impersonation_fails {};
    bool post_error {}, post_throw {}, creation_succeeds { true }, query_succeeds { true };
    bool elevated { true }, interactive { true };
    unsigned created {}, legacy_bridges {}, queries {}, waited {}, freed_attributes {};
    DWORD wait_result { WAIT_TIMEOUT }, code {}, native_error { ERROR_ACCESS_DENIED }, timeout {};
    std::vector<HANDLE> closed;
    std::function<void()> after_create;
  } host;

  unsigned closed(HANDLE handle) {
    return static_cast<unsigned>(std::count(host.closed.begin(), host.closed.end(), handle));
  }

  std::error_code launch(platf::owned_process_t &owner) {
    bp::environment env;
    env["OWNED_LAUNCH_TEST"] = "original";
    boost::filesystem::path directory { L"C:\\fake" };
    std::error_code ec;
    owner.launch(host.elevated, host.interactive, "original command", directory, env, nullptr, ec, nullptr);
    EXPECT_EQ(env["OWNED_LAUNCH_TEST"].to_string(), "original");
    return ec;
  }

  class OwnedProcess : public ::testing::Test {
    void SetUp() override { host = {}; }
  };
}

namespace fake_process {
  DWORD WINAPI attributes(LPCWSTR path) {
    EXPECT_EQ(std::wstring(path), L"C:\\fake");
    return FILE_ATTRIBUTE_DIRECTORY;
  }
  HANDLE WINAPI current() { return current_handle; }
  BOOL WINAPI open_token(HANDLE process, DWORD access, PHANDLE token) {
    EXPECT_EQ(process, current_handle);
    EXPECT_EQ(access, TOKEN_QUERY | TOKEN_DUPLICATE);
    if (host.token_fails) return FALSE;
    *token = token_handle;
    return TRUE;
  }
  BOOL WINAPI close(HANDLE handle) {
    host.closed.push_back(handle);
    host.native_error = ERROR_SUCCESS;
    return TRUE;
  }
  DWORD WINAPI last_error() { return host.native_error; }
  DWORD WINAPI wait(HANDLE handle, DWORD timeout) {
    EXPECT_EQ(handle, process_handle);
    ++host.waited;
    host.timeout = timeout;
    return host.wait_result;
  }
  BOOL WINAPI exit_code(HANDLE handle, LPDWORD code) {
    EXPECT_EQ(handle, process_handle);
    ++host.queries;
    if (!host.query_succeeds) return FALSE;
    *code = host.code;
    return TRUE;
  }
  BOOL WINAPI create(LPCWSTR application, LPWSTR command, LPSECURITY_ATTRIBUTES process_attributes,
    LPSECURITY_ATTRIBUTES thread_attributes, BOOL inherit, DWORD flags, LPVOID env, LPCWSTR directory,
    LPSTARTUPINFOW startup, LPPROCESS_INFORMATION result) {
    EXPECT_EQ(application, nullptr);
    EXPECT_EQ(std::wstring(command), L"resolved command");
    EXPECT_EQ(process_attributes, nullptr);
    EXPECT_EQ(thread_attributes, nullptr);
    EXPECT_TRUE(inherit);
    EXPECT_EQ(flags, EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT |
                      CREATE_BREAKAWAY_FROM_JOB | (host.interactive ? CREATE_NEW_CONSOLE : CREATE_NO_WINDOW));
    EXPECT_EQ(std::wstring(static_cast<const wchar_t *>(env)), L"fake environment");
    EXPECT_EQ(std::wstring(directory), L"C:\\fake");
    EXPECT_EQ(startup->cb, sizeof(STARTUPINFOEXW));
    ++host.created;
    if (!host.creation_succeeds) return FALSE;
    *result = { process_handle, thread_handle, pid, 43 };
    return TRUE;
  }
  BOOL WINAPI create_as_user(HANDLE token, LPCWSTR application, LPWSTR command,
    LPSECURITY_ATTRIBUTES process_attributes, LPSECURITY_ATTRIBUTES thread_attributes, BOOL inherit,
    DWORD flags, LPVOID env, LPCWSTR directory, LPSTARTUPINFOW startup, LPPROCESS_INFORMATION result) {
    EXPECT_EQ(token, token_handle);
    EXPECT_TRUE(host.system);
    return create(application, command, process_attributes, thread_attributes, inherit, flags, env, directory, startup, result);
  }
}

namespace platf {
  std::string to_utf8(const std::wstring &) { return "fake"; }
  bool is_running_as_system() { return host.system; }
  HANDLE retrieve_users_token(bool elevated) {
    EXPECT_EQ(elevated, host.elevated);
    return host.token_fails ? nullptr : token_handle;
  }
  bool merge_user_environment_block(bp::environment &env, HANDLE token) {
    EXPECT_EQ(token, token_handle);
    EXPECT_EQ(env["OWNED_LAUNCH_TEST"].to_string(), "original");
    env["OWNED_LAUNCH_TEST"] = "merged";
    return !host.merge_fails;
  }
  STARTUPINFOEXW create_startup_info(FILE *file, HANDLE *job, std::error_code &ec) {
    EXPECT_EQ(file, nullptr);
    EXPECT_EQ(job, nullptr);
    if (host.startup_fails) ec = std::make_error_code(std::errc::not_enough_memory);
    STARTUPINFOEXW result {};
    result.StartupInfo.cb = sizeof(result);
    result.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    result.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(&attributes_tag);
    return result;
  }
  void free_proc_thread_attr_list(LPPROC_THREAD_ATTRIBUTE_LIST list) {
    EXPECT_EQ(list, reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(&attributes_tag));
    ++host.freed_attributes;
  }
  std::error_code impersonate_current_user(HANDLE token, std::function<void()> callback) {
    EXPECT_EQ(token, token_handle);
    if (host.impersonation_fails) return std::make_error_code(std::errc::permission_denied);
    callback();
    if (host.after_create) host.after_create();
    if (host.post_throw) throw std::runtime_error("post-create failure");
    if (host.post_error) return std::make_error_code(std::errc::permission_denied);
    return {};
  }
  std::wstring create_environment_block(bp::environment &env) {
    EXPECT_EQ(env["OWNED_LAUNCH_TEST"].to_string(), "merged");
    return L"fake environment";
  }
  std::string expand_env_vars_in_cmd(const std::string &cmd, const bp::environment &env) {
    EXPECT_EQ(cmd, "original command");
    const auto value = std::find_if(env.cbegin(), env.cend(), [](const auto &entry) {
      return entry.get_name() == "OWNED_LAUNCH_TEST";
    });
    EXPECT_NE(value, env.cend());
    if (value != env.cend()) EXPECT_EQ(value->to_string(), "merged");
    return "expanded command";
  }
  std::wstring resolve_command_string(const std::string &cmd, const std::wstring &directory, HANDLE token, DWORD &) {
    EXPECT_EQ(cmd, "expanded command");
    EXPECT_EQ(directory, L"C:\\fake");
    EXPECT_EQ(token, host.system ? token_handle : nullptr);
    return L"resolved command";
  }
  bp::child create_boost_child_from_results(bool launched, const std::string &, std::error_code &, PROCESS_INFORMATION &process) {
    ++host.legacy_bridges;
    if (launched) {
      fake_process::close(process.hThread);
      fake_process::close(process.hProcess);
    }
    return {};
  }
}

TEST_F(OwnedProcess, BothCreationBranchesRetainOriginalHandlesWithoutTheBoostBridge) {
  for (const bool system : { false, true }) {
    for (const bool interactive : { false, true }) {
      host = {};
      host.system = system;
      host.interactive = interactive;
      host.elevated = interactive;
      {
        platf::owned_process_t owner;
        EXPECT_FALSE(launch(owner));
        EXPECT_TRUE(owner.started());
        EXPECT_EQ(owner.id(), pid);
        EXPECT_EQ(host.created, 1u);
        EXPECT_EQ(host.legacy_bridges, 0u);
        EXPECT_EQ(host.freed_attributes, 1u);
        EXPECT_EQ(closed(token_handle), 1u);
        EXPECT_EQ(closed(process_handle), 0u);
        EXPECT_EQ(closed(thread_handle), 0u);
      }
      EXPECT_EQ(closed(process_handle), 1u);
      EXPECT_EQ(closed(thread_handle), 1u);
    }
  }
}

TEST_F(OwnedProcess, PostCreateErrorOrExceptionCannotLoseOwnership) {
  for (const bool throws : { false, true }) {
    host = {};
    host.system = true;
    host.post_throw = throws;
    host.post_error = !throws;
    {
      platf::owned_process_t owner;
      host.after_create = [&]() {
        EXPECT_TRUE(owner.started());
        EXPECT_EQ(owner.id(), pid);
        EXPECT_EQ(closed(process_handle), 0u);
      };
      if (throws) EXPECT_THROW(launch(owner), std::runtime_error);
      else EXPECT_EQ(launch(owner), std::make_error_code(std::errc::permission_denied));
      EXPECT_TRUE(owner.started());
      EXPECT_EQ(host.legacy_bridges, 0u);
      EXPECT_EQ(closed(process_handle), 0u);
    }
    EXPECT_EQ(closed(process_handle), 1u);
    EXPECT_EQ(closed(thread_handle), 1u);
  }
}

TEST_F(OwnedProcess, PreCreationFailuresNeverCreateOrOwnAProcess) {
  for (int failure = 0; failure != 4; ++failure) {
    host = {};
    host.system = true;
    host.startup_fails = failure == 0;
    host.token_fails = failure == 1;
    host.merge_fails = failure == 2;
    host.impersonation_fails = failure == 3;
    platf::owned_process_t owner;
    EXPECT_TRUE(launch(owner));
    EXPECT_FALSE(owner.started());
    EXPECT_EQ(owner.observe(0).state, platf::process_observation_e::not_started);
    EXPECT_EQ(host.created, 0u);
    EXPECT_EQ(host.waited, 0u);
    EXPECT_EQ(closed(process_handle), 0u);
    EXPECT_EQ(closed(thread_handle), 0u);
  }
}

TEST_F(OwnedProcess, NativeRefusalPreservesItsErrorAndAllowsAnEmptySlotRetry) {
  for (const bool system : { false, true }) {
    host = {};
    host.system = system;
    host.creation_succeeds = false;
    platf::owned_process_t owner;
    auto ec = launch(owner);
    EXPECT_EQ(ec, std::error_code(ERROR_ACCESS_DENIED, std::system_category()));
    EXPECT_FALSE(owner.started());
    EXPECT_EQ(host.legacy_bridges, 0u);
    EXPECT_EQ(closed(process_handle), 0u);
    host.creation_succeeds = true;
    bp::environment env;
    env["OWNED_LAUNCH_TEST"] = "original";
    boost::filesystem::path directory { L"C:\\fake" };
    owner.launch(true, true, "original command", directory, env, nullptr, ec, nullptr);
    EXPECT_FALSE(ec);
    EXPECT_TRUE(owner.started());
    EXPECT_EQ(host.created, 2u);
  }
}

TEST_F(OwnedProcess, ReusingAnOwnedSlotRefusesBeforeAnotherCreation) {
  platf::owned_process_t owner;
  ASSERT_FALSE(launch(owner));
  EXPECT_THROW(launch(owner), std::logic_error);
  EXPECT_TRUE(owner.started());
  EXPECT_EQ(host.created, 1u);
  EXPECT_EQ(closed(process_handle), 0u);
}

TEST_F(OwnedProcess, TimeoutAndWaitErrorsPreserveOwnershipAndCannotBecomeExit) {
  platf::owned_process_t owner;
  ASSERT_FALSE(launch(owner));
  for (const DWORD result : std::initializer_list<DWORD> { WAIT_TIMEOUT, WAIT_FAILED, WAIT_ABANDONED }) {
    host.wait_result = result;
    host.native_error = ERROR_ACCESS_DENIED;
    const auto observed = owner.observe(17);
    EXPECT_EQ(observed.state, result == WAIT_TIMEOUT ? platf::process_observation_e::running : platf::process_observation_e::failed);
    EXPECT_EQ(static_cast<bool>(observed.error), result != WAIT_TIMEOUT);
    EXPECT_FALSE(observed.exit_code);
    EXPECT_EQ(host.queries, 0u);
    EXPECT_EQ(host.timeout, 17u);
    EXPECT_TRUE(owner.started());
    EXPECT_EQ(closed(process_handle), 0u);
  }
  host.wait_result = WAIT_OBJECT_0;
  const auto settled = owner.observe(0);
  EXPECT_EQ(settled.state, platf::process_observation_e::exited);
  EXPECT_EQ(settled.exit_code, 0u);
}

TEST_F(OwnedProcess, ASignalIsNotConfusedWithExitCode259OrAQueryError) {
  platf::owned_process_t owner;
  ASSERT_FALSE(launch(owner));
  host.wait_result = WAIT_OBJECT_0;
  for (const DWORD code : { 0u, 1u, 259u }) {
    host.code = code;
    const auto observed = owner.observe(0);
    EXPECT_EQ(observed.state, platf::process_observation_e::exited);
    EXPECT_EQ(observed.exit_code, code);
    EXPECT_FALSE(observed.error);
  }
  host.query_succeeds = false;
  host.native_error = ERROR_ACCESS_DENIED;
  const auto unavailable = owner.observe(0);
  EXPECT_EQ(unavailable.state, platf::process_observation_e::exited);
  EXPECT_FALSE(unavailable.exit_code);
  EXPECT_EQ(unavailable.error, std::error_code(ERROR_ACCESS_DENIED, std::system_category()));
  EXPECT_EQ(closed(process_handle), 0u);
}

TEST_F(OwnedProcess, LegacyCallStillDelegatesToItsExistingBoostBridge) {
  for (const bool system : { false, true }) {
    host = {};
    host.system = system;
    bp::environment env;
    env["OWNED_LAUNCH_TEST"] = "original";
    boost::filesystem::path directory { L"C:\\fake" };
    std::error_code ec;
    auto legacy = platf::run_command(true, true, "original command", directory, env, nullptr, ec, nullptr);
    EXPECT_FALSE(ec);
    EXPECT_EQ(host.created, 1u);
    EXPECT_EQ(host.legacy_bridges, 1u);
    EXPECT_EQ(closed(process_handle), 1u);
    EXPECT_EQ(closed(thread_handle), 1u);
    EXPECT_EQ(env["OWNED_LAUNCH_TEST"].to_string(), "original");
  }
}
