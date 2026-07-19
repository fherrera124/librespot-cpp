#include "ApResolve.h"

#include <initializer_list>  // for initializer_list
#include <map>               // for operator!=, operator==
#include <memory>            // for allocator, unique_ptr
#include <stdexcept>         // for runtime_error
#include <string_view>       // for string_view
#include <vector>            // for vector

#include "HTTPClient.h"  // for HTTPClient, HTTPClient::Response
#ifdef BELL_ONLY_CJSON
#include "cJSON.h"
#else
#include "nlohmann/json.hpp"      // for basic_json<>::object_t, basic_json
#include "nlohmann/json_fwd.hpp"  // for json
#endif

using namespace cspot;

ApResolve::ApResolve(std::string apOverride) {
  this->apOverride = apOverride;
}

std::string ApResolve::fetchFirstApAddress() {
  if (apOverride != "") {
    return apOverride;
  }

  auto request = bell::HTTPClient::get("https://apresolve.spotify.com/");
  std::string_view responseStr = request->body();

  // parse json with nlohmann
  //
  // FIX: neither branch used to check that "ap_list" actually existed and
  // had at least one element before indexing into it - an empty/malformed
  // response (e.g. a proxy error page instead of the real apresolve.spotify.com
  // JSON) dereferenced a NULL cJSON node (cJSON branch) or threw an opaque
  // nlohmann exception with no indication the real problem was an empty
  // ap_list (nlohmann branch), relying entirely on the generic top-level
  // catch (finding F17) instead of failing with a clear message. See
  // docs/spotify_component_analysis.md, finding F36.
#ifdef BELL_ONLY_CJSON
  cJSON* json = cJSON_Parse(responseStr.data());
  if (json == nullptr) {
    throw std::runtime_error("ApResolve: failed to parse JSON response");
  }
  cJSON* apList = cJSON_GetObjectItem(json, "ap_list");
  cJSON* firstAp = apList != nullptr ? cJSON_GetArrayItem(apList, 0) : nullptr;
  if (firstAp == nullptr || firstAp->valuestring == nullptr) {
    cJSON_Delete(json);
    throw std::runtime_error("ApResolve: response has no usable ap_list");
  }
  auto ap_string = std::string(firstAp->valuestring);
  cJSON_Delete(json);
  return ap_string;
#else
  auto json = nlohmann::json::parse(responseStr);
  if (!json.contains("ap_list") || !json["ap_list"].is_array() ||
      json["ap_list"].empty()) {
    throw std::runtime_error("ApResolve: response has no usable ap_list");
  }
  return json["ap_list"][0];
#endif
}

// Modern typed resolution (?type=dealer / ?type=spclient) - the response
// keys each list by its type name, unlike the legacy no-query form's
// "ap_list" above. Same F36 hardening: never index before checking.
std::vector<std::string> ApResolve::fetchAddressesOfType(
    const std::string& type) {
  auto request =
      bell::HTTPClient::get("https://apresolve.spotify.com/?type=" + type);
  std::string_view responseStr = request->body();

#ifdef BELL_ONLY_CJSON
  cJSON* json = cJSON_Parse(responseStr.data());
  if (json == nullptr) {
    throw std::runtime_error("ApResolve: failed to parse JSON response");
  }
  cJSON* list = cJSON_GetObjectItem(json, type.c_str());
  if (list == nullptr || cJSON_GetArraySize(list) == 0) {
    cJSON_Delete(json);
    throw std::runtime_error("ApResolve: response has no usable " + type +
                             " list");
  }
  std::vector<std::string> addresses;
  cJSON* item = nullptr;
  cJSON_ArrayForEach(item, list) {
    if (item->valuestring != nullptr) {
      addresses.emplace_back(item->valuestring);
    }
  }
  cJSON_Delete(json);
  if (addresses.empty()) {
    throw std::runtime_error("ApResolve: response has no usable " + type +
                             " list");
  }
  return addresses;
#else
  auto json = nlohmann::json::parse(responseStr);
  if (!json.contains(type) || !json[type].is_array() || json[type].empty()) {
    throw std::runtime_error("ApResolve: response has no usable " + type +
                             " list");
  }
  return json[type].get<std::vector<std::string>>();
#endif
}

std::string ApResolve::fetchFirstAddressOfType(const std::string& type) {
  return fetchAddressesOfType(type)[0];
}

std::vector<std::string> ApResolve::fetchDealerAddresses() {
  return fetchAddressesOfType("dealer");
}

std::vector<std::string> ApResolve::fetchApAddresses() {
  return fetchAddressesOfType("accesspoint");
}

std::string ApResolve::fetchFirstSpclientAddress() {
  return fetchFirstAddressOfType("spclient");
}
