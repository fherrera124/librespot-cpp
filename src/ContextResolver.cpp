#include "ContextResolver.h"

#include <exception>

#include "ApResolve.h"
#include "BellLogger.h"    // for AbstractLogger
#include "CSpotContext.h"  // for Context
#include "Crypto.h"        // for Crypto::base64Decode
#include "HTTPClient.h"
#include "HttpRetry.h"  // for HttpRetry
#include "Login5Client.h"
#include "Logger.h"  // for CSPOT_LOG

// The context-resolve response is protobuf's canonical JSON mapping, not
// nanopb-decodable.
#include "cJSON.h"

using namespace cspot;

namespace {
std::string stripHmPrefix(const std::string& url) {
  if (url.rfind("hm://", 0) == 0) {
    return url.substr(5);
  }
  return url;
}

void appendTracks(cJSON* tracksArray, std::vector<TrackReference>& tracksOut) {
  cJSON* trackItem = nullptr;
  cJSON_ArrayForEach(trackItem, tracksArray) {
    TrackReference ref;
    if (ContextResolver::trackFromJson(trackItem, ref)) {
      tracksOut.push_back(std::move(ref));
    } else {
      CSPOT_LOG(error, "context-resolve: track with no usable gid/uri, skipped");
    }
  }
}
}  // namespace

ContextResolver::ContextResolver(std::shared_ptr<cspot::Context> ctx,
                                 std::shared_ptr<cspot::Login5Client> login5)
    : ctx(ctx), login5(login5) {}

void ContextResolver::seedSpclientHost(const std::string& host) {
  std::lock_guard<std::mutex> lock(hostMutex);
  if (spclientHost.empty()) {
    spclientHost = host;
  }
}

bool ContextResolver::trackFromJson(cJSON* trackItem, TrackReference& out) {
  cJSON* uriItem = cJSON_GetObjectItem(trackItem, "uri");
  if (uriItem != nullptr && uriItem->valuestring != nullptr) {
    out.uri = uriItem->valuestring;
  }

  // Per-context-instance id - see TrackReference::uid's own comment on why
  // a remote "play" command's skip_to needs this, not uri/gid.
  cJSON* uidItem = cJSON_GetObjectItem(trackItem, "uid");
  if (uidItem != nullptr && uidItem->valuestring != nullptr) {
    out.uid = uidItem->valuestring;
  }

  // Canonical protobuf JSON mapping: `bytes gid` travels as base64 -
  // unlike SPIRC's TrackRef, no hex round-trip needed here.
  cJSON* gidItem = cJSON_GetObjectItem(trackItem, "gid");
  if (gidItem != nullptr && gidItem->valuestring != nullptr &&
      gidItem->valuestring[0] != '\0') {
    out.gid = Crypto::base64Decode(gidItem->valuestring);
  }
  if (out.gid.empty() && !out.uri.empty()) {
    out.decodeURI();  // TrackReference's own base62 URI->gid fallback
  }
  if (out.uri.find("episode:") != std::string::npos) {
    out.type = TrackReference::Type::EPISODE;
  }

  return !out.gid.empty();
}

bool ContextResolver::fetchJson(const std::string& path, std::string& jsonOut) {
  auto accessToken = login5->getToken();
  auto clientToken = login5->getClientToken();
  if (accessToken.empty() || clientToken.empty()) {
    CSPOT_LOG(error, "context-resolve GET %s skipped: missing token(s)",
             path.c_str());
    return false;
  }

  try {
    std::string host;
    {
      std::lock_guard<std::mutex> lock(hostMutex);
      if (spclientHost.empty()) {
        spclientHost = ApResolve("").fetchFirstSpclientAddress();
      }
      host = spclientHost;
    }
    auto url = "https://" + host + "/" + path;

    bell::HTTPClient::Headers headers = {
        {"Authorization", "Bearer " + accessToken},
        {"Client-Token", clientToken},
        {"Accept", "application/json"}};

    // Kept-alive connection, reused across fetches (pages of one resolve
    // and successive resolves) - one TLS handshake per session, not per
    // fetch.
    if (connection == nullptr) {
      connection = bell::HTTPClient::get(url, headers);
    } else {
      connection->get(url, headers);
    }

    int status = connection->statusCode();
    // Drain the body regardless of status - the connection is reused, and
    // an undrained response desyncs the next request's parse.
    jsonOut = std::string(connection->body());
    if (status != 200) {
      CSPOT_LOG(error, "context-resolve GET %s failed, status %d",
               path.c_str(), status);
      return false;
    }
    return true;
  } catch (const std::exception& e) {
    // Transport failure - drop the cached host/connection so the next call
    // starts fresh instead of reusing a possibly-stale target.
    connection.reset();
    {
      std::lock_guard<std::mutex> lock(hostMutex);
      spclientHost.clear();
    }
    CSPOT_LOG(error, "context-resolve GET %s failed: %s", path.c_str(),
             e.what());
    return false;
  }
}

cJSON* ContextResolver::fetchAndParse(const std::string& path) {
  return HttpRetry(3, std::chrono::milliseconds(500), "context-resolve")
      .run([&]() -> cJSON* {
        std::string json;
        if (!fetchJson(path, json)) {
          throw std::runtime_error("fetch failed");  // already logged
        }
        cJSON* parsed = cJSON_Parse(json.c_str());
        if (parsed == nullptr) {
          throw std::runtime_error("response wasn't valid JSON");
        }
        return parsed;
      });
}

bool ContextResolver::resolve(const std::string& contextUri,
                              std::vector<TrackReference>& tracksOut) {
  // Not applied to the "loading" case below - see its own comment.
  cJSON* root = nullptr;
  try {
    root = fetchAndParse("context-resolve/v1/" + contextUri);
  } catch (const std::exception&) {
    return false;  // fetchAndParse() already logged the final failure
  }

  cJSON* loadingItem = cJSON_GetObjectItem(root, "loading");
  if (loadingItem != nullptr && cJSON_IsTrue(loadingItem)) {
    // go-librespot treats this as retriable, but it's never been observed
    // in practice - no retry here without real data to design one against.
    CSPOT_LOG(error,
             "context-resolve: '%s' is still loading server-side, giving up",
             contextUri.c_str());
    cJSON_Delete(root);
    return false;
  }

  bool ok = true;
  cJSON* pages = cJSON_GetObjectItem(root, "pages");
  if (pages != nullptr) {
    cJSON* page = nullptr;
    cJSON_ArrayForEach(page, pages) {
      // A single inline page is the expected case - looping defensively
      // costs nothing if there's ever more than one.
      if (!followPageChain(page, tracksOut)) {
        ok = false;
        break;
      }
    }
  }

  cJSON_Delete(root);
  return ok && !tracksOut.empty();
}

bool ContextResolver::followPageChain(cJSON* firstPage,
                                      std::vector<TrackReference>& tracksOut) {
  cJSON* ownedRoot = nullptr;  // non-null once `current` is a fetched page,
                               // not the caller-owned `firstPage`
  cJSON* current = firstPage;

  while (true) {
    cJSON* tracksItem = cJSON_GetObjectItem(current, "tracks");

    // Some pages arrive as just a reference (page_url, no inline tracks) -
    // fetch the real page before reading its tracks/next_page_url.
    if (tracksItem == nullptr || cJSON_GetArraySize(tracksItem) == 0) {
      cJSON* pageUrlItem = cJSON_GetObjectItem(current, "page_url");
      if (pageUrlItem != nullptr && pageUrlItem->valuestring != nullptr &&
          pageUrlItem->valuestring[0] != '\0') {
        cJSON* fetched;
        try {
          fetched = fetchAndParse(stripHmPrefix(pageUrlItem->valuestring));
        } catch (const std::exception&) {
          if (ownedRoot != nullptr) cJSON_Delete(ownedRoot);
          return false;
        }
        if (ownedRoot != nullptr) cJSON_Delete(ownedRoot);
        ownedRoot = fetched;
        current = ownedRoot;
        tracksItem = cJSON_GetObjectItem(current, "tracks");
      }
    }

    if (tracksItem != nullptr) {
      appendTracks(tracksItem, tracksOut);
    }

    cJSON* nextPageUrlItem = cJSON_GetObjectItem(current, "next_page_url");
    if (nextPageUrlItem == nullptr || nextPageUrlItem->valuestring == nullptr ||
        nextPageUrlItem->valuestring[0] == '\0') {
      break;
    }

    cJSON* nextPage;
    try {
      nextPage = fetchAndParse(stripHmPrefix(nextPageUrlItem->valuestring));
    } catch (const std::exception&) {
      if (ownedRoot != nullptr) cJSON_Delete(ownedRoot);
      return false;
    }
    if (ownedRoot != nullptr) cJSON_Delete(ownedRoot);
    ownedRoot = nextPage;
    current = ownedRoot;
  }

  if (ownedRoot != nullptr) {
    cJSON_Delete(ownedRoot);
  }
  return true;
}
