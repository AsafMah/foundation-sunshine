#pragma once

#include "src/platform/run_command.h"
#include "misc.h"

namespace platf {
  bool merge_user_environment_block(boost::process::v1::environment &env, HANDLE token);
  std::wstring create_environment_block(boost::process::v1::environment &env);
  STARTUPINFOEXW create_startup_info(FILE *file, HANDLE *job, std::error_code &ec);
  void free_proc_thread_attr_list(LPPROC_THREAD_ATTRIBUTE_LIST list);
  std::string expand_env_vars_in_cmd(const std::string &cmd, const boost::process::v1::environment &env);
  std::wstring resolve_command_string(const std::string &cmd, const std::wstring &working_dir, HANDLE token, DWORD &creation_flags);
  boost::process::v1::child create_boost_child_from_results(bool launched, const std::string &cmd,
    std::error_code &ec, PROCESS_INFORMATION &process);
}  // namespace platf
