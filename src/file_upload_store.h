/**
 * @file src/file_upload_store.h
 * @brief Client-to-host file uploads staged by Sunshine for the user-session agent.
 *
 * A paired client opens a batch, creates one upload per file (reserving its full
 * declared size against quotas), appends bytes at the current offset, then
 * commits the batch. Commit verifies every upload is complete and returns a
 * single manifest capability. Only the manifest id travels over the clipboard
 * control channel (wire kind 5); the agent resolves it locally to read the bytes.
 *
 * Everything is bound to the certificate-derived client UUID and the single
 * running stream session that UUID had when the batch was opened.
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace file_upload_store {
  using steady_t = std::chrono::steady_clock;

  struct limits_t {
    std::uint64_t max_file_bytes = 4ULL * 1024 * 1024 * 1024;
    std::uint64_t max_batch_bytes = 16ULL * 1024 * 1024 * 1024;
    std::uint32_t max_batch_files = 1000;
    std::uint64_t max_client_bytes = 8ULL * 1024 * 1024 * 1024;
    std::uint64_t max_total_bytes = 16ULL * 1024 * 1024 * 1024;
    std::uint32_t max_client_open_uploads = 64;
    std::uint32_t max_client_open_batches = 4;
    std::uint64_t max_chunk_bytes = 4ULL * 1024 * 1024;
    // Keep this much free on the staging volume beyond all reservations.
    std::uint64_t min_free_bytes = 1ULL * 1024 * 1024 * 1024;
    std::chrono::seconds idle_ttl { 60 * 60 };
    std::chrono::seconds manifest_ttl { 10 * 60 };
  };

  // Resolves the stream sessions currently RUNNING for a client UUID.
  using session_lookup_t = std::function<std::vector<std::uint32_t>(const std::string &client_uuid)>;
  // Free bytes on the staging volume; injectable for tests.
  using free_space_t = std::function<std::uint64_t(const std::filesystem::path &)>;

  struct options_t {
    std::filesystem::path staging_dir;
    limits_t limits;
    session_lookup_t sessions;
    free_space_t free_space;
    std::function<steady_t::time_point()> now;
    // Tests only: runs while a chunk's staging file is open, outside the lock.
    std::function<void()> while_writing;
  };

  enum class error_e {
    none,
    bad_request,
    bad_name,
    too_large,
    too_many,
    quota,
    disk_full,
    no_session,
    ambiguous_session,
    not_found,
    offset_mismatch,
    incomplete,
    duplicate_path,
    io,
  };

  /// Stable lowercase identifier used in JSON error bodies.
  const char *
  to_string(error_e err);

  struct batch_created_t {
    error_e err = error_e::none;
    std::string batch_id;
  };

  struct upload_created_t {
    error_e err = error_e::none;
    std::string upload_id;
    std::uint64_t chunk_size = 0;
  };

  struct upload_state_t {
    error_e err = error_e::none;
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
    bool complete = false;
  };

  struct manifest_item_t {
    std::string name;  // sanitized final path segment
    std::string rel;  // validated relative directory ("" or "a/b"), '/' separated
    std::uint64_t size = 0;
    std::filesystem::path staged;
  };

  struct manifest_t {
    std::string id;
    std::string client_uuid;
    std::uint32_t session_id = 0;
    std::string intent;
    std::optional<double> x;
    std::optional<double> y;
    std::uint64_t total_size = 0;
    std::vector<manifest_item_t> items;
  };

  struct commit_result_t {
    error_e err = error_e::none;
    manifest_t manifest;
  };

  /// Returns the cleaned single path segment, or empty if it cannot be made safe.
  std::string
  sanitize_name(std::string_view utf8_name);
  /// Validates a '/'- or '\\'-separated relative directory; returns the normalized
  /// '/'-separated form, or nullopt when any segment is unsafe.
  std::optional<std::string>
  validate_rel(std::string_view utf8_rel);

  class store_t {
  public:
    explicit store_t(options_t options);
    ~store_t();

    store_t(const store_t &) = delete;
    store_t &
    operator=(const store_t &) = delete;

    /// Removes and recreates the staging directory; uploads never survive a restart.
    bool
    reset_staging();

    batch_created_t
    open_batch(const std::string &client_uuid, std::uint32_t count, std::uint64_t total_size);
    upload_created_t
    create_upload(const std::string &client_uuid, const std::string &batch_id, std::string_view name, std::string_view rel, std::uint64_t size);
    upload_state_t
    append(const std::string &client_uuid, const std::string &upload_id, std::uint64_t offset, std::string_view bytes);
    upload_state_t
    state(const std::string &client_uuid, const std::string &upload_id);
    error_e
    cancel_upload(const std::string &client_uuid, const std::string &upload_id);
    error_e
    cancel_batch(const std::string &client_uuid, const std::string &batch_id);
    commit_result_t
    commit(const std::string &client_uuid, const std::string &batch_id, std::string intent, std::optional<double> x, std::optional<double> y);

    /// Agent side: resolve a manifest for the stream session that delivered it.
    std::optional<manifest_t>
    manifest(const std::string &manifest_id, std::uint32_t session_id);
    /// Agent side: release a manifest and delete its staged files.
    bool
    release_manifest(const std::string &manifest_id, std::uint32_t session_id);

    void
    sweep();

    /// Tests only; see options_t::while_writing.
    void
    set_while_writing_for_test(std::function<void()> hook);

    struct stats_t {
      std::size_t batches = 0;
      std::size_t uploads = 0;
      std::size_t manifests = 0;
      std::uint64_t reserved_bytes = 0;
    };

    stats_t
    stats();

  private:
    struct impl_t;
    std::unique_ptr<impl_t> impl_;
  };

  /// Process-wide store used by the HTTP routes (null until init()).
  store_t *
  instance();
  void
  init(options_t options);
  void
  shutdown();
}  // namespace file_upload_store
