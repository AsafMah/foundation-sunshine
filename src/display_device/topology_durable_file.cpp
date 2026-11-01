/**
 * @file src/display_device/topology_durable_file.cpp
 * @brief Shared crash-safe single-file replacement for topology recovery records.
 */
#include "topology_durable_file.h"

#include <fstream>
#include <system_error>
#include <utility>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
#else
  #include <cerrno>
  #include <cstdio>
  #include <fcntl.h>
  #include <unistd.h>
#endif

namespace display_device::topology {

  namespace {
    error_t
    io_error(std::string detail) {
      return error_t { error_code_e::resource_unavailable, std::move(detail), {} };
    }

    std::string
    what(const char *prefix, std::string_view noun, const char *suffix) {
      return std::string(prefix) + std::string(noun) + suffix;
    }

#ifdef _WIN32
    std::string
    last_error_text(const std::string &text, DWORD code) {
      return text + " (Windows error " + std::to_string(code) + ")";
    }

    result_t<void>
    write_durably(const std::filesystem::path &temp, const std::filesystem::path &target, const std::string &bytes, std::string_view noun) {
      HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (file == INVALID_HANDLE_VALUE) {
        return std::unexpected(io_error(last_error_text(what("Cannot create the ", noun, " temp file"), GetLastError())));
      }
      DWORD written {};
      bool ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
                written == bytes.size();
      ok = ok && FlushFileBuffers(file);
      const auto write_error = GetLastError();
      CloseHandle(file);
      if (!ok) {
        DeleteFileW(temp.c_str());
        return std::unexpected(io_error(last_error_text(what("Cannot write the ", noun, " temp file"), write_error)));
      }
      if (!MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const auto move_error = GetLastError();
        DeleteFileW(temp.c_str());
        return std::unexpected(io_error(last_error_text(what("Cannot replace the ", noun, " file"), move_error)));
      }
      return {};
    }
#else
    std::string
    errno_text(const std::string &text, int code) {
      return text + " (errno " + std::to_string(code) + ")";
    }

    result_t<void>
    write_durably(const std::filesystem::path &temp, const std::filesystem::path &target, const std::string &bytes, std::string_view noun) {
      const int file = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
      if (file < 0) {
        return std::unexpected(io_error(errno_text(what("Cannot create the ", noun, " temp file"), errno)));
      }
      std::size_t offset {};
      while (offset < bytes.size()) {
        const auto count = ::write(file, bytes.data() + offset, bytes.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        offset += static_cast<std::size_t>(count);
      }
      const bool ok = offset == bytes.size() && ::fsync(file) == 0;
      const int write_error = errno;
      ::close(file);
      if (!ok) {
        ::unlink(temp.c_str());
        return std::unexpected(io_error(errno_text(what("Cannot write the ", noun, " temp file"), write_error)));
      }
      if (std::rename(temp.c_str(), target.c_str()) != 0) {
        const int move_error = errno;
        ::unlink(temp.c_str());
        return std::unexpected(io_error(errno_text(what("Cannot replace the ", noun, " file"), move_error)));
      }
      const auto parent = target.has_parent_path() ? target.parent_path() : std::filesystem::path(".");
  #ifdef O_DIRECTORY
      const int directory = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  #else
      const int directory = ::open(parent.c_str(), O_RDONLY | O_CLOEXEC);
  #endif
      if (directory < 0) {
        return std::unexpected(io_error(errno_text(what("Cannot open the ", noun, " directory"), errno)));
      }
      // The rename is durable only once the directory is.
      const bool synced = ::fsync(directory) == 0;
      const int sync_error = errno;
      ::close(directory);
      if (!synced) {
        return std::unexpected(io_error(errno_text(what("Cannot flush the ", noun, " directory"), sync_error)));
      }
      return {};
    }
#endif
  }  // namespace

  std::filesystem::path
  durable_temp_path(const std::filesystem::path &path) {
    auto temp = path;
    temp += ".tmp";
    return temp;
  }

  result_t<void>
  replace_file_durably(const std::filesystem::path &path, const std::string &bytes, std::string_view noun) {
    return write_durably(durable_temp_path(path), path, bytes, noun);
  }

  durable_read_t
  read_file_bounded(const std::filesystem::path &path, std::size_t limit, std::string_view noun) {
    std::error_code error;
    const auto status = std::filesystem::status(path, error);
    if (status.type() == std::filesystem::file_type::not_found) {
      return { durable_read_e::absent, {}, {} };
    }
    if (error) {
      return { durable_read_e::unreadable, {}, what("Cannot inspect the ", noun, " file: ") + error.message() };
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) {
      return { durable_read_e::unreadable, {}, what("Cannot open the ", noun, " file") };
    }
    std::string bytes(limit, '\0');
    file.read(bytes.data(), static_cast<std::streamsize>(limit));
    if (file.bad()) {
      return { durable_read_e::unreadable, {}, what("Cannot read the ", noun, " file") };
    }
    bytes.resize(static_cast<std::size_t>(file.gcount()));
    return { durable_read_e::read, std::move(bytes), {} };
  }

  result_t<void>
  remove_file_durably(const std::filesystem::path &path, std::string_view noun) {
    std::error_code error;
    std::filesystem::remove(path, error);
    if (error) {
      return std::unexpected(io_error(what("Cannot remove the ", noun, " file: ") + error.message()));
    }
    std::filesystem::remove(durable_temp_path(path), error);
    if (error) {
      return std::unexpected(io_error(what("Cannot remove the ", noun, " temp file: ") + error.message()));
    }
    return {};
  }

}  // namespace display_device::topology
