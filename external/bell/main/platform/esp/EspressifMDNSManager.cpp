// Own header
#include "bell/mdns/Manager.h"

// Standard includes
#include <mutex>
#include <set>
#include <utility>
#include <vector>

// Bell includes
#include "bell/Logger.h"
#include "bell/Result.h"
#include "bell/mdns/Error.h"
#include "bell/net/IpAddress.h"

// Library includes
#include "bell/utils/Task.h"
#include "bell/utils/Utils.h"
#include "fmt/format.h"

// Espressif includes
#include "mdns.h"

using namespace bell::mdns;

class BrowseExecutor : public bell::Task {
 public:
  // Constructor
  BrowseExecutor()
      : bell::Task("mdns_browse_executor", 1024 * 8, 0,
                   bell::utils::TaskCore::Core0) {
    startTask();
  }

  // Destructor
  ~BrowseExecutor() { stopTask(); }

  using BrowseCallback = std::function<void(mdns_result_t*)>;

  // Register a callback for a specific service type
  void registerCallback(const std::string& regType,
                        const BrowseCallback& callback) {
    std::scoped_lock lock(browseMutex);
    browseCallbacks[regType] = callback;
  }

  // Unregister a callback for a specific service type
  void unregisterCallback(const std::string& regType) {
    std::scoped_lock lock(browseMutex);
    browseCallbacks.erase(regType);
  }

 private:
  const char* LOG_TAG = "MdnsBrowseExecutor";

  // Maximum number of results to query
  static const int maxResultsPerQuery = 32;

  // Mutex for thread safety
  std::mutex browseMutex;

  // Map of registered callbacks
  std::unordered_map<std::string, BrowseCallback> browseCallbacks;

  void taskLoop() override {
    // Run the mDNS browse loop
    // Call the registered callbacks
    std::scoped_lock lock(browseMutex);
    for (const auto& [regType, callback] : browseCallbacks) {
      mdns_result_t* result = nullptr;

      std::string regService = regType.substr(0, regType.find_first_of('.'));
      std::string regProto = regType.substr(regType.find_first_of('.') + 1);

      auto res = mdns_query_ptr(regService.c_str(), regProto.c_str(), 5000,
                                maxResultsPerQuery, &result);
      if (res != ESP_OK) {
        BELL_LOG(error, LOG_TAG, "Failed to query mDNS service: {}", res);
        continue;
      }

      if (result != nullptr) {
        // Call the callback with the result
        callback(result);
        mdns_query_results_free(result);
      }
    }

    if (browseCallbacks.empty()) {
      // Sleep for a while to avoid busy waiting
      bell::utils::sleepMs(1000);
    }
  }
};

class EspressifMdnsBrowser : public Browser {
 public:
  // Constructor
  EspressifMdnsBrowser(std::shared_ptr<BrowseExecutor> browseExecutor)
      : browseExecutor(std::move(browseExecutor)) {}

  ~EspressifMdnsBrowser() { stopDiscovery(); }

  bell::Result<> browse(const std::string& regType,
                        const DiscoveryEventCallback& onEvent) {
    // Ensure no previous browse is active
    stopDiscovery();

    this->regType = regType;
    this->regService = regType.substr(0, regType.find_first_of('.'));
    this->regProto = regType.substr(regType.find_first_of('.') + 1);
    this->onEvent = onEvent;

    browseExecutor->registerCallback(
        regType, [this](mdns_result_t* result) { this->parseResults(result); });

    return {};
  }

  bell::Result<> resolveService(const ServiceRecord& service) override {
    (void)service;
    BELL_LOG(warn, LOG_TAG,
             "No need to manually resolve the services on Espressif platforms, "
             "the mDNS service is automatically resolved.");
    return {};
  }

  bell::Result<> resolveAddress(const ServiceRecord& service) override {
    (void)service;
    BELL_LOG(warn, LOG_TAG,
             "No need to manually resolve addresses on Espressif platforms, "
             "the mDNS service's address is automatically resolved.");
    return {};
  }

  void stopDiscovery() override {
    if (!regType.empty()) {
      browseExecutor->unregisterCallback(regType);
      regType.clear();
    }
  }

 private:
  const char* LOG_TAG = "EspressifMdnsBrowser";

  // Pointer to the event callback
  DiscoveryEventCallback onEvent = {};

  // Pointer to the browse executor
  std::shared_ptr<BrowseExecutor> browseExecutor;

  // Service type and protocol
  std::string regService;
  std::string regProto;
  std::string regType;  // regService.regProto

  // Set of discovered services
  std::set<ServiceRecord> recordsCache{};

  // Parse the results from the mDNS query, and notify the event callback
  void parseResults(mdns_result_t* results) {
    mdns_result_t* r = results;
    mdns_ip_addr_t* a = nullptr;

    while (r) {

      // Reconstruct the service type and protocol
      std::string serviceType = fmt::format("{}.{}", r->service_type, r->proto);

      ServiceRecord service(r->instance_name, serviceType, "",
                            esp_netif_get_netif_impl_index(r->esp_netif));

      if (r->hostname) {
        // Service is resolved
        service.hostname = r->hostname;
        service.port = r->port;

        // Parse TXT records
        for (int x = 0; x < r->txt_count; x++) {
          service.txtRecords.insert(
              {std::string(r->txt[x].key),
               std::string(&r->txt[x].value[0],
                           &r->txt[x].value[r->txt_value_len[x]])});
        }

        service.serviceResolved = true;
      }

      a = r->addr;
      while (a) {
        if (a->addr.type == IPADDR_TYPE_V4) {
          std::array<char, IP4ADDR_STRLEN_MAX> strCharData{};
          esp_ip4addr_ntoa(&a->addr.u_addr.ip4, strCharData.data(),
                           IP4ADDR_STRLEN_MAX);
          service.ipv4 = bell::net::IpAddress::fromString(strCharData.data());
        } else if (a->addr.type == IPADDR_TYPE_V6) {
          // TODO: Implement IPv6 support
        }
        a = a->next;
      }

      if (!service.ipv4) {
        // Ugly fix for espressif mdns sometimes missing an address when two instances of the same service are registered
        for (auto& it : recordsCache) {
          if (it.hostname == service.hostname && it.ipv4.has_value()) {
            service.ipv4 = it.ipv4;
            break;
          }
        }
      }

      bool freshRecord = false;

      if (recordsCache.find(service) == recordsCache.end()) {
        if (r->ttl == 0) {
          // Service is already removed
          r = r->next;
          continue;
        }

        // New service, notify the event callback
        DiscoveryEvent event{
            .type = EventType::Added,
            .service = service,
            .error = {},
        };
        onEvent(event);

        // Insert the service into the cache
        recordsCache.insert(service);

        freshRecord = true;
      }

      auto it = recordsCache.find(service);

      if (r->ttl == 0) {
        // Service is being removed
        DiscoveryEvent event{
            .type = EventType::Removed,
            .service = service,
            .error = {},
        };
        onEvent(event);

        // Remove the service from the cache
        recordsCache.erase(it);
        r = r->next;
        continue;
      }

      bool serviceUpdated = false;
      if ((!it->serviceResolved && service.serviceResolved) ||
          (it->serviceResolved && freshRecord)) {
        // Service is resolved, notify the event callback
        DiscoveryEvent event{
            .type = EventType::Resolved,
            .service = service,
            .error = {},
        };
        onEvent(event);

        // Update the service in the cache
        serviceUpdated = true;
      }

      if ((!it->ipv4.has_value() && service.ipv4.has_value()) ||
          (it->ipv4.has_value() && freshRecord)) {
        // Address is resolved, notify the event callback
        DiscoveryEvent event{
            .type = EventType::AddressResolved,
            .service = service,
            .error = {},
        };
        onEvent(event);

        // Update the service in the cache
        serviceUpdated = true;
      }

      if (serviceUpdated && !freshRecord) {
        // Update the service in the cache
        recordsCache.erase(it);
        recordsCache.insert(service);
      }

      r = r->next;
    }
  }
};

class EspressifMDNAdvertiser : public Advertiser {
 public:
  EspressifMDNAdvertiser() = default;
  ~EspressifMDNAdvertiser() override { stopAdvertising(); }

  // Remoce the copy constructor and assignment operator
  EspressifMDNAdvertiser(const EspressifMDNAdvertiser&) = delete;
  EspressifMDNAdvertiser& operator=(const EspressifMDNAdvertiser&) = delete;

  bell::Result<> advertise(
      const std::string& serviceName, const std::string& serviceType,
      uint16_t port,
      const std::unordered_map<std::string, std::string>& txtRecords) {
    // Create a new mDNS service
    this->regService = serviceType.substr(0, serviceType.find_first_of('.'));
    this->regProto = serviceType.substr(serviceType.find_first_of('.') + 1);

    txtItems.reserve(txtRecords.size());

    // Parse the TXT records
    for (const auto& data : txtRecords) {
      mdns_txt_item_t item;
      item.key = data.first.c_str();
      item.value = data.second.c_str();
      txtItems.push_back(item);
    }

    auto res = mdns_service_add(serviceName.c_str(), this->regService.c_str(),
                                this->regProto.c_str(), port, txtItems.data(),
                                txtItems.size());

    if (res != ESP_OK) {
      return tl::make_unexpected(
          bell::mdns::MdnsErrc::service_registration_failed);
    }

    return {};
  }

  void stopAdvertising() override {
    if (!regService.empty()) {
      (void)mdns_service_remove(regService.c_str(), regProto.c_str());
      regService.clear();
      regProto.clear();
      txtItems.clear();
    }
  }

 private:
  std::string regService;
  std::string regProto;
  std::vector<mdns_txt_item_t> txtItems;
};

class EspressifMDNSManager : public Manager {
 public:
  EspressifMDNSManager() = default;

  bell::Result<std::unique_ptr<Browser>> browse(
      const std::string& serviceType, const std::string& /*serviceDomain*/,
      int /*interfaceIndex*/, const Browser::DiscoveryEventCallback& onEvent,
      bool /*autoResolveService */, bool /*autoResolveAddresses */,
      bool /*resolveIPv6*/) override {
    auto browser = std::make_unique<EspressifMdnsBrowser>(browseExecutor);
    auto res = browser->browse(serviceType, onEvent);

    if (!res) {
      return tl::make_unexpected(res.error());
    }

    return browser;
  }

  bell::Result<std::unique_ptr<Advertiser>> advertise(
      const std::string& serviceName, const std::string& serviceType,
      const std::string& /*serviceDomain*/, const std::string& /*serviceHost*/,
      uint16_t port,
      const std::unordered_map<std::string, std::string>& txtRecords,
      int /*interfaceIndex*/) override {
    auto advertiser = std::make_unique<EspressifMDNAdvertiser>();
    auto res =
        advertiser->advertise(serviceName, serviceType, port, txtRecords);

    if (!res) {
      return tl::make_unexpected(res.error());
    }

    return advertiser;
  }

  // Returns a pointer to the singleton instance
  static EspressifMDNSManager* getDefaultManager() {
    static EspressifMDNSManager* defaultManagerInstance = nullptr;

    if (!defaultManagerInstance) {
      defaultManagerInstance = new EspressifMDNSManager();
    }
    return defaultManagerInstance;
  }

 private:
  // Pointer to the browse executor
  std::shared_ptr<BrowseExecutor> browseExecutor =
      std::make_shared<BrowseExecutor>();
};

Manager* bell::mdns::getDefaultManager() {
  return EspressifMDNSManager::getDefaultManager();
}
