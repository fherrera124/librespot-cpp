#include "MDNSService.h"
#include <arpa/inet.h>
#include <mutex>
#include <vector>
#include "esp_log.h"
#include "mdns.h"

static const char* MDNS_SVC_TAG = "MDNSService";

using namespace bell;

namespace {
std::mutex responderInitMutex;
bool responderInitialized = false;

// Starts the mDNS responder itself (once, lazily) and sets its
// device-level hostname/instance name. Every other platform's
// MDNSService does the equivalent of this internally already - Linux's
// registerService() lazily calls avahi_client_new()/mdnsd_start() on
// first use, so its callers never touch the raw platform mDNS API
// directly. This backend didn't do that before: cspot_connect.cpp had to
// call mdns_init()/mdns_hostname_set()/mdns_instance_name_set() by hand
// before ever calling registerService(). Uses `hostname` (the caller's
// serviceName - the same value cspot_connect.cpp was already passing to
// those calls) so this needed no API change.
void ensureResponderStarted(const std::string& hostname) {
  std::lock_guard<std::mutex> lock(responderInitMutex);
  if (responderInitialized) {
    return;
  }
  esp_err_t err = mdns_init();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(MDNS_SVC_TAG, "mdns_init failed: %s", esp_err_to_name(err));
  }
  mdns_hostname_set(hostname.c_str());
  mdns_instance_name_set(hostname.c_str());
  responderInitialized = true;
}
}  // namespace

class implMDNSService : public MDNSService {
 private:
  const std::string type;
  const std::string proto;
  void unregisterService() { mdns_service_remove(type.c_str(), proto.c_str()); }

 public:
  implMDNSService(std::string type, std::string proto)
      : type(type), proto(proto){};
};

/**
 * ESP32 implementation of MDNSService
 * @see https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/protocols/mdns.html
 **/

std::unique_ptr<MDNSService> MDNSService::registerService(
    const std::string& serviceName, const std::string& serviceType,
    const std::string& serviceProto, const std::string& serviceHost,
    int servicePort, const std::map<std::string, std::string> txtData) {
  ensureResponderStarted(serviceName);

  std::vector<mdns_txt_item_t> txtItems;
  txtItems.reserve(txtData.size());
  for (auto& data : txtData) {
    mdns_txt_item_t item;
    item.key = data.first.c_str();
    item.value = data.second.c_str();
    txtItems.push_back(item);
  }

  // FIX: this return value used to be discarded entirely - if
  // mdns_service_add() itself fails (duplicate service, mdns not ready,
  // etc.) there was no way to tell from the logs, since the caller
  // (cspot_connect.cpp) only logs its own "advertised, waiting to be
  // selected" message regardless of whether this actually succeeded.
  esp_err_t err = mdns_service_add(
      serviceName.c_str(),  /* instance_name */
      serviceType.c_str(),  /* service_type */
      serviceProto.c_str(), /* proto */
      servicePort,          /* port */
      txtItems.data(),      /* txt */
      txtItems.size()       /* num_items */
  );
  if (err != ESP_OK) {
    ESP_LOGE(MDNS_SVC_TAG, "mdns_service_add failed: %s",
             esp_err_to_name(err));
  } else {
    ESP_LOGI(MDNS_SVC_TAG,
             "mdns_service_add OK: instance='%s' type=%s.%s port=%d",
             serviceName.c_str(), serviceType.c_str(), serviceProto.c_str(),
             servicePort);
  }

  return std::make_unique<implMDNSService>(serviceType, serviceProto);
}
