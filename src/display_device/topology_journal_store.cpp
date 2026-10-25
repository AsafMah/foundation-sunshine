/**
 * @file src/display_device/topology_journal_store.cpp
 * @brief Durable storage for the single native topology recovery journal.
 */
#include "topology_journal_store.h"

#include <fstream>
#include <iterator>
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
    // Larger than any journal `serialize_journal` accepts, so oversize files are
    // still read far enough for deserialization to reject them.
    constexpr std::streamsize read_limit = 1024 * 1024 + 1;

    error_t
    io_error(std::string detail) {
      return error_t { error_code_e::resource_unavailable, std::move(detail), {} };
    }

#ifdef _WIN32
    std::string
    last_error_text(const char *what, DWORD code) {
      return std::string(what) + " (Windows error " + std::to_string(code) + ")";
    }

    result_t<void>
    write_durably(const std::filesystem::path &temp, const std::filesystem::path &target, const std::string &bytes) {
      HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (file == INVALID_HANDLE_VALUE) {
        return std::unexpected(io_error(last_error_text("Cannot create the journal temp file", GetLastError())));
      }
      DWORD written {};
      bool ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
                written == bytes.size();
      ok = ok && FlushFileBuffers(file);
      const auto write_error = GetLastError();
      CloseHandle(file);
      if (!ok) {
        DeleteFileW(temp.c_str());
        return std::unexpected(io_error(last_error_text("Cannot write the journal temp file", write_error)));
      }
      if (!MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const auto move_error = GetLastError();
        DeleteFileW(temp.c_str());
        return std::unexpected(io_error(last_error_text("Cannot replace the journal file", move_error)));
      }
      return {};
    }
#else
    std::string
    errno_text(const char *what, int code) {
      return std::string(what) + " (errno " + std::to_string(code) + ")";
    }

    result_t<void>
    write_durably(const std::filesystem::path &temp, const std::filesystem::path &target, const std::string &bytes) {
      const int file = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
      if (file < 0) {
        return std::unexpected(io_error(errno_text("Cannot create the journal temp file", errno)));
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
        return std::unexpected(io_error(errno_text("Cannot write the journal temp file", write_error)));
      }
      if (std::rename(temp.c_str(), target.c_str()) != 0) {
        const int move_error = errno;
        ::unlink(temp.c_str());
        return std::unexpected(io_error(errno_text("Cannot replace the journal file", move_error)));
      }
      const auto parent = target.has_parent_path() ? target.parent_path() : std::filesystem::path(".");
  #ifdef O_DIRECTORY
      const int directory = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  #else
      const int directory = ::open(parent.c_str(), O_RDONLY | O_CLOEXEC);
  #endif
      if (directory < 0) {
        return std::unexpected(io_error(errno_text("Cannot open the journal directory", errno)));
      }
      // The rename is durable only once the directory is.
      const bool synced = ::fsync(directory) == 0;
      const int sync_error = errno;
      ::close(directory);
      if (!synced) {
        return std::unexpected(io_error(errno_text("Cannot flush the journal directory", sync_error)));
      }
      return {};
    }
#endif
  }  // namespace

  std::string_view
  to_string(journal_load_status_e status) noexcept {
    switch (status) {
      case journal_load_status_e::absent:
        return "absent";
      case journal_load_status_e::loaded:
        return "loaded";
      case journal_load_status_e::invalid:
        return "invalid";
      case journal_load_status_e::unsupported_version:
        return "unsupported_version";
      case journal_load_status_e::unreadable:
        return "unreadable";
    }
    return "unknown";
  }

  file_journal_store_t::file_journal_store_t(std::filesystem::path path):
      path_(std::move(path)) {}

  std::filesystem::path
  file_journal_store_t::temp_path() const {
    auto temp = path_;
    temp += ".tmp";
    return temp;
  }

  journal_load_t
  file_journal_store_t::load() {
    std::error_code error;
    const auto status = std::filesystem::status(path_, error);
    if (status.type() == std::filesystem::file_type::not_found) {
      return { journal_load_status_e::absent, std::nullopt, {} };
    }
    if (error) {
      return { journal_load_status_e::unreadable, std::nullopt, "Cannot inspect the journal file: " + error.message() };
    }
    std::ifstream file(path_, std::ios::binary);
    if (!file) {
      return { journal_load_status_e::unreadable, std::nullopt, "Cannot open the journal file" };
    }
    std::string bytes(static_cast<std::size_t>(read_limit), '\0');
    file.read(bytes.data(), read_limit);
    if (file.bad()) {
      return { journal_load_status_e::unreadable, std::nullopt, "Cannot read the journal file" };
    }
    bytes.resize(static_cast<std::size_t>(file.gcount()));

    auto record = deserialize_journal(bytes);
    if (!record) {
      const auto load_status = record.error().code == error_code_e::unsupported_version ?
                                 journal_load_status_e::unsupported_version :
                                 journal_load_status_e::invalid;
      return { load_status, std::nullopt, record.error().detail };
    }
    return { journal_load_status_e::loaded, std::move(*record), {} };
  }

  result_t<void>
  file_journal_store_t::save(const journal_record_t &record) {
    auto bytes = serialize_journal(record);
    if (!bytes) return std::unexpected(bytes.error());
    return write_durably(temp_path(), path_, *bytes);
  }

  result_t<void>
  file_journal_store_t::clear() {
    std::error_code error;
    std::filesystem::remove(path_, error);
    if (error) {
      return std::unexpected(io_error("Cannot remove the journal file: " + error.message()));
    }
    std::filesystem::remove(temp_path(), error);
    if (error) {
      return std::unexpected(io_error("Cannot remove the journal temp file: " + error.message()));
    }
    return {};
  }

}  // namespace display_device::topology
