/**
 * @file src/file_upload_store.cpp
 * @brief See file_upload_store.h.
 */
#include "file_upload_store.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <system_error>
#include <unordered_set>

#include <openssl/rand.h>

namespace file_upload_store {
  namespace {
    constexpr std::size_t kMaxNameBytes = 180;
    constexpr std::size_t kMaxRelBytes = 240;
    constexpr std::size_t kMaxRelDepth = 8;

    // Decodes UTF-8 strictly; returns false on any malformed sequence.
    bool
    decode_utf8(std::string_view in, std::vector<char32_t> &out) {
      out.clear();
      for (std::size_t i = 0; i < in.size();) {
        const auto c = static_cast<unsigned char>(in[i]);
        std::size_t len = 0;
        char32_t cp = 0;
        if (c < 0x80) {
          len = 1;
          cp = c;
        }
        else if ((c & 0xE0) == 0xC0) {
          len = 2;
          cp = c & 0x1F;
        }
        else if ((c & 0xF0) == 0xE0) {
          len = 3;
          cp = c & 0x0F;
        }
        else if ((c & 0xF8) == 0xF0) {
          len = 4;
          cp = c & 0x07;
        }
        else {
          return false;
        }
        if (i + len > in.size()) {
          return false;
        }
        for (std::size_t k = 1; k < len; ++k) {
          const auto cc = static_cast<unsigned char>(in[i + k]);
          if ((cc & 0xC0) != 0x80) {
            return false;
          }
          cp = (cp << 6) | (cc & 0x3F);
        }
        static constexpr char32_t kMin[] = { 0, 0, 0x80, 0x800, 0x10000 };
        if (cp < kMin[len] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
          return false;
        }
        out.push_back(cp);
        i += len;
      }
      return true;
    }

    void
    append_utf8(std::string &out, char32_t cp) {
      if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
      }
      else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
      }
      else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
      }
      else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
      }
    }

    bool
    is_forbidden(char32_t cp) {
      if (cp < 0x20 || cp == 0x7F) {
        return true;
      }
      // Bidirectional controls can disguise "txt.exe" as "exe.txt".
      if (cp == 0x200E || cp == 0x200F || (cp >= 0x202A && cp <= 0x202E) || (cp >= 0x2066 && cp <= 0x2069)) {
        return true;
      }
      switch (cp) {
        case '<':
        case '>':
        case ':':
        case '"':
        case '/':
        case '\\':
        case '|':
        case '?':
        case '*':
          return true;
        default:
          return false;
      }
    }

    char32_t
    ascii_upper(char32_t cp) {
      return cp >= 'a' && cp <= 'z' ? cp - 'a' + 'A' : cp;
    }

    // CON, PRN, AUX, NUL, COM0-9/¹²³, LPT0-9/¹²³ — also with any extension.
    bool
    is_reserved_device(const std::vector<char32_t> &segment) {
      std::vector<char32_t> base;
      for (char32_t cp : segment) {
        if (cp == '.') {
          break;
        }
        base.push_back(ascii_upper(cp));
      }
      while (!base.empty() && base.back() == ' ') {
        base.pop_back();
      }
      auto eq = [&](const char *word) {
        std::size_t n = 0;
        for (; word[n]; ++n) {
          if (n >= base.size() || base[n] != static_cast<char32_t>(word[n])) {
            return false;
          }
        }
        return n == base.size();
      };
      if (eq("CON") || eq("PRN") || eq("AUX") || eq("NUL")) {
        return true;
      }
      if (base.size() == 4 && (base[0] == 'C' || base[0] == 'L')) {
        const bool com = base[0] == 'C' && base[1] == 'O' && base[2] == 'M';
        const bool lpt = base[0] == 'L' && base[1] == 'P' && base[2] == 'T';
        const char32_t d = base[3];
        const bool digit = (d >= '0' && d <= '9') || d == 0xB9 || d == 0xB2 || d == 0xB3;
        return (com || lpt) && digit;
      }
      return false;
    }

    std::string
    encode(const std::vector<char32_t> &cps) {
      std::string out;
      for (char32_t cp : cps) {
        append_utf8(out, cp);
      }
      return out;
    }

    // Lowercased (ASCII) key used to detect case-insensitive path collisions.
    std::string
    fold_key(const std::string &path) {
      std::string out = path;
      std::transform(out.begin(), out.end(), out.begin(), [](char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
      });
      return out;
    }

    std::string
    make_id() {
      std::array<std::uint8_t, 32> raw {};
      if (RAND_bytes(raw.data(), static_cast<int>(raw.size())) != 1) {
        return {};
      }
      static constexpr char kHex[] = "0123456789abcdef";
      std::string out(raw.size() * 2, '0');
      for (std::size_t i = 0; i < raw.size(); ++i) {
        out[i * 2] = kHex[raw[i] >> 4];
        out[i * 2 + 1] = kHex[raw[i] & 0x0F];
      }
      return out;
    }

    void
    remove_file(const std::filesystem::path &path) {
      std::error_code ec;
      std::filesystem::remove(path, ec);
    }
  }  // namespace

  const char *
  to_string(error_e err) {
    switch (err) {
      case error_e::none:
        return "ok";
      case error_e::bad_request:
        return "bad_request";
      case error_e::bad_name:
        return "bad_name";
      case error_e::too_large:
        return "too_large";
      case error_e::too_many:
        return "too_many";
      case error_e::quota:
        return "quota_exceeded";
      case error_e::disk_full:
        return "disk_full";
      case error_e::no_session:
        return "no_session";
      case error_e::ambiguous_session:
        return "ambiguous_session";
      case error_e::not_found:
        return "not_found";
      case error_e::offset_mismatch:
        return "offset_mismatch";
      case error_e::incomplete:
        return "incomplete";
      case error_e::duplicate_path:
        return "duplicate_path";
      case error_e::io:
        return "io_error";
    }
    return "error";
  }

  std::string
  sanitize_name(std::string_view utf8_name) {
    std::vector<char32_t> cps;
    if (!decode_utf8(utf8_name, cps)) {
      return {};
    }
    const auto sep = std::find_if(cps.rbegin(), cps.rend(), [](char32_t c) { return c == '/' || c == '\\'; });
    std::vector<char32_t> name(sep.base(), cps.end());
    for (char32_t &cp : name) {
      if (is_forbidden(cp)) {
        cp = '_';
      }
    }
    while (!name.empty() && name.front() == ' ') {
      name.erase(name.begin());
    }
    while (!name.empty() && (name.back() == '.' || name.back() == ' ')) {
      name.pop_back();
    }
    if (name.empty()) {
      return {};
    }
    if (is_reserved_device(name)) {
      name.insert(name.begin(), '_');
    }

    std::string out = encode(name);
    if (out.size() > kMaxNameBytes) {
      std::vector<char32_t> suffix;
      const auto dot = std::find(name.rbegin(), name.rend(), U'.');
      if (dot != name.rend()) {
        suffix.assign(dot.base() - 1, name.end());
        if (encode(suffix).size() > 16) {
          suffix.clear();
        }
      }
      const std::size_t suffix_bytes = encode(suffix).size();
      std::vector<char32_t> stem;
      std::size_t bytes = 0;
      for (char32_t cp : name) {
        std::string one;
        append_utf8(one, cp);
        if (bytes + one.size() + suffix_bytes > kMaxNameBytes) {
          break;
        }
        bytes += one.size();
        stem.push_back(cp);
      }
      while (!stem.empty() && (stem.back() == '.' || stem.back() == ' ')) {
        stem.pop_back();
      }
      if (stem.empty()) {
        return {};
      }
      stem.insert(stem.end(), suffix.begin(), suffix.end());
      out = encode(stem);
    }
    return out;
  }

  std::optional<std::string>
  validate_rel(std::string_view utf8_rel) {
    if (utf8_rel.empty()) {
      return std::string {};
    }
    if (utf8_rel.size() > kMaxRelBytes) {
      return std::nullopt;
    }
    std::vector<char32_t> cps;
    if (!decode_utf8(utf8_rel, cps)) {
      return std::nullopt;
    }

    std::vector<std::vector<char32_t>> segments(1);
    for (char32_t cp : cps) {
      if (cp == '/' || cp == '\\') {
        segments.emplace_back();
      }
      else {
        segments.back().push_back(cp);
      }
    }
    if (segments.size() > kMaxRelDepth) {
      return std::nullopt;
    }

    std::string out;
    for (const auto &segment : segments) {
      // Empty segments cover leading separators (absolute/UNC paths) and "a//b".
      if (segment.empty() || segment.front() == ' ' || segment.back() == '.' || segment.back() == ' ') {
        return std::nullopt;
      }
      if (std::any_of(segment.begin(), segment.end(), is_forbidden) || is_reserved_device(segment)) {
        return std::nullopt;
      }
      if (!out.empty()) {
        out.push_back('/');
      }
      out += encode(segment);
    }
    return out;
  }

  struct store_t::impl_t {
    struct upload_t {
      std::string id;
      std::string batch_id;
      std::string client_uuid;
      std::string name;
      std::string rel;
      std::uint64_t size = 0;
      std::uint64_t offset = 0;
      bool writing = false;
      std::filesystem::path path;
    };

    struct batch_t {
      std::string id;
      std::string client_uuid;
      std::uint32_t session_id = 0;
      std::uint32_t count = 0;
      std::uint64_t total_size = 0;
      std::vector<std::string> uploads;
      steady_t::time_point last_activity;
    };

    struct stored_manifest_t {
      manifest_t manifest;
      steady_t::time_point expires_at;
    };

    options_t opt;
    std::mutex mu;
    std::unordered_map<std::string, batch_t> batches;
    std::unordered_map<std::string, upload_t> uploads;
    std::unordered_map<std::string, stored_manifest_t> manifests;

    steady_t::time_point
    now() const {
      return opt.now ? opt.now() : steady_t::now();
    }

    std::uint64_t
    reserved_for(const std::string *client) const {
      std::uint64_t total = 0;
      for (const auto &[id, u] : uploads) {
        if (!client || u.client_uuid == *client) {
          total += u.size;
        }
      }
      for (const auto &[id, m] : manifests) {
        if (!client || m.manifest.client_uuid == *client) {
          total += m.manifest.total_size;
        }
      }
      return total;
    }

    std::uint64_t
    unwritten() const {
      std::uint64_t total = 0;
      for (const auto &[id, u] : uploads) {
        total += u.size - u.offset;
      }
      return total;
    }

    std::string
    unique_id() const {
      for (int i = 0; i < 8; ++i) {
        auto id = make_id();
        if (!id.empty() && !batches.count(id) && !uploads.count(id) && !manifests.count(id)) {
          return id;
        }
      }
      return {};
    }

    void
    drop_upload_locked(const std::string &id) {
      auto it = uploads.find(id);
      if (it == uploads.end()) {
        return;
      }
      // An append in progress still holds the file open (Windows can't delete it);
      // that writer removes it once it sees the upload is gone.
      if (!it->second.writing) {
        remove_file(it->second.path);
      }
      auto batch = batches.find(it->second.batch_id);
      if (batch != batches.end()) {
        auto &ids = batch->second.uploads;
        ids.erase(std::remove(ids.begin(), ids.end(), id), ids.end());
      }
      uploads.erase(it);
    }

    void
    drop_batch_locked(const std::string &id) {
      auto it = batches.find(id);
      if (it == batches.end()) {
        return;
      }
      const auto ids = it->second.uploads;
      for (const auto &upload_id : ids) {
        drop_upload_locked(upload_id);
      }
      batches.erase(id);
    }

    void
    sweep_locked() {
      const auto t = now();
      std::vector<std::string> expired;
      for (const auto &[id, b] : batches) {
        if (t - b.last_activity >= opt.limits.idle_ttl) {
          expired.push_back(id);
        }
      }
      for (const auto &id : expired) {
        drop_batch_locked(id);
      }
      for (auto it = manifests.begin(); it != manifests.end();) {
        if (t >= it->second.expires_at) {
          for (const auto &item : it->second.manifest.items) {
            remove_file(item.staged);
          }
          it = manifests.erase(it);
        }
        else {
          ++it;
        }
      }
    }

    upload_t *
    owned_upload(const std::string &client_uuid, const std::string &upload_id) {
      auto it = uploads.find(upload_id);
      return it == uploads.end() || it->second.client_uuid != client_uuid ? nullptr : &it->second;
    }

    batch_t *
    owned_batch(const std::string &client_uuid, const std::string &batch_id) {
      auto it = batches.find(batch_id);
      return it == batches.end() || it->second.client_uuid != client_uuid ? nullptr : &it->second;
    }
  };

  store_t::store_t(options_t options):
      impl_(std::make_unique<impl_t>()) {
    impl_->opt = std::move(options);
  }

  store_t::~store_t() = default;

  bool
  store_t::reset_staging() {
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->batches.clear();
    impl_->uploads.clear();
    impl_->manifests.clear();
    std::error_code ec;
    std::filesystem::remove_all(impl_->opt.staging_dir, ec);
    ec.clear();
    std::filesystem::create_directories(impl_->opt.staging_dir, ec);
    return !ec && std::filesystem::is_directory(impl_->opt.staging_dir, ec);
  }

  batch_created_t
  store_t::open_batch(const std::string &client_uuid, std::uint32_t count, std::uint64_t total_size) {
    auto &d = *impl_;
    if (client_uuid.empty() || count == 0) {
      return { error_e::bad_request, {} };
    }
    if (count > d.opt.limits.max_batch_files) {
      return { error_e::too_many, {} };
    }
    if (total_size > d.opt.limits.max_batch_bytes) {
      return { error_e::too_large, {} };
    }

    const auto sessions = d.opt.sessions ? d.opt.sessions(client_uuid) : std::vector<std::uint32_t> {};
    if (sessions.empty()) {
      return { error_e::no_session, {} };
    }
    if (sessions.size() > 1) {
      return { error_e::ambiguous_session, {} };
    }

    std::lock_guard<std::mutex> lk(d.mu);
    d.sweep_locked();
    const auto open = std::count_if(d.batches.begin(), d.batches.end(), [&](const auto &kv) {
      return kv.second.client_uuid == client_uuid;
    });
    if (static_cast<std::uint32_t>(open) >= d.opt.limits.max_client_open_batches) {
      return { error_e::too_many, {} };
    }

    impl_t::batch_t batch;
    batch.id = d.unique_id();
    if (batch.id.empty()) {
      return { error_e::io, {} };
    }
    batch.client_uuid = client_uuid;
    batch.session_id = sessions.front();
    batch.count = count;
    batch.total_size = total_size;
    batch.last_activity = d.now();
    const auto id = batch.id;
    d.batches.emplace(id, std::move(batch));
    return { error_e::none, id };
  }

  upload_created_t
  store_t::create_upload(const std::string &client_uuid, const std::string &batch_id, std::string_view name, std::string_view rel, std::uint64_t size) {
    auto &d = *impl_;
    std::lock_guard<std::mutex> lk(d.mu);
    d.sweep_locked();
    auto *batch = d.owned_batch(client_uuid, batch_id);
    if (!batch) {
      return { error_e::not_found, {}, 0 };
    }

    const auto clean_name = sanitize_name(name);
    const auto clean_rel = validate_rel(rel);
    if (clean_name.empty() || !clean_rel) {
      return { error_e::bad_name, {}, 0 };
    }
    if (batch->uploads.size() >= batch->count) {
      return { error_e::too_many, {}, 0 };
    }
    std::uint64_t batch_reserved = 0;
    for (const auto &id : batch->uploads) {
      batch_reserved += d.uploads.at(id).size;
    }
    if (size > d.opt.limits.max_file_bytes || batch_reserved + size > batch->total_size) {
      return { error_e::too_large, {}, 0 };
    }

    const auto open_uploads = std::count_if(d.uploads.begin(), d.uploads.end(), [&](const auto &kv) {
      return kv.second.client_uuid == client_uuid;
    });
    if (static_cast<std::uint32_t>(open_uploads) >= d.opt.limits.max_client_open_uploads) {
      return { error_e::too_many, {}, 0 };
    }
    if (d.reserved_for(&client_uuid) + size > d.opt.limits.max_client_bytes ||
        d.reserved_for(nullptr) + size > d.opt.limits.max_total_bytes) {
      return { error_e::quota, {}, 0 };
    }
    if (d.opt.free_space) {
      const auto free = d.opt.free_space(d.opt.staging_dir);
      if (free < d.unwritten() + size + d.opt.limits.min_free_bytes) {
        return { error_e::disk_full, {}, 0 };
      }
    }

    impl_t::upload_t upload;
    upload.id = d.unique_id();
    if (upload.id.empty()) {
      return { error_e::io, {}, 0 };
    }
    upload.batch_id = batch_id;
    upload.client_uuid = client_uuid;
    upload.name = clean_name;
    upload.rel = *clean_rel;
    upload.size = size;
    upload.path = d.opt.staging_dir / upload.id;
    {
      std::ofstream out(upload.path, std::ios::binary | std::ios::trunc);
      if (!out) {
        return { error_e::io, {}, 0 };
      }
    }

    batch->uploads.push_back(upload.id);
    batch->last_activity = d.now();
    const auto id = upload.id;
    d.uploads.emplace(id, std::move(upload));
    return { error_e::none, id, d.opt.limits.max_chunk_bytes };
  }

  upload_state_t
  store_t::append(const std::string &client_uuid, const std::string &upload_id, std::uint64_t offset, std::string_view bytes) {
    auto &d = *impl_;
    std::filesystem::path path;
    std::function<void()> while_writing;
    {
      std::lock_guard<std::mutex> lk(d.mu);
      d.sweep_locked();
      auto *upload = d.owned_upload(client_uuid, upload_id);
      if (!upload) {
        return { error_e::not_found };
      }
      if (upload->offset == upload->size) {
        // A retried final chunk (its response was lost) succeeds without rewriting.
        if (offset + bytes.size() == upload->size && offset < upload->size) {
          return { error_e::none, upload->offset, upload->size, true };
        }
        return { error_e::too_large, upload->offset, upload->size, true };
      }
      if (bytes.size() > d.opt.limits.max_chunk_bytes) {
        return { error_e::too_large, upload->offset, upload->size, false };
      }
      if (upload->writing || offset != upload->offset) {
        return { error_e::offset_mismatch, upload->offset, upload->size, false };
      }
      if (offset + bytes.size() > upload->size) {
        d.drop_upload_locked(upload_id);
        return { error_e::too_large };
      }
      upload->writing = true;
      path = upload->path;
      while_writing = d.opt.while_writing;
    }

    // Write outside the lock; `writing` serializes appends to this upload.
    bool ok = true;
    {
      std::ofstream out(path, std::ios::binary | std::ios::app);
      out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
      out.flush();
      ok = static_cast<bool>(out);
      if (while_writing) {
        while_writing();
      }
    }

    std::lock_guard<std::mutex> lk(d.mu);
    auto *upload = d.owned_upload(client_uuid, upload_id);
    if (!upload) {
      // Cancelled or expired while writing: the bytes are no longer accounted for.
      remove_file(path);
      return { error_e::not_found };
    }
    upload->writing = false;
    if (!ok) {
      std::error_code ec;
      std::filesystem::resize_file(path, upload->offset, ec);
      return { error_e::io, upload->offset, upload->size, false };
    }
    upload->offset += bytes.size();
    if (auto batch = d.batches.find(upload->batch_id); batch != d.batches.end()) {
      batch->second.last_activity = d.now();
    }
    return { error_e::none, upload->offset, upload->size, upload->offset == upload->size };
  }

  upload_state_t
  store_t::state(const std::string &client_uuid, const std::string &upload_id) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->sweep_locked();
    const auto *upload = impl_->owned_upload(client_uuid, upload_id);
    if (!upload) {
      return { error_e::not_found };
    }
    return { error_e::none, upload->offset, upload->size, upload->offset == upload->size };
  }

  error_e
  store_t::cancel_upload(const std::string &client_uuid, const std::string &upload_id) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (!impl_->owned_upload(client_uuid, upload_id)) {
      return error_e::not_found;
    }
    impl_->drop_upload_locked(upload_id);
    return error_e::none;
  }

  error_e
  store_t::cancel_batch(const std::string &client_uuid, const std::string &batch_id) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (!impl_->owned_batch(client_uuid, batch_id)) {
      return error_e::not_found;
    }
    impl_->drop_batch_locked(batch_id);
    return error_e::none;
  }

  commit_result_t
  store_t::commit(const std::string &client_uuid, const std::string &batch_id, std::string intent, std::optional<double> x, std::optional<double> y) {
    auto &d = *impl_;
    std::lock_guard<std::mutex> lk(d.mu);
    d.sweep_locked();
    auto *batch = d.owned_batch(client_uuid, batch_id);
    if (!batch) {
      return { error_e::not_found, {} };
    }
    auto in_unit = [](const std::optional<double> &v) {
      return !v || (*v >= 0.0 && *v <= 1.0);
    };
    if ((intent != "drop" && intent != "clipboard") || !in_unit(x) || !in_unit(y)) {
      return { error_e::bad_request, {} };
    }
    if (batch->uploads.size() != batch->count) {
      return { error_e::incomplete, {} };
    }
    for (const auto &id : batch->uploads) {
      const auto &u = d.uploads.at(id);
      if (u.offset != u.size || u.writing) {
        return { error_e::incomplete, {} };
      }
    }
    std::unordered_set<std::string> seen;
    for (const auto &id : batch->uploads) {
      const auto &u = d.uploads.at(id);
      if (!seen.insert(fold_key(u.rel.empty() ? u.name : u.rel + "/" + u.name)).second) {
        return { error_e::duplicate_path, {} };
      }
    }

    manifest_t m;
    m.id = d.unique_id();
    if (m.id.empty()) {
      return { error_e::io, {} };
    }
    m.client_uuid = client_uuid;
    m.session_id = batch->session_id;
    m.intent = std::move(intent);
    m.x = x;
    m.y = y;
    for (const auto &id : batch->uploads) {
      auto &u = d.uploads.at(id);
      m.items.push_back({ u.name, u.rel, u.size, u.path });
      m.total_size += u.size;
    }
    for (const auto &id : batch->uploads) {
      d.uploads.erase(id);
    }
    d.batches.erase(batch_id);
    d.manifests.emplace(m.id, impl_t::stored_manifest_t { m, d.now() + d.opt.limits.manifest_ttl });
    return { error_e::none, m };
  }

  std::optional<manifest_t>
  store_t::manifest(const std::string &manifest_id, std::uint32_t session_id) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->sweep_locked();
    auto it = impl_->manifests.find(manifest_id);
    if (it == impl_->manifests.end() || it->second.manifest.session_id != session_id) {
      return std::nullopt;
    }
    // Idle TTL: a large batch stays alive while the agent keeps reading it.
    it->second.expires_at = impl_->now() + impl_->opt.limits.manifest_ttl;
    return it->second.manifest;
  }

  bool
  store_t::release_manifest(const std::string &manifest_id, std::uint32_t session_id) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    auto it = impl_->manifests.find(manifest_id);
    if (it == impl_->manifests.end() || it->second.manifest.session_id != session_id) {
      return false;
    }
    for (const auto &item : it->second.manifest.items) {
      remove_file(item.staged);
    }
    impl_->manifests.erase(it);
    return true;
  }

  void
  store_t::set_while_writing_for_test(std::function<void()> hook) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->opt.while_writing = std::move(hook);
  }

  void
  store_t::sweep() {
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->sweep_locked();
  }

  store_t::stats_t
  store_t::stats() {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return { impl_->batches.size(), impl_->uploads.size(), impl_->manifests.size(), impl_->reserved_for(nullptr) };
  }

  namespace {
    std::mutex g_instance_mu;
    std::unique_ptr<store_t> g_instance;
  }  // namespace

  store_t *
  instance() {
    std::lock_guard<std::mutex> lk(g_instance_mu);
    return g_instance.get();
  }

  void
  init(options_t options) {
    auto store = std::make_unique<store_t>(std::move(options));
    store->reset_staging();
    std::lock_guard<std::mutex> lk(g_instance_mu);
    g_instance = std::move(store);
  }

  void
  shutdown() {
    std::lock_guard<std::mutex> lk(g_instance_mu);
    g_instance.reset();
  }
}  // namespace file_upload_store
