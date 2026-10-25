/**
 * @file src/display_device/topology_journal_store.h
 * @brief Durable storage for the single native topology recovery journal.
 *
 * The journal is write-ahead: callers save intent before the platform action it
 * describes, so `save` must be durable when it returns. Loading never deletes or
 * rewrites a file it cannot use; an unusable journal is reported so recovery can
 * fail closed (In conflict) instead of guessing.
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "topology_journal.h"

namespace display_device::topology {

  enum class journal_load_status_e : std::uint8_t {
    absent,
    loaded,
    invalid,
    unsupported_version,
    unreadable,
  };

  std::string_view
  to_string(journal_load_status_e status) noexcept;

  struct journal_load_t {
    journal_load_status_e status { journal_load_status_e::absent };
    std::optional<journal_record_t> record;
    std::string detail;

    /** Whether a journal exists in any form, usable or not. */
    bool
    present() const noexcept {
      return status != journal_load_status_e::absent;
    }
  };

  class journal_store_t {
  public:
    virtual ~journal_store_t() = default;

    virtual journal_load_t
    load() = 0;

    /** Replace the journal durably; on failure the previous journal is left intact. */
    virtual result_t<void>
    save(const journal_record_t &record) = 0;

    /** Remove the journal. Succeeds when it is already absent. */
    virtual result_t<void>
    clear() = 0;
  };

  /**
   * @brief Journal in one file, replaced via temp file, flush and atomic rename.
   * @details A leftover temp file is an unfinished save whose platform action never
   *          started, so `load` ignores it and the next `save` overwrites it.
   */
  class file_journal_store_t final: public journal_store_t {
  public:
    explicit file_journal_store_t(std::filesystem::path path);

    journal_load_t
    load() override;

    result_t<void>
    save(const journal_record_t &record) override;

    result_t<void>
    clear() override;

    const std::filesystem::path &
    path() const noexcept {
      return path_;
    }

    std::filesystem::path
    temp_path() const;

  private:
    std::filesystem::path path_;
  };

}  // namespace display_device::topology
