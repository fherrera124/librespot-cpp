#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "HTTPClient.h"  // for HTTPClient::Response
#include "TrackReference.h"

typedef struct cJSON cJSON;

namespace cspot {
struct Context;
class Login5Client;

// Resolves a connect-state context URI (spotify:playlist:..., spotify:album:
// ...) into a flat track list via spclient's context-resolve endpoint - the
// one capability SPIRC's Load frame never needed cspot to have, since
// Spotify resolved contexts server-side there. See docs/dealer_websocket_
// migration.md, Fase 6 "corte completo".
//
// Context/ContextPage/ContextTrack are protobuf's canonical JSON mapping
// (bytes fields as base64 strings) - cJSON parsing end to end, no nanopb
// schema, same as DealerSession's envelope.
class ContextResolver {
 public:
  ContextResolver(std::shared_ptr<cspot::Context> ctx,
                  std::shared_ptr<cspot::Login5Client> login5);

  // Fetches the full context and follows every page's next_page_url until
  // exhausted, flattening the result into `tracksOut` - TrackQueue expects
  // one complete list up front, same as SPIRC's old Load frame did.
  // @returns false if the context couldn't be resolved at all
  bool resolve(const std::string& contextUri,
              std::vector<TrackReference>& tracksOut);

  // Pre-seeds the cached spclient host so the first resolve() of the
  // session skips its own apresolve round trip - PlayerEngine
  // already resolved the same host for its registration PUT.
  void seedSpclientHost(const std::string& host);

  // Parses one ContextTrack (canonical protobuf JSON: uri, base64 gid) into
  // a TrackReference. Shared by resolve()'s page parsing and
  // PlayerEngine's set_queue/add_to_queue handlers, which receive
  // the exact same ContextTrack shape inline on the command object.
  // @returns false if the track has no usable gid/uri
  static bool trackFromJson(cJSON* trackItem, TrackReference& out);

 private:
  // path is spclient-relative (the "hm://" prefix real responses use for
  // page_url/next_page_url is stripped by the caller - it's not an actual
  // Mercury URI, just a path-flavored string).
  bool fetchJson(const std::string& path, std::string& jsonOut);

  // fetchJson() + cJSON_Parse(), with a bounded retry (see HttpRetry.h).
  // Throws on failure (already logged) - caller owns the returned cJSON*.
  cJSON* fetchAndParse(const std::string& path);

  // Reads `firstPage`'s tracks (fetching page_url first if they aren't
  // inline), then follows next_page_url as many times as needed.
  bool followPageChain(cJSON* firstPage, std::vector<TrackReference>& tracksOut);

  std::shared_ptr<cspot::Context> ctx;
  std::shared_ptr<cspot::Login5Client> login5;

  // Cached across fetchJson() calls, cleared on transport failure. hostMutex
  // only guards spclientHost: seedSpclientHost() can run on
  // PlayerEngine's task while fetchJson() runs on DealerSession's.
  // `connection` stays single-task (only fetchJson() touches it), so it
  // needs no lock of its own.
  std::mutex hostMutex;
  std::string spclientHost;
  std::unique_ptr<bell::HTTPClient::Response> connection;
};
}  // namespace cspot
