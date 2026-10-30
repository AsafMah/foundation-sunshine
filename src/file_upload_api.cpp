/**
 * @file src/file_upload_api.cpp
 * @brief See file_upload_api.h.
 */
#include "file_upload_api.h"

#include <charconv>
#include <cmath>
#include <regex>

#include <nlohmann/json.hpp>

namespace file_upload_api {
  namespace {
    using json = nlohmann::json;
    using file_upload_store::error_e;

    const std::regex kBatches(R"(^/api/v1/file-transfer/batches$)");
    const std::regex kBatch(R"(^/api/v1/file-transfer/batches/([a-f0-9]{64})$)");
    const std::regex kCommit(R"(^/api/v1/file-transfer/batches/([a-f0-9]{64})/commit$)");
    const std::regex kUploads(R"(^/api/v1/file-transfer/uploads$)");
    const std::regex kUpload(R"(^/api/v1/file-transfer/uploads/([a-f0-9]{64})$)");
    const std::regex kManifest(R"(^/api/v1/file-transfer/manifests/([a-f0-9]{64})$)");
    const std::regex kItem(R"(^/api/v1/file-transfer/manifests/([a-f0-9]{64})/items/([0-9]{1,4})$)");

    reply_t
    json_reply(int status, const json &body) {
      reply_t r;
      r.status = status;
      r.body = body.dump();
      r.headers["Content-Type"] = "application/json";
      r.headers["Cache-Control"] = "no-store";
      return r;
    }

    reply_t
    error_reply(int status, const std::string &error) {
      return json_reply(status, json { { "error", error } });
    }

    reply_t
    no_content() {
      reply_t r;
      r.status = 204;
      r.headers["Cache-Control"] = "no-store";
      return r;
    }

    int
    status_for(error_e err) {
      switch (err) {
        case error_e::none:
          return 200;
        case error_e::bad_request:
        case error_e::bad_name:
          return 400;
        case error_e::not_found:
          return 404;
        case error_e::too_large:
          return 413;
        case error_e::too_many:
          return 429;
        case error_e::quota:
        case error_e::disk_full:
          return 507;
        case error_e::no_session:
        case error_e::ambiguous_session:
        case error_e::offset_mismatch:
        case error_e::incomplete:
        case error_e::duplicate_path:
          return 409;
        case error_e::io:
          return 500;
      }
      return 500;
    }

    reply_t
    store_error(error_e err) {
      return error_reply(status_for(err), file_upload_store::to_string(err));
    }

    std::optional<std::string>
    query_value(std::string_view query, std::string_view key) {
      while (!query.empty()) {
        const auto amp = query.find('&');
        const auto pair = query.substr(0, amp);
        const auto eq = pair.find('=');
        if (eq != std::string_view::npos && pair.substr(0, eq) == key) {
          return std::string(pair.substr(eq + 1));
        }
        if (amp == std::string_view::npos) {
          break;
        }
        query.remove_prefix(amp + 1);
      }
      return std::nullopt;
    }

    template <typename T>
    std::optional<T>
    parse_uint(const std::optional<std::string> &value) {
      if (!value || value->empty()) {
        return std::nullopt;
      }
      T out {};
      const auto [end, ec] = std::from_chars(value->data(), value->data() + value->size(), out);
      if (ec != std::errc {} || end != value->data() + value->size()) {
        return std::nullopt;
      }
      return out;
    }

    // Non-negative integral JSON number.
    std::optional<std::uint64_t>
    json_uint(const json &obj, const char *key) {
      const auto it = obj.find(key);
      if (it == obj.end()) {
        return std::nullopt;
      }
      if (it->is_number_unsigned()) {
        return it->get<std::uint64_t>();
      }
      if (it->is_number_integer()) {
        const auto v = it->get<std::int64_t>();
        return v < 0 ? std::nullopt : std::optional<std::uint64_t>(static_cast<std::uint64_t>(v));
      }
      return std::nullopt;
    }

    std::optional<double>
    json_coord(const json &obj, const char *key, bool &bad) {
      const auto it = obj.find(key);
      if (it == obj.end() || it->is_null()) {
        return std::nullopt;
      }
      if (!it->is_number() || !std::isfinite(it->get<double>())) {
        bad = true;
        return std::nullopt;
      }
      return it->get<double>();
    }

    std::optional<json>
    parse_object(std::string_view body) {
      auto j = json::parse(body, nullptr, false);
      if (j.is_discarded() || !j.is_object()) {
        return std::nullopt;
      }
      return j;
    }

    json
    state_json(const file_upload_store::upload_state_t &st) {
      return json { { "offset", st.offset }, { "size", st.size }, { "complete", st.complete } };
    }
  }  // namespace

  bool
  is_client_path(std::string_view path) {
    const std::string p(path);
    return std::regex_match(p, kBatches) || std::regex_match(p, kBatch) || std::regex_match(p, kCommit) ||
           std::regex_match(p, kUploads) || std::regex_match(p, kUpload);
  }

  bool
  is_agent_path(std::string_view path) {
    const std::string p(path);
    return std::regex_match(p, kManifest) || std::regex_match(p, kItem);
  }

  reply_t
  handle_client(file_upload_store::store_t *store, bool enabled, const std::string &client_uuid, const request_t &req) {
    if (!enabled) {
      return error_reply(403, "file_upload_disabled");
    }
    if (client_uuid.empty()) {
      return error_reply(401, "unauthorized");
    }
    if (!store) {
      return error_reply(503, "unavailable");
    }

    std::smatch m;
    const std::string &path = req.path;

    if (std::regex_match(path, kBatches)) {
      if (req.method != "POST") {
        return error_reply(405, "method_not_allowed");
      }
      const auto body = parse_object(req.body);
      const auto count = body ? json_uint(*body, "count") : std::nullopt;
      const auto total = body ? json_uint(*body, "total_size") : std::nullopt;
      if (!count || !total || *count > 0xFFFFFFFFull) {
        return error_reply(400, "bad_request");
      }
      const auto out = store->open_batch(client_uuid, static_cast<std::uint32_t>(*count), *total);
      if (out.err != error_e::none) {
        return store_error(out.err);
      }
      return json_reply(200, json { { "batch", out.batch_id } });
    }

    if (std::regex_match(path, m, kBatch)) {
      if (req.method != "DELETE") {
        return error_reply(405, "method_not_allowed");
      }
      const auto err = store->cancel_batch(client_uuid, m[1].str());
      return err == error_e::none ? no_content() : store_error(err);
    }

    if (std::regex_match(path, m, kCommit)) {
      if (req.method != "POST") {
        return error_reply(405, "method_not_allowed");
      }
      const auto body = parse_object(req.body);
      if (!body || !body->contains("intent") || !(*body)["intent"].is_string()) {
        return error_reply(400, "bad_request");
      }
      bool bad = false;
      const auto x = json_coord(*body, "x", bad);
      const auto y = json_coord(*body, "y", bad);
      if (bad) {
        return error_reply(400, "bad_request");
      }
      auto out = store->commit(client_uuid, m[1].str(), (*body)["intent"].get<std::string>(), x, y);
      if (out.err != error_e::none) {
        return store_error(out.err);
      }
      return json_reply(200, json { { "manifest", out.manifest.id }, { "count", out.manifest.items.size() }, { "total_size", out.manifest.total_size } });
    }

    if (std::regex_match(path, kUploads)) {
      if (req.method != "POST") {
        return error_reply(405, "method_not_allowed");
      }
      const auto body = parse_object(req.body);
      if (!body) {
        return error_reply(400, "bad_request");
      }
      const auto size = json_uint(*body, "size");
      const auto batch = body->value("batch", json {});
      const auto name = body->value("name", json {});
      const auto rel = body->value("rel", json(""));
      if (!size || !batch.is_string() || !name.is_string() || !rel.is_string()) {
        return error_reply(400, "bad_request");
      }
      const auto out = store->create_upload(client_uuid, batch.get<std::string>(), name.get<std::string>(), rel.get<std::string>(), *size);
      if (out.err != error_e::none) {
        return store_error(out.err);
      }
      return json_reply(200, json { { "id", out.upload_id }, { "chunk_size", out.chunk_size }, { "offset", 0 } });
    }

    if (std::regex_match(path, m, kUpload)) {
      const auto id = m[1].str();
      if (req.method == "GET") {
        const auto st = store->state(client_uuid, id);
        return st.err == error_e::none ? json_reply(200, state_json(st)) : store_error(st.err);
      }
      if (req.method == "DELETE") {
        const auto err = store->cancel_upload(client_uuid, id);
        return err == error_e::none ? no_content() : store_error(err);
      }
      if (req.method != "PUT") {
        return error_reply(405, "method_not_allowed");
      }
      if (!req.content_length) {
        return error_reply(411, "length_required");
      }
      if (*req.content_length != req.body.size()) {
        return error_reply(400, "bad_content_length");
      }
      const auto offset = parse_uint<std::uint64_t>(query_value(req.query, "offset"));
      if (!offset) {
        return error_reply(400, "bad_offset");
      }
      const auto st = store->append(client_uuid, id, *offset, req.body);
      if (st.err == error_e::offset_mismatch) {
        auto out = state_json(st);
        out["error"] = file_upload_store::to_string(st.err);
        return json_reply(409, out);
      }
      return st.err == error_e::none ? json_reply(200, state_json(st)) : store_error(st.err);
    }

    return error_reply(404, "not_found");
  }

  reply_t
  handle_agent(file_upload_store::store_t *store, bool enabled, const request_t &req) {
    if (!enabled) {
      return error_reply(403, "file_upload_disabled");
    }
    if (!store) {
      return error_reply(503, "unavailable");
    }
    const auto sid = parse_uint<std::uint32_t>(query_value(req.query, "sid"));
    if (!sid) {
      return error_reply(400, "missing_sid");
    }

    std::smatch m;
    const std::string &path = req.path;
    if (std::regex_match(path, m, kManifest)) {
      if (req.method == "DELETE") {
        return store->release_manifest(m[1].str(), *sid) ? no_content() : error_reply(404, "not_found");
      }
      if (req.method != "GET") {
        return error_reply(405, "method_not_allowed");
      }
      const auto manifest = store->manifest(m[1].str(), *sid);
      if (!manifest) {
        return error_reply(404, "not_found");
      }
      json items = json::array();
      for (std::size_t i = 0; i < manifest->items.size(); ++i) {
        const auto &item = manifest->items[i];
        items.push_back(json { { "index", i }, { "name", item.name }, { "rel", item.rel }, { "size", item.size } });
      }
      json out { { "id", manifest->id }, { "intent", manifest->intent }, { "total_size", manifest->total_size }, { "items", items } };
      out["x"] = manifest->x ? json(*manifest->x) : json(nullptr);
      out["y"] = manifest->y ? json(*manifest->y) : json(nullptr);
      return json_reply(200, out);
    }

    if (std::regex_match(path, m, kItem)) {
      if (req.method != "GET") {
        return error_reply(405, "method_not_allowed");
      }
      const auto manifest = store->manifest(m[1].str(), *sid);
      const auto index = parse_uint<std::size_t>(m[2].str());
      if (!manifest || !index || *index >= manifest->items.size()) {
        return error_reply(404, "not_found");
      }
      const auto &item = manifest->items[*index];
      reply_t r;
      r.status = 200;
      r.file = item.staged;
      r.headers["Content-Type"] = "application/octet-stream";
      r.headers["Content-Length"] = std::to_string(item.size);
      r.headers["Cache-Control"] = "no-store";
      r.headers["X-Content-Type-Options"] = "nosniff";
      return r;
    }

    return error_reply(404, "not_found");
  }
}  // namespace file_upload_api
