#include "owned_process.h"
#include "run_command_helpers.h"

#include <boost/algorithm/string.hpp>
#include <mutex>

#include "src/logging.h"
#include "src/utility.h"

namespace bp = boost::process::v1;
using namespace std::literals;

namespace platf {
  namespace {
    std::mutex launch_path_mutex;

    std::error_code process_error(DWORD value) noexcept {
      return value ? std::error_code(static_cast<int>(value), std::system_category()) :
                     std::make_error_code(std::errc::io_error);
    }
  }

  owned_process_t::~owned_process_t() {
    if (process_.hThread) CloseHandle(process_.hThread);
    if (process_.hProcess) CloseHandle(process_.hProcess);
  }

  bool
  owned_process_t::started() const noexcept {
    return process_.hProcess != nullptr;
  }

  DWORD
  owned_process_t::id() const noexcept {
    return process_.dwProcessId;
  }

  process_observation_t
  owned_process_t::observe(DWORD timeout_ms) const noexcept {
    if (!started()) return {};
    const auto waited = WaitForSingleObject(process_.hProcess, timeout_ms);
    if (waited == WAIT_TIMEOUT) return { process_observation_e::running, {}, {} };
    if (waited != WAIT_OBJECT_0) {
      const auto error = waited == WAIT_FAILED ? GetLastError() : ERROR_INVALID_FUNCTION;
      return { process_observation_e::failed, {}, process_error(error) };
    }
    DWORD code {};
    if (!GetExitCodeProcess(process_.hProcess, &code)) {
      const auto error = GetLastError();
      return { process_observation_e::exited, {}, process_error(error) };
    }
    return { process_observation_e::exited, code, {} };
  }

  void
  owned_process_t::launch(bool elevated, bool interactive, const std::string &cmd,
    boost::filesystem::path &working_dir, const bp::environment &env,
    FILE *file, std::error_code &ec, bp::group *group) {
    if (started()) throw std::logic_error { "An owned process slot cannot be reused" };
    ec.clear();
    run_impl(elevated, interactive, cmd, working_dir, env, file, ec, group, this);
  }

  bp::child
  run_command(bool elevated, bool interactive, const std::string &cmd, boost::filesystem::path &working_dir,
    const bp::environment &env, FILE *file, std::error_code &ec, bp::group *group) {
    return owned_process_t::run_impl(elevated, interactive, cmd, working_dir, env, file, ec, group, nullptr);
  }

  bp::child
  owned_process_t::run_impl(bool elevated, bool interactive, const std::string &cmd, boost::filesystem::path &working_dir,
    const bp::environment &env, FILE *file, std::error_code &ec, bp::group *group, owned_process_t *owner) {
    std::wstring start_dir = working_dir.wstring();

    // Validate lpCurrentDirectory before CreateProcessW for an actionable log entry.
    if (!start_dir.empty()) {
      // Keep drive roots such as "C:\" intact.
      std::wstring probe_dir = start_dir;
      while (probe_dir.size() > 1 && (probe_dir.back() == L'\\' || probe_dir.back() == L'/') && probe_dir[probe_dir.size() - 2] != L':') {
        probe_dir.pop_back();
      }

      const auto probe_attributes = GetFileAttributesW(probe_dir.c_str());
      if (probe_attributes == INVALID_FILE_ATTRIBUTES ||
          (probe_attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        const auto probe_error = probe_attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_DIRECTORY;
        BOOST_LOG(warning) << "Working directory ["sv << to_utf8(start_dir) << "] does not exist or is not accessible (error "
                           << util::hex(probe_error).to_string_view() << ")."sv;
      }
    }

    HANDLE job = group ? group->native_handle() : nullptr;
    STARTUPINFOEXW startup_info = create_startup_info(file, job ? &job : nullptr, ec);
    PROCESS_INFORMATION process_info {};
    DWORD creation_error {};

    // Clone the environment to create a local copy. Boost.Process (bp) shares the environment with all spawned processes.
    // Since we're going to modify the 'env' variable by merging user-specific environment variables into it,
    // we make a clone to prevent side effects to the shared environment.
    bp::environment cloned_env = env;

    if (ec) {
      // In the event that startup_info failed, return a blank child process.
      return bp::child();
    }

    // Use RAII to ensure the attribute list is freed when we're done with it
    auto attr_list_free = util::fail_guard([list = startup_info.lpAttributeList]() {
      free_proc_thread_attr_list(list);
    });

    DWORD creation_flags = EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | CREATE_BREAKAWAY_FROM_JOB;

    // Create a new console for interactive processes and use no console for non-interactive processes
    creation_flags |= interactive ? CREATE_NEW_CONSOLE : CREATE_NO_WINDOW;

    // Both launch APIs temporarily change the same process-global PATH.
    std::lock_guard path_lock { launch_path_mutex };

    // Find the PATH variable in our environment block using a case-insensitive search
    auto sunshine_wenv = boost::this_process::wenvironment();
    std::wstring path_var_name { L"PATH" };
    std::wstring old_path_val;
    auto itr = std::find_if(sunshine_wenv.cbegin(), sunshine_wenv.cend(), [&](const auto &e) { return boost::iequals(e.get_name(), path_var_name); });
    if (itr != sunshine_wenv.cend()) {
      // Use the existing variable if it exists, since Boost treats these as case-sensitive.
      path_var_name = itr->get_name();
      old_path_val = sunshine_wenv[path_var_name].to_string();
    }

    // Temporarily prepend the specified working directory to PATH to ensure CreateProcess()
    // will (preferentially) find binaries that reside in the working directory.
    sunshine_wenv[path_var_name].assign(start_dir + L";" + old_path_val);

    // Restore the old PATH value for our process when we're done here
    auto restore_path = util::fail_guard([&]() {
      if (old_path_val.empty()) {
        sunshine_wenv[path_var_name].clear();
      }
      else {
        sunshine_wenv[path_var_name].assign(old_path_val);
      }
    });

    BOOL ret {};
    if (is_running_as_system()) {
      // Duplicate the current user's token
      HANDLE user_token = retrieve_users_token(elevated);
      if (!user_token) {
        // Fail the launch rather than risking launching with Sunshine's permissions unmodified.
        ec = std::make_error_code(std::errc::permission_denied);
        return bp::child();
      }

      // Use RAII to ensure the shell token is closed when we're done with it
      auto token_close = util::fail_guard([user_token]() {
        CloseHandle(user_token);
      });

      // Populate env with user-specific environment variables
      if (!merge_user_environment_block(cloned_env, user_token)) {
        ec = std::make_error_code(std::errc::not_enough_memory);
        return bp::child();
      }

      // Open the process as the current user account, elevation is handled in the token itself.
      ec = impersonate_current_user(user_token, [&]() {
        std::wstring env_block = create_environment_block(cloned_env);
        // Expand environment variables in the command using cloned_env (which contains SUNSHINE_* vars)
        std::string expanded_cmd = expand_env_vars_in_cmd(cmd, cloned_env);
        std::wstring wcmd = resolve_command_string(expanded_cmd, start_dir, user_token, creation_flags);
        ret = CreateProcessAsUserW(user_token,
          NULL,
          (LPWSTR) wcmd.c_str(),
          NULL,
          NULL,
          !!(startup_info.StartupInfo.dwFlags & STARTF_USESTDHANDLES),
          creation_flags,
          env_block.data(),
          start_dir.empty() ? NULL : start_dir.c_str(),
          (LPSTARTUPINFOW) &startup_info,
          &process_info);
        if (owner) {
          if (ret) owner->process_ = process_info;
          else creation_error = GetLastError();
        }
      });
    }
    // Otherwise, launch the process using CreateProcessW()
    // This will inherit the elevation of whatever the user launched Sunshine with.
    else {
      // Open our current token to resolve environment variables
      HANDLE process_token;
      if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &process_token)) {
        ec = std::make_error_code(std::errc::permission_denied);
        return bp::child();
      }
      auto token_close = util::fail_guard([process_token]() {
        CloseHandle(process_token);
      });

      // Populate env with user-specific environment variables
      if (!merge_user_environment_block(cloned_env, process_token)) {
        ec = std::make_error_code(std::errc::not_enough_memory);
        return bp::child();
      }

      std::wstring env_block = create_environment_block(cloned_env);
      // Expand environment variables in the command using cloned_env (which contains SUNSHINE_* vars)
      std::string expanded_cmd = expand_env_vars_in_cmd(cmd, cloned_env);
      std::wstring wcmd = resolve_command_string(expanded_cmd, start_dir, NULL, creation_flags);
      ret = CreateProcessW(NULL,
        (LPWSTR) wcmd.c_str(),
        NULL,
        NULL,
        !!(startup_info.StartupInfo.dwFlags & STARTF_USESTDHANDLES),
        creation_flags,
        env_block.data(),
        start_dir.empty() ? NULL : start_dir.c_str(),
        (LPSTARTUPINFOW) &startup_info,
        &process_info);
      if (owner) {
        if (ret) owner->process_ = process_info;
        else creation_error = GetLastError();
      }
    }

    if (owner) {
      if (!ret && !ec) ec = process_error(creation_error);
      return {};
    }

    // Use the results of the launch to create a bp::child object
    return create_boost_child_from_results(ret, cmd, ec, process_info);
  }
}  // namespace platf
