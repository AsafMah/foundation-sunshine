/**
 * @file src/file_upload_api.h
 * @brief HTTP request handling for client-to-host uploads, independent of the web server.
 *
 * Paired clients (nvhttp, mTLS; identity = certificate client UUID):
 *   POST   /api/v1/file-transfer/batches                {count,total_size}       -> {batch}
 *   DELETE /api/v1/file-transfer/batches/<id>
 *   POST   /api/v1/file-transfer/batches/<id>/commit    {intent,x?,y?}           -> {manifest,count,total_size}
 *   POST   /api/v1/file-transfer/uploads                {batch,name,rel?,size}   -> {id,chunk_size,offset}
 *   PUT    /api/v1/file-transfer/uploads/<id>?offset=N  raw bytes                -> {offset,size,complete}
 *   GET    /api/v1/file-transfer/uploads/<id>                                    -> {offset,size,complete}
 *   DELETE /api/v1/file-transfer/uploads/<id>
 *
 * Local GUI agent (confighttp, loopback):
 *   GET    /api/v1/file-transfer/manifests/<id>?sid=N            -> {intent,x,y,total_size,items[]}
 *   GET    /api/v1/file-transfer/manifests/<id>/items/<n>?sid=N  -> file bytes
 *   DELETE /api/v1/file-transfer/manifests/<id>?sid=N
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "file_upload_store.h"

namespace file_upload_api {
  struct request_t {
    std::string method;
    std::string path;
    std::string query;
    std::optional<std::uint64_t> content_length;
    std::string_view body;
  };

  struct reply_t {
    int status = 200;
    std::string body;
    std::map<std::string, std::string> headers;
    // When set, the body is this file's contents (streamed by the route adapter).
    std::filesystem::path file;
  };

  /// True when the path belongs to the client upload API (so the adapter can route it).
  bool
  is_client_path(std::string_view path);
  bool
  is_agent_path(std::string_view path);

  reply_t
  handle_client(file_upload_store::store_t *store, bool enabled, const std::string &client_uuid, const request_t &req);
  reply_t
  handle_agent(file_upload_store::store_t *store, bool enabled, const request_t &req);
}  // namespace file_upload_api
