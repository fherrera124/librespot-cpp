#include "AccessKeyFetcher.h"

#include <cstring>           // for strrchr
#include <initializer_list>  // for initializer_list
#include <map>               // for operator!=, operator==
#include <type_traits>       // for remove_extent_t
#include <vector>            // for vector

#include "BellLogger.h"    // for AbstractLogger
#include "BellUtils.h"     // for BELL_SLEEP_MS
#include "CSpotContext.h"  // for Context
#include "HTTPClient.h"
#include "Logger.h"            // for CSPOT_LOG
#include "MercurySession.h"    // for MercurySession, MercurySession::Res...
#include "NanoPBExtensions.h"  // for bell::nanopb::encode...
#include "NanoPBHelper.h"      // for pbEncode and pbDecode
#include "Packet.h"            // for cspot
#include "TimeProvider.h"      // for TimeProvider
#include "Utils.h"             // for string_format

#ifdef BELL_ONLY_CJSON
#include "cJSON.h"
#else
#include "nlohmann/json.hpp"      // for basic_json<>::object_t, basic_json
#include "nlohmann/json_fwd.hpp"  // for json
#endif

using namespace cspot;

static std::string SCOPES =
    "streaming,user-library-read,user-library-modify,user-top-read,user-read-"
    "recently-played";  // Required access scopes

AccessKeyFetcher::AccessKeyFetcher(std::shared_ptr<cspot::Context> ctx)
    : ctx(ctx) {}

bool AccessKeyFetcher::isExpired() {
  if (accessKey.empty()) {
    return true;
  }

  if (ctx->timeProvider->getSyncedTimestamp() > expiresAt) {
    return true;
  }

  return false;
}

std::string AccessKeyFetcher::getAccessKey() {
  if (!isExpired()) {
    return accessKey;
  }

  updateAccessKey();

  return accessKey;
}

void AccessKeyFetcher::updateAccessKey() {
  if (keyPending) {
    // Already pending refresh request
    return;
  }

  keyPending = true;

  // Max retry of 3, can receive different hash cat types
  int retryCount = 3;
  bool success = false;

  do {
    CSPOT_LOG(info, "Access token expired, fetching new one...");

    auto credentials = "grant_type=client_credentials&client_id=" + ctx->config.clientId + "&client_secret=" + ctx->config.clientSecret;
    std::vector<uint8_t> body(credentials.begin(), credentials.end());

    // FIX: neither branch used to check that the response actually parsed
    // or that "access_token"/"expires_in" were present before dereferencing
    // them - a malformed/truncated response (e.g. a proxy error page
    // instead of the real token response) crashed via a NULL cJSON
    // ->valuestring deref (cJSON branch) or an uncaught nlohmann exception
    // (nlohmann branch). This runs on TrackQueue's own task ("CSpotTrackQueue"),
    // a different task/stack than the one the runSession() try/catch
    // (finding F17) wraps - so an uncaught failure here escapes straight to
    // std::terminate()/abort() and reboots the device. See
    // docs/spotify_component_analysis.md, finding F26.
    //
    // The HTTP POST itself can also throw (TLSSocket handshake failure) -
    // same F26 gap, uncaught here it rebooted the device. See F63.
    std::unique_ptr<bell::HTTPClient::Response> response;
    try {
      response = bell::HTTPClient::post(
          "https://accounts.spotify.com/api/token",
          { {"Content-Type", "application/x-www-form-urlencoded"} }, body);
    } catch (const std::exception& e) {
      CSPOT_LOG(error, "Failed to reach Spotify token endpoint: %s", e.what());
      BELL_SLEEP_MS(3000);
      retryCount--;
      continue;
    }

#ifdef BELL_ONLY_CJSON
    cJSON* root = cJSON_Parse(response->body().data());
    cJSON* tokenItem = root ? cJSON_GetObjectItem(root, "access_token") : nullptr;
    cJSON* expiresItem = root ? cJSON_GetObjectItem(root, "expires_in") : nullptr;
    bool hasError = root ? (cJSON_GetObjectItem(root, "error") != nullptr) : true;
    if (root && !hasError && tokenItem && tokenItem->valuestring && expiresItem) {
        accessKey = std::string(tokenItem->valuestring);
        int expiresIn = expiresItem->valueint;
        cJSON_Delete(root);
#else
    std::string tokenValue;
    int expiresIn = 0;
    bool parsedOk = false;
    try {
      auto root = nlohmann::json::parse(response->bytes());
      if (!root.contains("error") && root.contains("access_token") &&
          root.contains("expires_in")) {
        tokenValue = root["access_token"].get<std::string>();
        expiresIn = root["expires_in"].get<int>();
        parsedOk = true;
      }
    } catch (const std::exception& e) {
      CSPOT_LOG(error, "Failed to parse access token response: %s", e.what());
    }
    if (parsedOk) {
        accessKey = tokenValue;
#endif
        // Successfully received an auth token
      CSPOT_LOG(info, "Access token sucessfully fetched");
      success = true;

      this->expiresAt =
            ctx->timeProvider->getSyncedTimestamp() + (expiresIn * 1000);
    }
    else {
#ifdef BELL_ONLY_CJSON
      if (root) cJSON_Delete(root);
#endif
      CSPOT_LOG(error, "Failed to fetch access token");
      BELL_SLEEP_MS(3000);
    }

    retryCount--;
  } while (retryCount >= 0 && !success);

  keyPending = false;
}
