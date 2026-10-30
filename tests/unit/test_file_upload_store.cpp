#include "src/file_upload_store.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include <gtest/gtest.h>

using namespace file_upload_store;

namespace {
  struct fixture_t {
    std::filesystem::path dir;
    steady_t::time_point now = steady_t::now();
    std::vector<std::uint32_t> sessions { 7 };
    std::uint64_t free_bytes = 1ULL << 40;
    std::unique_ptr<store_t> store;

    explicit fixture_t(limits_t limits = {}) {
      const auto stamp = steady_t::now().time_since_epoch().count();
      dir = std::filesystem::temp_directory_path() / ("sunshine-upload-test-" + std::to_string(stamp));
      options_t opt;
      opt.staging_dir = dir;
      opt.limits = limits;
      opt.sessions = [this](const std::string &uuid) {
        return uuid == "client-a" || uuid == "client-b" ? sessions : std::vector<std::uint32_t> {};
      };
      opt.free_space = [this](const std::filesystem::path &) { return free_bytes; };
      opt.now = [this]() { return now; };
      store = std::make_unique<store_t>(opt);
      EXPECT_TRUE(store->reset_staging());
    }

    ~fixture_t() {
      store.reset();
      std::error_code ec;
      std::filesystem::remove_all(dir, ec);
    }

    std::string
    batch(std::uint32_t count = 1, std::uint64_t total = 5, const std::string &uuid = "client-a") {
      auto b = store->open_batch(uuid, count, total);
      EXPECT_EQ(b.err, error_e::none);
      return b.batch_id;
    }

    std::string
    upload(const std::string &batch_id, const std::string &name, std::uint64_t size, const std::string &rel = "", const std::string &uuid = "client-a") {
      auto u = store->create_upload(uuid, batch_id, name, rel, size);
      EXPECT_EQ(u.err, error_e::none) << to_string(u.err);
      return u.upload_id;
    }
  };

  std::string
  read_file(const std::filesystem::path &path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
  }
}  // namespace

TEST(FileUploadNames, SanitizesNames) {
  EXPECT_EQ(sanitize_name("save.sav"), "save.sav");
  EXPECT_EQ(sanitize_name("..\\..\\evil.exe"), "evil.exe");
  EXPECT_EQ(sanitize_name("C:/Windows/x.dll"), "x.dll");
  EXPECT_EQ(sanitize_name("a:b*c?.txt"), "a_b_c_.txt");
  EXPECT_EQ(sanitize_name("CON"), "_CON");
  EXPECT_EQ(sanitize_name("con.txt"), "_con.txt");
  EXPECT_EQ(sanitize_name("COM\xC2\xB9.bin"), "_COM\xC2\xB9.bin");
  EXPECT_EQ(sanitize_name("trail. . "), "trail");
  EXPECT_EQ(sanitize_name("invoice\xE2\x80\xAEtxt.exe"), "invoice_txt.exe");
  EXPECT_EQ(sanitize_name("\xE5\xAD\x98\xE6\xA1\xA3.sav"), "\xE5\xAD\x98\xE6\xA1\xA3.sav");
  EXPECT_EQ(sanitize_name(""), "");
  EXPECT_EQ(sanitize_name(".."), "");
  EXPECT_EQ(sanitize_name("bad\xFF"), "");
  EXPECT_LE(sanitize_name(std::string(400, 'a') + ".txt").size(), 180u);
}

TEST(FileUploadNames, ValidatesRelativeDirectories) {
  EXPECT_EQ(validate_rel(""), std::optional<std::string> { "" });
  EXPECT_EQ(validate_rel("Saves/slot1"), std::optional<std::string> { "Saves/slot1" });
  EXPECT_EQ(validate_rel("Saves\\slot1"), std::optional<std::string> { "Saves/slot1" });
  for (const char *bad : { "..", "a/../b", "./a", "/abs", "\\\\server\\share", "C:/x", "C:x", "\\\\?\\C:\\x",
         "a//b", "a/b.", "a/b ", "a/CON", "a/nul.txt", "a/LPT\xC2\xB3", "a:stream", "a/\x01",
         "a/\xE2\x80\xAE", "a/b/c/d/e/f/g/h/i" }) {
    EXPECT_FALSE(validate_rel(bad).has_value()) << bad;
  }
  EXPECT_FALSE(validate_rel(std::string(241, 'a')).has_value());
}

TEST(FileUploadStore, RoundTripCommitAndRelease) {
  fixture_t f;
  const auto b = f.batch(2, 8);
  const auto u1 = f.upload(b, "a.txt", 5, "Saves");
  const auto u2 = f.upload(b, "empty.txt", 0);

  auto st = f.store->append("client-a", u1, 0, "hel");
  EXPECT_EQ(st.err, error_e::none);
  EXPECT_EQ(st.offset, 3u);
  st = f.store->append("client-a", u1, 3, "lo");
  EXPECT_TRUE(st.complete);
  EXPECT_TRUE(f.store->state("client-a", u2).complete);

  auto c = f.store->commit("client-a", b, "drop", 0.25, 0.5);
  ASSERT_EQ(c.err, error_e::none) << to_string(c.err);
  EXPECT_EQ(c.manifest.id.size(), 64u);
  EXPECT_EQ(c.manifest.session_id, 7u);
  EXPECT_EQ(c.manifest.total_size, 5u);
  ASSERT_EQ(c.manifest.items.size(), 2u);
  EXPECT_EQ(c.manifest.items[0].rel, "Saves");
  EXPECT_EQ(read_file(c.manifest.items[0].staged), "hello");

  EXPECT_FALSE(f.store->manifest(c.manifest.id, 8).has_value());
  auto m = f.store->manifest(c.manifest.id, 7);
  ASSERT_TRUE(m.has_value());
  EXPECT_EQ(m->x, std::optional<double> { 0.25 });

  EXPECT_FALSE(f.store->release_manifest(c.manifest.id, 8));
  EXPECT_TRUE(f.store->release_manifest(c.manifest.id, 7));
  EXPECT_FALSE(std::filesystem::exists(c.manifest.items[0].staged));
  EXPECT_FALSE(f.store->manifest(c.manifest.id, 7).has_value());
  EXPECT_EQ(f.store->stats().reserved_bytes, 0u);
}

TEST(FileUploadStore, OffsetsAreStrictAndFinalRetryIsIdempotent) {
  fixture_t f;
  const auto u = f.upload(f.batch(), "a", 5);
  EXPECT_EQ(f.store->append("client-a", u, 1, "x").err, error_e::offset_mismatch);
  EXPECT_EQ(f.store->append("client-a", u, 0, "hello!").err, error_e::too_large);
  EXPECT_EQ(f.store->state("client-a", u).err, error_e::not_found) << "overflow aborts the upload";

  const auto u2 = f.upload(f.batch(), "b", 5);
  EXPECT_EQ(f.store->append("client-a", u2, 0, "hello").offset, 5u);
  auto again = f.store->append("client-a", u2, 0, "hello");
  EXPECT_EQ(again.err, error_e::none);
  EXPECT_TRUE(again.complete);
  EXPECT_EQ(again.offset, 5u);
  EXPECT_EQ(f.store->append("client-a", u2, 5, "x").err, error_e::too_large);
}

TEST(FileUploadStore, ChunkLimit) {
  limits_t l;
  l.max_chunk_bytes = 4;
  fixture_t f(l);
  const auto u = f.upload(f.batch(), "a", 5);
  EXPECT_EQ(f.store->append("client-a", u, 0, "hello").err, error_e::too_large);
  EXPECT_EQ(f.store->append("client-a", u, 0, "hell").offset, 4u);
}

TEST(FileUploadStore, BoundToClientAndSession) {
  fixture_t f;
  const auto b = f.batch();
  EXPECT_EQ(f.store->create_upload("client-b", b, "a", "", 5).err, error_e::not_found);
  const auto u = f.upload(b, "a", 5);
  EXPECT_EQ(f.store->append("client-b", u, 0, "hello").err, error_e::not_found);
  EXPECT_EQ(f.store->state("client-b", u).err, error_e::not_found);
  EXPECT_EQ(f.store->cancel_upload("client-b", u), error_e::not_found);
  EXPECT_EQ(f.store->commit("client-b", b, "drop", {}, {}).err, error_e::not_found);

  f.sessions = {};
  EXPECT_EQ(f.store->open_batch("client-a", 1, 1).err, error_e::no_session);
  f.sessions = { 7, 9 };
  EXPECT_EQ(f.store->open_batch("client-a", 1, 1).err, error_e::ambiguous_session);
  EXPECT_EQ(f.store->open_batch("unknown", 1, 1).err, error_e::no_session);
}

TEST(FileUploadStore, CommitRequiresCompleteUniqueFiles) {
  fixture_t f;
  const auto b = f.batch(2, 10);
  const auto u1 = f.upload(b, "Save.txt", 5);
  f.upload(b, "save.TXT", 5);
  f.store->append("client-a", u1, 0, "hello");
  EXPECT_EQ(f.store->commit("client-a", b, "drop", {}, {}).err, error_e::incomplete);

  fixture_t g;
  const auto b2 = g.batch(2, 2);
  const auto a = g.upload(b2, "Save.txt", 1);
  const auto c = g.upload(b2, "save.TXT", 1);
  g.store->append("client-a", a, 0, "x");
  g.store->append("client-a", c, 0, "y");
  EXPECT_EQ(g.store->commit("client-a", b2, "drop", {}, {}).err, error_e::duplicate_path);

  fixture_t h;
  const auto b3 = h.batch(1, 0);
  EXPECT_EQ(h.store->commit("client-a", b3, "drop", {}, {}).err, error_e::incomplete) << "declared file never created";
  h.upload(b3, "z", 0);
  EXPECT_EQ(h.store->commit("client-a", b3, "drop", 2.0, {}).err, error_e::bad_request);
  EXPECT_EQ(h.store->commit("client-a", b3, "explode", {}, {}).err, error_e::bad_request);
  EXPECT_EQ(h.store->commit("client-a", b3, "clipboard", {}, {}).err, error_e::none);
  EXPECT_EQ(h.store->commit("client-a", b3, "clipboard", {}, {}).err, error_e::not_found) << "single commit";
}

TEST(FileUploadStore, BatchDeclarationIsEnforced) {
  fixture_t f;
  EXPECT_EQ(f.store->open_batch("client-a", 0, 0).err, error_e::bad_request);
  EXPECT_EQ(f.store->open_batch("client-a", 1001, 1).err, error_e::too_many);
  EXPECT_EQ(f.store->open_batch("client-a", 1, 17ULL << 30).err, error_e::too_large);
  const auto b = f.batch(1, 5);
  f.upload(b, "a", 2);
  EXPECT_EQ(f.store->create_upload("client-a", b, "b", "", 1).err, error_e::too_many);
  const auto b2 = f.batch(2, 5);
  f.upload(b2, "a", 4);
  EXPECT_EQ(f.store->create_upload("client-a", b2, "b", "", 2).err, error_e::too_large) << "exceeds declared total";
  EXPECT_EQ(f.store->create_upload("client-a", b2, "b", "", 5ULL << 30).err, error_e::too_large);
  EXPECT_EQ(f.store->create_upload("client-a", b2, "..", "", 1).err, error_e::bad_name);
  EXPECT_EQ(f.store->create_upload("client-a", b2, "b", "../x", 1).err, error_e::bad_name);
}

TEST(FileUploadStore, QuotasReserveDeclaredSize) {
  limits_t l;
  l.max_client_bytes = 10;
  l.max_total_bytes = 15;
  l.max_client_open_batches = 2;
  l.min_free_bytes = 0;
  fixture_t f(l);
  const auto b = f.batch(2, 20);
  f.upload(b, "a", 8);
  EXPECT_EQ(f.store->create_upload("client-a", b, "b", "", 3).err, error_e::quota) << "per-client";
  f.batch(1, 1);
  EXPECT_EQ(f.store->open_batch("client-a", 1, 1).err, error_e::too_many) << "open batches";

  const auto bb = f.batch(1, 10, "client-b");
  EXPECT_EQ(f.store->create_upload("client-b", bb, "c", "", 8).err, error_e::quota) << "global";
  f.upload(bb, "c", 7, "", "client-b");

  EXPECT_EQ(f.store->cancel_batch("client-a", b), error_e::none);
  EXPECT_EQ(f.store->stats().reserved_bytes, 7u);
}

TEST(FileUploadStore, DiskHeadroom) {
  limits_t l;
  l.min_free_bytes = 100;
  fixture_t f(l);
  f.free_bytes = 150;
  const auto b = f.batch(2, 100);
  f.upload(b, "a", 40);
  EXPECT_EQ(f.store->create_upload("client-a", b, "b", "", 20).err, error_e::disk_full);
}

TEST(FileUploadStore, ExpiryAndCancelDeleteStagedBytes) {
  fixture_t f;
  const auto b = f.batch(1, 5);
  const auto u = f.upload(b, "a", 5);
  f.store->append("client-a", u, 0, "he");
  EXPECT_EQ(f.store->cancel_upload("client-a", u), error_e::none);
  EXPECT_EQ(f.store->state("client-a", u).err, error_e::not_found);

  const auto b2 = f.batch(1, 5);
  const auto u2 = f.upload(b2, "a", 5);
  f.store->append("client-a", u2, 0, "he");
  f.now += std::chrono::minutes(30);
  f.store->append("client-a", u2, 2, "l");
  f.now += std::chrono::minutes(59);
  f.store->sweep();
  EXPECT_EQ(f.store->state("client-a", u2).offset, 3u) << "activity refreshes TTL";
  f.now += std::chrono::minutes(61);
  f.store->sweep();
  EXPECT_EQ(f.store->state("client-a", u2).err, error_e::not_found);
  EXPECT_EQ(f.store->stats().reserved_bytes, 0u);
  std::size_t files = 0;
  for (auto &e : std::filesystem::recursive_directory_iterator(f.dir)) {
    files += e.is_regular_file();
  }
  EXPECT_EQ(files, 0u);

  const auto b3 = f.batch(1, 0);
  f.upload(b3, "z", 0);
  auto c = f.store->commit("client-a", b3, "drop", {}, {});
  f.now += std::chrono::minutes(11);
  f.store->sweep();
  EXPECT_FALSE(f.store->manifest(c.manifest.id, 7).has_value()) << "manifest TTL";
}

TEST(FileUploadStore, ResetStagingWipesLeftovers) {
  fixture_t f;
  const auto u = f.upload(f.batch(), "a", 5);
  f.store->append("client-a", u, 0, "he");
  ASSERT_TRUE(f.store->reset_staging());
  EXPECT_EQ(f.store->state("client-a", u).err, error_e::not_found);
  EXPECT_TRUE(std::filesystem::is_empty(f.dir));
}

TEST(FileUploadStore, CancelDuringWriteLeavesNoUntrackedFile) {
  fixture_t f;
  const auto b = f.batch(1, 5);
  const auto u = f.upload(b, "a", 5);
  bool cancelled = false;
  f.store->set_while_writing_for_test([&] {
    cancelled = f.store->cancel_upload("client-a", u) == error_e::none;
  });
  EXPECT_EQ(f.store->append("client-a", u, 0, "he").err, error_e::not_found);
  f.store->set_while_writing_for_test({});
  EXPECT_TRUE(cancelled);
  EXPECT_EQ(f.store->stats().reserved_bytes, 0u);
  std::size_t files = 0;
  for (auto &e : std::filesystem::recursive_directory_iterator(f.dir)) {
    files += e.is_regular_file();
  }
  EXPECT_EQ(files, 0u) << "staged bytes must not outlive their accounting";
}

TEST(FileUploadStore, ManifestTtlIsIdleWhileTheAgentReads) {
  fixture_t f;
  const auto b = f.batch(1, 0);
  f.upload(b, "z", 0);
  const auto c = f.store->commit("client-a", b, "drop", {}, {});
  f.now += std::chrono::minutes(9);
  ASSERT_TRUE(f.store->manifest(c.manifest.id, 7).has_value());
  f.now += std::chrono::minutes(9);
  f.store->sweep();
  EXPECT_TRUE(f.store->manifest(c.manifest.id, 7).has_value()) << "reads refresh the TTL";
  f.now += std::chrono::minutes(11);
  f.store->sweep();
  EXPECT_FALSE(f.store->manifest(c.manifest.id, 7).has_value());
}