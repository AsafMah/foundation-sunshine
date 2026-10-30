#include "src/file_upload_api.h"

#include <filesystem>
#include <fstream>
#include <iterator>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

using namespace file_upload_api;
using json = nlohmann::json;

namespace {
  struct api_fixture_t {
    std::filesystem::path dir;
    std::vector<std::uint32_t> sessions { 11 };
    std::unique_ptr<file_upload_store::store_t> store;

    api_fixture_t() {
      dir = std::filesystem::temp_directory_path() /
            ("sunshine-upload-api-" + std::to_string(file_upload_store::steady_t::now().time_since_epoch().count()));
      file_upload_store::options_t opt;
      opt.staging_dir = dir;
      opt.sessions = [this](const std::string &uuid) { return uuid == "c1" || uuid == "c2" ? sessions : std::vector<std::uint32_t> {}; };
      opt.free_space = [](const std::filesystem::path &) { return 1ULL << 40; };
      store = std::make_unique<file_upload_store::store_t>(opt);
      store->reset_staging();
    }

    ~api_fixture_t() {
      store.reset();
      std::error_code ec;
      std::filesystem::remove_all(dir, ec);
    }

    reply_t
    client(const std::string &method, const std::string &path, std::string_view body = {}, const std::string &query = {}, const std::string &uuid = "c1", bool enabled = true) {
      request_t r { method, path, query, body.size(), body };
      return handle_client(store.get(), enabled, uuid, r);
    }

    reply_t
    agent(const std::string &method, const std::string &path, const std::string &query) {
      return handle_agent(store.get(), true, { method, path, query, std::nullopt, {} });
    }
  };

  json
  body_of(const reply_t &r) {
    return json::parse(r.body, nullptr, false);
  }
}  // namespace

TEST(FileUploadApi, Routing) {
  EXPECT_TRUE(is_client_path("/api/v1/file-transfer/batches"));
  EXPECT_TRUE(is_client_path("/api/v1/file-transfer/uploads/" + std::string(64, 'a')));
  EXPECT_FALSE(is_client_path("/api/v1/file-transfer/" + std::string(64, 'a')));
  EXPECT_FALSE(is_client_path("/api/v1/file-transfer/manifests/" + std::string(64, 'a')));
  EXPECT_TRUE(is_agent_path("/api/v1/file-transfer/manifests/" + std::string(64, 'a') + "/items/3"));
  EXPECT_FALSE(is_agent_path("/api/v1/file-transfer/uploads"));
}

TEST(FileUploadApi, DisabledAndUnauthenticated) {
  api_fixture_t f;
  auto r = f.client("POST", "/api/v1/file-transfer/batches", R"({"count":1,"total_size":1})", {}, "c1", false);
  EXPECT_EQ(r.status, 403);
  EXPECT_EQ(body_of(r)["error"], "file_upload_disabled");
  r = f.client("POST", "/api/v1/file-transfer/batches", R"({"count":1,"total_size":1})", {}, "");
  EXPECT_EQ(r.status, 401);
  r = handle_client(nullptr, true, "c1", { "POST", "/api/v1/file-transfer/batches", {}, 2, "{}" });
  EXPECT_EQ(r.status, 503);
}

TEST(FileUploadApi, FullFlow) {
  api_fixture_t f;
  auto r = f.client("POST", "/api/v1/file-transfer/batches", R"({"count":1,"total_size":5})");
  ASSERT_EQ(r.status, 200) << r.body;
  const std::string batch = body_of(r)["batch"];

  r = f.client("POST", "/api/v1/file-transfer/uploads", json { { "batch", batch }, { "name", "save.sav" }, { "rel", "Saves" }, { "size", 5 } }.dump());
  ASSERT_EQ(r.status, 200) << r.body;
  const std::string id = body_of(r)["id"];
  EXPECT_EQ(body_of(r)["chunk_size"], 4 * 1024 * 1024);

  r = f.client("PUT", "/api/v1/file-transfer/uploads/" + id, "hel", "offset=1");
  EXPECT_EQ(r.status, 409);
  EXPECT_EQ(body_of(r)["offset"], 0);

  request_t no_length { "PUT", "/api/v1/file-transfer/uploads/" + id, "offset=0", std::nullopt, "hel" };
  EXPECT_EQ(handle_client(f.store.get(), true, "c1", no_length).status, 411);
  request_t wrong_length { "PUT", "/api/v1/file-transfer/uploads/" + id, "offset=0", 9, "hel" };
  EXPECT_EQ(handle_client(f.store.get(), true, "c1", wrong_length).status, 400);

  r = f.client("PUT", "/api/v1/file-transfer/uploads/" + id, "hel", "offset=0");
  ASSERT_EQ(r.status, 200) << r.body;
  r = f.client("GET", "/api/v1/file-transfer/uploads/" + id);
  EXPECT_EQ(body_of(r)["offset"], 3);
  EXPECT_EQ(f.client("GET", "/api/v1/file-transfer/uploads/" + id, {}, {}, "c2").status, 404);
  r = f.client("PUT", "/api/v1/file-transfer/uploads/" + id, "lo", "offset=3");
  EXPECT_EQ(body_of(r)["complete"], true);

  r = f.client("POST", "/api/v1/file-transfer/batches/" + batch + "/commit", R"({"intent":"drop","x":0.5,"y":0.25})");
  ASSERT_EQ(r.status, 200) << r.body;
  const std::string manifest = body_of(r)["manifest"];
  EXPECT_EQ(body_of(r)["count"], 1);
  EXPECT_EQ(body_of(r)["total_size"], 5);

  EXPECT_EQ(f.agent("GET", "/api/v1/file-transfer/manifests/" + manifest, "sid=12").status, 404);
  EXPECT_EQ(f.agent("GET", "/api/v1/file-transfer/manifests/" + manifest, "").status, 400);
  r = f.agent("GET", "/api/v1/file-transfer/manifests/" + manifest, "sid=11");
  ASSERT_EQ(r.status, 200) << r.body;
  const auto m = body_of(r);
  EXPECT_EQ(m["intent"], "drop");
  EXPECT_EQ(m["x"], 0.5);
  ASSERT_EQ(m["items"].size(), 1u);
  EXPECT_EQ(m["items"][0]["name"], "save.sav");
  EXPECT_EQ(m["items"][0]["rel"], "Saves");
  EXPECT_EQ(m["items"][0]["size"], 5);

  r = f.agent("GET", "/api/v1/file-transfer/manifests/" + manifest + "/items/0", "sid=11");
  ASSERT_EQ(r.status, 200);
  std::ifstream in(r.file, std::ios::binary);
  EXPECT_EQ(std::string(std::istreambuf_iterator<char>(in), {}), "hello");
  EXPECT_EQ(r.headers["Content-Length"], "5");
  EXPECT_EQ(f.agent("GET", "/api/v1/file-transfer/manifests/" + manifest + "/items/1", "sid=11").status, 404);

  EXPECT_EQ(f.agent("DELETE", "/api/v1/file-transfer/manifests/" + manifest, "sid=11").status, 204);
  EXPECT_EQ(f.agent("GET", "/api/v1/file-transfer/manifests/" + manifest, "sid=11").status, 404);
}

TEST(FileUploadApi, ErrorMapping) {
  api_fixture_t f;
  EXPECT_EQ(f.client("POST", "/api/v1/file-transfer/batches", "not json").status, 400);
  EXPECT_EQ(f.client("POST", "/api/v1/file-transfer/batches", R"({"count":-1,"total_size":1})").status, 400);
  EXPECT_EQ(f.client("POST", "/api/v1/file-transfer/batches", R"({"count":1.5,"total_size":1})").status, 400);
  EXPECT_EQ(f.client("POST", "/api/v1/file-transfer/batches", R"({"count":1,"total_size":17179869185})").status, 413);
  f.sessions = { 1, 2 };
  auto r = f.client("POST", "/api/v1/file-transfer/batches", R"({"count":1,"total_size":1})");
  EXPECT_EQ(r.status, 409);
  EXPECT_EQ(body_of(r)["error"], "ambiguous_session");
  f.sessions = { 11 };
  r = f.client("POST", "/api/v1/file-transfer/batches", R"({"count":1,"total_size":1})");
  const std::string batch = body_of(r)["batch"];
  r = f.client("POST", "/api/v1/file-transfer/uploads", json { { "batch", batch }, { "name", ".." }, { "size", 1 } }.dump());
  EXPECT_EQ(r.status, 400);
  EXPECT_EQ(body_of(r)["error"], "bad_name");
  EXPECT_EQ(f.client("POST", "/api/v1/file-transfer/batches/" + batch + "/commit", R"({"intent":"drop"})").status, 409);
  EXPECT_EQ(f.client("DELETE", "/api/v1/file-transfer/batches/" + batch).status, 204);
  EXPECT_EQ(f.client("PATCH", "/api/v1/file-transfer/batches").status, 405);
}
