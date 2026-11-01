#pragma once

#include "src/platform/run_command.h"

#include <optional>
#include <windows.h>

namespace platf {
  enum class process_observation_e {
    not_started,
    running,
    exited,
    failed,
  };

  struct process_observation_t {
    process_observation_e state { process_observation_e::not_started };
    std::optional<DWORD> exit_code;
    std::error_code error;
  };

  /** Single-thread-confined ownership of the original creation handles; never terminates a child. */
  class owned_process_t {
  public:
    owned_process_t() = default;
    ~owned_process_t();
    owned_process_t(const owned_process_t &) = delete;
    owned_process_t &operator=(const owned_process_t &) = delete;

    /** Only an empty slot may launch; a later error/exception does not undo ownership. */
    void launch(bool elevated, bool interactive, const std::string &cmd,
      boost::filesystem::path &working_dir, const boost::process::v1::environment &env,
      FILE *file, std::error_code &ec, boost::process::v1::group *group);

    bool started() const noexcept;
    DWORD id() const noexcept;
    /** A signaled process can still have an unavailable exit code, reported separately as an error. */
    process_observation_t observe(DWORD timeout_ms) const noexcept;

  private:
    friend boost::process::v1::child run_command(bool, bool, const std::string &,
      boost::filesystem::path &, const boost::process::v1::environment &,
      FILE *, std::error_code &, boost::process::v1::group *);

    static boost::process::v1::child run_impl(bool elevated, bool interactive, const std::string &cmd,
      boost::filesystem::path &working_dir, const boost::process::v1::environment &env,
      FILE *file, std::error_code &ec, boost::process::v1::group *group, owned_process_t *owner);

    PROCESS_INFORMATION process_ {};
  };
}  // namespace platf
