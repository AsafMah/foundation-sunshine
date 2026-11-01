/**
 * @file src/display_device/topology_journal_store.cpp
 * @brief Durable storage for the single native topology recovery journal.
 */
#include "topology_journal_store.h"

#include "topology_durable_file.h"

#include <utility>

namespace display_device::topology {

  namespace {
    // Larger than any journal `serialize_journal` accepts, so oversize files are
    // still read far enough for deserialization to reject them.
    constexpr std::size_t read_limit = 1024 * 1024 + 1;
    constexpr std::string_view noun = "journal";
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
    return durable_temp_path(path_);
  }

  journal_load_t
  file_journal_store_t::load() {
    auto read = read_file_bounded(path_, read_limit, noun);
    if (read.status == durable_read_e::absent) {
      return { journal_load_status_e::absent, std::nullopt, {} };
    }
    if (read.status == durable_read_e::unreadable) {
      return { journal_load_status_e::unreadable, std::nullopt, std::move(read.detail) };
    }
    auto record = deserialize_journal(read.bytes);
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
    return replace_file_durably(path_, *bytes, noun);
  }

  result_t<void>
  file_journal_store_t::clear() {
    return remove_file_durably(path_, noun);
  }

}  // namespace display_device::topology