#include "ApResolve.h"

#include <initializer_list>  // for initializer_list
#include <map>               // for operator!=, operator==
#include <memory>            // for allocator, unique_ptr
#include <stdexcept>         // for runtime_error
#include <vector>            // for vector

#include "HTTPClient.h"  // for HTTPClient, HTTPClient::Response
#include "cJSON.h"

using namespace cspot;

ApResolve::ApResolve(std::string apOverride) {
  this->apOverride = apOverride;
}

std::string ApResolve::fetchFirstApAddress() {
  if (apOverride != "") {
    return apOverride;
  }

  auto request = bell::HTTPClient::get("https://apresolve.spotify.com/");
  // Copied into a std::string (not left as the string_view body() actually
  // returns) specifically so .c_str() below is guaranteed null-terminated -
  // cJSON_Parse() scans for '\0' to know where to stop, and a
  // string_view's .data() carries no such guarantee (it's a raw view into
  // HTTPClient's own read buffer). Same fix as AccessKeyFetcher.cpp's own
  // identical bug, found while investigating a real access-token failure
  // that traced back to this exact pattern - this call site never visibly
  // failed from it, purely by buffer-layout luck, but it was the same
  // undefined behavior underneath.
  std::string responseStr(request->body());

  // FIX: this used to skip checking that "ap_list" actually existed and
  // had at least one element before indexing into it - an empty/malformed
  // response (e.g. a proxy error page instead of the real
  // apresolve.spotify.com JSON) dereferenced a NULL cJSON node, relying
  // entirely on the generic top-level catch (finding F17) instead of
  // failing with a clear message. See docs/spotify_component_analysis.md,
  // finding F36.
  cJSON* json = cJSON_Parse(responseStr.c_str());
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
}

// Modern typed resolution (?type=dealer / ?type=spclient) - the response
// keys each list by its type name, unlike the legacy no-query form's
// "ap_list" above. Same F36 hardening: never index before checking.
std::vector<std::string> ApResolve::fetchAddressesOfType(
    const std::string& type) {
  auto request =
      bell::HTTPClient::get("https://apresolve.spotify.com/?type=" + type);
  // See fetchFirstApAddress()'s own comment on why this is a std::string,
  // not the string_view body() actually returns.
  std::string responseStr(request->body());

  cJSON* json = cJSON_Parse(responseStr.c_str());
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
