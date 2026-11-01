/**
 * @file src/display_device/topology_durable_file.h
 * @brief Shared crash-safe single-file replacement for topology recovery records.
 */
#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "topology.h"

namespace display_device::topology {

  /** `path` with ".tmp" appended; an unfinished replacement never becomes the record. */
  std::filesystem::path
  durable_temp_path(const std::filesystem::path &path);

  /**
   * Replace `path` with `bytes` via the temp file, flush and atomic rename.
   * When this returns success the new contents are durable; on failure the
   * previous file is left intact. Errors name the file by `noun`.
   */
  result_t<void>
  replace_file_durably(const std::filesystem::path &path, const std::string &bytes, std::string_view noun);

  enum class durable_read_e {
    absent,
    read,
    unreadable,
  };

  struct durable_read_t {
    durable_read_e status { durable_read_e::absent };
    std::string bytes;
    std::string detail;
  };

  /** Read at most `limit` bytes; a missing file is `absent`, never an error. */
  durable_read_t
  read_file_bounded(const std::filesystem::path &path, std::size_t limit, std::string_view noun);

  /** Remove `path` and its temp file. Succeeds when both are already absent. */
  result_t<void>
  remove_file_durably(const std::filesystem::path &path, std::string_view noun);

}  // namespace display_device::topology
