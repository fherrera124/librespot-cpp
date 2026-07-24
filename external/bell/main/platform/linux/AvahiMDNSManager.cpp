// Own header
#include "bell/mdns/Manager.h"

// Standard includes
#include <cassert>
#include <mutex>
#include <set>
#include <system_error>
#include <utility>
#include <vector>

// Bell includes
#include "bell/Logger.h"
#include "bell/Result.h"
#include "bell/mdns/Error.h"
#include "bell/net/IpAddress.h"
#include "bell/utils/Semaphore.h"
#include "bell/utils/Task.h"
#include "bell/utils/Utils.h"

// Library includes
#include "fmt/format.h"
#include "tl/expected.hpp"

// Avahi includes
#include <avahi-client/client.h>
#include <avahi-client/lookup.h>
#include <avahi-client/publish.h>
#include <avahi-common/address.h>
#include <avahi-common/defs.h>
#include <avahi-common/error.h>
#include <avahi-common/simple-watch.h>
#include <avahi-common/strlst.h>

using namespace bell::mdns;

namespace {
const char* LOG_TAG = "AvahiMDNSManager";

// Mapping Avahi error codes to std::error_code
inline std::error_code translateAvahiError(int err) {
  switch (err) {
    case AVAHI_OK:
      return make_error_code(MdnsErrc::success);
    case AVAHI_ERR_INVALID_HOST_NAME:
    case AVAHI_ERR_INVALID_DOMAIN_NAME:
    case AVAHI_ERR_INVALID_TTL:
      return make_error_code(bell::mdns::MdnsErrc::invalid_parameters);
    case AVAHI_ERR_INVALID_FLAGS:
      return std::make_error_code(std::errc::invalid_argument);
    case AVAHI_ERR_TIMEOUT:
      return std::make_error_code(std::errc::timed_out);
    case AVAHI_ERR_NO_MEMORY:
      return std::make_error_code(std::errc::not_enough_memory);
    case AVAHI_ERR_NO_NETWORK:
      return std::make_error_code(std::errc::network_unreachable);
    case AVAHI_ERR_INVALID_INTERFACE:
      return std::make_error_code(std::errc::no_such_device);
    case AVAHI_ERR_INVALID_PROTOCOL:
      return std::make_error_code(std::errc::protocol_not_supported);
    case AVAHI_ERR_INVALID_SERVICE_NAME:
    case AVAHI_ERR_INVALID_SERVICE_TYPE:
      return make_error_code(bell::mdns::MdnsErrc::invalid_parameters);
    case AVAHI_ERR_DISCONNECTED:
      return std::make_error_code(std::errc::connection_aborted);
    case AVAHI_ERR_NO_DAEMON:
      return std::make_error_code(std::errc::no_such_device);
    case AVAHI_ERR_INVALID_CONFIG:
      return make_error_code(bell::mdns::MdnsErrc::invalid_parameters);
    case AVAHI_ERR_NOT_FOUND:
      return std::make_error_code(std::errc::no_such_file_or_directory);
    case AVAHI_ERR_COLLISION:
      return make_error_code(bell::mdns::MdnsErrc::name_conflict);
    default:
      return make_error_code(bell::mdns::MdnsErrc::unknown);
  }
}

// Helper function to parse Avahi TXT records into a map
inline void parseAvahiTxtValues(
    AvahiStringList* txt,
    std::unordered_map<std::string, std::string>& txtRecords) {
  txtRecords.clear();

  for (AvahiStringList* entry = txt; entry != nullptr;
       entry = avahi_string_list_get_next(entry)) {
    char* key;
    size_t size;
    char* value;
    if (avahi_string_list_get_pair(entry, &key, &value, &size) == 0) {
      txtRecords[key] = (value != nullptr ? value : "");
      free(key);
      free(value);
    }
  }
}
}  // namespace

class AvahiMDNSBrowser : public Browser {
 public:
  // Constructor
  AvahiMDNSBrowser(AvahiClient* avahiClient) : avahiClient(avahiClient) {}

  ~AvahiMDNSBrowser() override { stopDiscovery(); }

  // Delete the copy and move constructors
  AvahiMDNSBrowser(const AvahiMDNSBrowser&) = delete;
  AvahiMDNSBrowser& operator=(const AvahiMDNSBrowser&) = delete;

  // Context passed as a pointer to the address and service resolve methods
  struct ResolveContext {
    ResolveContext() = default;

    // Delete the copy and move constructors
    ResolveContext(const ResolveContext&) = delete;
    ResolveContext& operator=(const ResolveContext&) = delete;

    uint32_t serviceHash = 0;  // Used to identify the service being resolved
    AvahiServiceResolver* resolveRef = nullptr;
    AvahiMDNSBrowser* browserPtr = nullptr;

    // Destructor, used to deallocate the Avahi service resolver
    ~ResolveContext() {
      if (resolveRef) {
        avahi_service_resolver_free(resolveRef);
        resolveRef = nullptr;
      }
    };
  };

  bell::Result<> browse(const std::string& serviceType,
                        const std::string& serviceDomain, int interfaceIndex,
                        const Browser::DiscoveryEventCallback& onEvent,
                        bool autoResolveService, bool resolveIpv6) {
    // Ensure no previous browse is active
    stopDiscovery();

    this->onEvent = onEvent;
    this->autoResolveService = autoResolveService;
    this->resolveIpv6 = resolveIpv6;

    AvahiServiceBrowserCallback avahiBrowseCallback =
        [](AvahiServiceBrowser* /*browser*/, AvahiIfIndex interface,
           AvahiProtocol protocol, AvahiBrowserEvent event, const char* name,
           const char* type, const char* domain, AvahiLookupResultFlags flags,
           void* ctx) {
          // Simply pass the C-Style callback to the member function
          static_cast<AvahiMDNSBrowser*>(ctx)->avahiBrowseCallback(
              interface, protocol, event, name, type, domain, flags);
        };

    avahiBrowser = avahi_service_browser_new(
        avahiClient, interfaceIndex == 0 ? AVAHI_IF_UNSPEC : interfaceIndex,
        resolveIpv6 ? AVAHI_PROTO_UNSPEC : AVAHI_PROTO_INET,
        serviceType.c_str(),
        serviceDomain.empty() ? nullptr : serviceDomain.c_str(),
        static_cast<AvahiLookupFlags>(0), avahiBrowseCallback, this);

    if (avahiBrowser == nullptr) {
      int avahiErr = avahi_client_errno(avahiClient);
      BELL_LOG(error, LOG_TAG, "Failed to create Avahi service browser, {}",
               avahi_strerror(avahiErr));
      return tl::make_unexpected(translateAvahiError(avahiErr));
    }

    return {};
  }

  bell::Result<> resolveService(const ServiceRecord& service) override {
    std::scoped_lock lock(serviceMutex);
    return resolveServiceWithProtocol(service, AVAHI_PROTO_UNSPEC);
  }

  bell::Result<> resolveAddress(const ServiceRecord& service) override {
    (void)service;
    BELL_LOG(warn, LOG_TAG,
             "No need to manually resolve addresses with Avahi, the mDNS "
             "service's address is automatically resolved.");
    return {};
  }

  void stopDiscovery() override {
    std::scoped_lock lock(serviceMutex);

    if (avahiBrowser != nullptr) {
      avahi_service_browser_free(avahiBrowser);
      avahiBrowser = nullptr;

      resolveContexts.clear();
      registeredServices.clear();
    }
  }

 private:
  const char* LOG_TAG = "AvahiMDNSBrowser";

  // Pointer to the event callback
  DiscoveryEventCallback onEvent;

  // Pointer to the avahi client
  AvahiClient* avahiClient = nullptr;

  // Pointer to the Avahi browser
  AvahiServiceBrowser* avahiBrowser = nullptr;

  // Browser options
  bool autoResolveService = true;
  bool resolveIpv6 = true;

  // Set of registered services
  std::set<ServiceRecord> registeredServices;

  // Holds currently active resolve contexts
  std::vector<std::unique_ptr<ResolveContext>> resolveContexts;

  // Protects access to the registered services and resolve contexts
  std::recursive_mutex serviceMutex;

  void avahiResolveCallback(
      AvahiIfIndex /*interfaceIndex*/, AvahiProtocol /*protocol*/,
      AvahiResolverEvent event, const char* /*serviceName*/,
      const char* /*serviceType*/, const char* /*serviceDomain*/,
      const char* hostname, const AvahiAddress* address, uint16_t port,
      AvahiStringList* txtRecords, AvahiLookupResultFlags /*flags*/,
      ResolveContext* context) {
    std::scoped_lock lock(serviceMutex);

    // Find the service record by its hash
    auto serviceItr =
        std::find_if(registeredServices.begin(), registeredServices.end(),
                     [context](const ServiceRecord& s) {
                       return s.getHash() == context->serviceHash;
                     });

    if (serviceItr != registeredServices.end()) {
      ServiceRecord service = *serviceItr;

      switch (event) {
        case AVAHI_RESOLVER_FAILURE: {
          int avahiErr = avahi_client_errno(avahiClient);
          DiscoveryEvent event{
              .type = EventType::ResolveFailure,
              .service = service,
              .error = translateAvahiError(avahiErr),
          };
          onEvent(event);
          break;
        }
        case AVAHI_RESOLVER_FOUND: {
          std::string addressStr(AVAHI_ADDRESS_STR_MAX, '\0');

          if (!service.serviceResolved) {
            parseAvahiTxtValues(txtRecords, service.txtRecords);
            service.hostname = hostname;
            service.port = port;
            service.serviceResolved = true;

            // Notify of the resolved event
            DiscoveryEvent event{
                .type = EventType::Resolved,
                .service = service,
            };

            onEvent(event);
          }

          avahi_address_snprint(addressStr.data(), addressStr.size(), address);
          addressStr.erase(addressStr.find('\0'));

          auto parsedAddress = bell::net::IpAddress::fromString(addressStr);

          if (parsedAddress.has_value()) {
            if (parsedAddress->getType() == bell::net::IpAddress::Type::IPv4) {
              service.ipv4 = parsedAddress.value();
            } else if (parsedAddress->getType() ==
                       bell::net::IpAddress::Type::IPv6) {
              service.ipv6 = parsedAddress.value();
            }

            // Notify of the address resolved event
            DiscoveryEvent event{
                .type = EventType::AddressResolved,
                .service = service,
            };

            onEvent(event);
          }

          break;
        }
        default:
          break;
      }
    }
  }

  // Resolve the service with the given protocol
  bell::Result<> resolveServiceWithProtocol(const ServiceRecord& service,
                                            AvahiProtocol protocol) {

    // Wrap C-style callback
    AvahiServiceResolverCallback resolverCallback =
        [](AvahiServiceResolver* /*resolver*/, AvahiIfIndex interfaceIndex,
           AvahiProtocol protocol, AvahiResolverEvent event,
           const char* serviceName, const char* serviceType,
           const char* serviceDomain, const char* hostname,
           const AvahiAddress* addresses, uint16_t port,
           AvahiStringList* txtRecords, AvahiLookupResultFlags flags,
           void* context) {
          auto* resolveContext = static_cast<ResolveContext*>(context);

          // Pass to member function
          resolveContext->browserPtr->avahiResolveCallback(
              interfaceIndex, protocol, event, serviceName, serviceType,
              serviceDomain, hostname, addresses, port, txtRecords, flags,
              resolveContext);
        };

    // Prepare the resolve context
    auto resolveContext = std::make_unique<ResolveContext>();
    resolveContext->browserPtr = this;
    resolveContext->serviceHash = service.getHash();

    resolveContext->resolveRef = avahi_service_resolver_new(
        avahiClient, static_cast<AvahiIfIndex>(service.interfaceIndex),
        protocol, service.name.c_str(), service.regType.c_str(),
        service.domain.c_str(),
        resolveIpv6 ? AVAHI_PROTO_UNSPEC : AVAHI_PROTO_INET,
        static_cast<AvahiLookupFlags>(0), resolverCallback,
        resolveContext.get());

    if (resolveContext->resolveRef == nullptr) {
      int avahiErr = avahi_client_errno(avahiClient);
      return tl::make_unexpected(translateAvahiError(avahiErr));
    }

    // Add the resolve context to the cached list
    resolveContexts.push_back(std::move(resolveContext));
    return {};
  }

  // Implementation of the Avahi callback for service discovery
  void avahiBrowseCallback(AvahiIfIndex interface, AvahiProtocol protocol,
                           AvahiBrowserEvent event, const char* name,
                           const char* type, const char* domain,
                           AvahiLookupResultFlags /*flags*/) {
    std::scoped_lock lock(serviceMutex);

    // Construct the service record
    ServiceRecord service{
        .name = name ? name : "",
        .regType = type ? type : "",
        .domain = domain ? domain : "",
        .interfaceIndex = static_cast<uint32_t>(interface),
    };

    switch (event) {
      case AVAHI_BROWSER_FAILURE: {
        int avahiErr = avahi_client_errno(avahiClient);
        BELL_LOG(error, LOG_TAG, "Avahi browser failure {}",
                 avahi_strerror(avahiErr));

        DiscoveryEvent event = {
            .type = EventType::BrowseFailure,
            .service = service,
            .error = translateAvahiError(avahiErr),
        };

        onEvent(event);
        break;
      }
      case AVAHI_BROWSER_NEW: {
        // Check if the service is already registered
        registeredServices.insert(service);

        DiscoveryEvent event{
            .type = EventType::Added,
            .service = service,
        };
        onEvent(event);

        if (autoResolveService) {
          auto res = resolveServiceWithProtocol(service, protocol);
          if (!res) {
            // Resolve failed, notify
            DiscoveryEvent event{
                .type = EventType::ResolveFailure,
                .service = service,
                .error = res.error(),
            };
            onEvent(event);
          }
        }
        break;
      }
      case AVAHI_BROWSER_REMOVE: {
        // Clean up service resolve operations
        std::erase_if(resolveContexts, [&service](const auto& ctx) {
          return ctx->serviceHash == service.getHash();
        });

        // Remove the service from the list of registered services
        registeredServices.erase(service);

        DiscoveryEvent event{
            .type = EventType::Removed,
            .service = service,
        };

        onEvent(event);
        break;
      }
      case AVAHI_BROWSER_ALL_FOR_NOW:
      case AVAHI_BROWSER_CACHE_EXHAUSTED:
        break;
    }
  }
};

class AvahiMDNSAdvertiser : public Advertiser {
 public:
  AvahiMDNSAdvertiser(AvahiClient* avahiClient) : avahiClient(avahiClient) {}
  ~AvahiMDNSAdvertiser() override { stopAdvertising(); }

  // Remove the copy constructor and assignment operator
  AvahiMDNSAdvertiser(const AvahiMDNSAdvertiser&) = delete;
  AvahiMDNSAdvertiser& operator=(const AvahiMDNSAdvertiser&) = delete;

  bell::Result<> advertise(
      const std::string& serviceName, const std::string& serviceType,
      const std::string& serviceDomain, const std::string& serviceHost,
      uint16_t port,
      const std::unordered_map<std::string, std::string>& txtRecords,
      int interfaceIndex) {

    // Create a new Avahi entry group
    entryGroup =
        avahi_entry_group_new(avahiClient, avahiGroupCallback, nullptr);

    if (entryGroup == nullptr) {
      return tl::make_unexpected(MdnsErrc::service_registration_failed);
    }

    // Construct the TXT data
    AvahiStringList* avahiTxt = nullptr;
    for (const auto& [key, value] : txtRecords) {
      avahiTxt =
          avahi_string_list_add_pair(avahiTxt, key.c_str(), value.c_str());
    }

    // Register the service with the constructed TXT data
    int ret = avahi_entry_group_add_service_strlst(
        entryGroup, interfaceIndex == 0 ? AVAHI_IF_UNSPEC : interfaceIndex,
        AVAHI_PROTO_UNSPEC, (AvahiPublishFlags)0, serviceName.c_str(),
        serviceType.c_str(),
        serviceDomain.empty() ? nullptr : serviceDomain.c_str(),
        serviceHost.empty() ? nullptr : serviceHost.c_str(), port, avahiTxt);

    // Free the TXT data, no longer needed at this point
    avahi_string_list_free(avahiTxt);

    if (ret >= 0) {
      // Success, commit the entry group
      ret = avahi_entry_group_commit(entryGroup);
    } else if (ret < 0) {
      BELL_LOG(error, LOG_TAG, "Failed to register service: {}",
               avahi_strerror(ret));
    }

    if (ret < 0) {
      return tl::make_unexpected(MdnsErrc::service_registration_failed);
    }

    return {};
  }

  void stopAdvertising() override {
    if (entryGroup != nullptr) {
      avahi_entry_group_free(entryGroup);
      entryGroup = nullptr;
    }
  }

 private:
  AvahiClient* avahiClient = nullptr;
  AvahiEntryGroup* entryGroup = nullptr;

  // Callback for Avahi entry group state changes, used for debugging
  static void avahiGroupCallback(AvahiEntryGroup* group,
                                 AvahiEntryGroupState state,
                                 void* /*userdata*/) {
    (void)state;
    (void)group;
    assert(group != nullptr);
    // Not used, but can be useful for debugging
    // TODO: Technically we could use this to check for name conflicts and such
  }
};

class AvahiMDNSManager : public Manager, bell::Task {
 public:
  AvahiMDNSManager()
      : bell::Task("avahi_mdns_manager", 1024, 0),
        avahiPoll(avahi_simple_poll_new()) {
    if (avahiPoll == nullptr) {
      throw std::runtime_error("Failed to create Avahi poll object");
    }

    // Create the Avahi client
    avahiClient = avahi_client_new(
        avahi_simple_poll_get(avahiPoll), static_cast<AvahiClientFlags>(0),
        avahiClientCallback, &connectedSemaphore, nullptr);

    if (avahiClient == nullptr) {
      throw std::runtime_error("Failed to create Avahi client");
    }

    // Start polling
    startTask();

    // Wait for the Avahi client to connect. Technically most of the cases this should be instant, with the semaphore already given by the callback.
    if (!connectedSemaphore.take(5000)) {
      throw std::runtime_error("Connection to Avahi client timed out");
    }
  };

  ~AvahiMDNSManager() {
    stopTask();

    if (avahiClient != nullptr) {
      avahi_client_free(avahiClient);
      avahiClient = nullptr;
    }

    if (avahiPoll != nullptr) {
      avahi_simple_poll_free(avahiPoll);
      avahiPoll = nullptr;
    }
  }

  bell::Result<std::unique_ptr<Browser>> browse(
      const std::string& serviceType, const std::string& serviceDomain,
      int interfaceIndex, const Browser::DiscoveryEventCallback& onEvent,
      bool autoResolveService, bool /*autoResolveAddresses */,
      bool resolveIPv6) override {
    auto browser = std::make_unique<AvahiMDNSBrowser>(avahiClient);
    auto res = browser->browse(serviceType, serviceDomain, interfaceIndex,
                               onEvent, autoResolveService, resolveIPv6);

    if (!res) {
      return tl::make_unexpected(res.error());
    }

    return browser;
  }

  bell::Result<std::unique_ptr<Advertiser>> advertise(
      const std::string& serviceName, const std::string& serviceType,
      const std::string& serviceDomain, const std::string& serviceHost,
      uint16_t port,
      const std::unordered_map<std::string, std::string>& txtRecords,
      int interfaceIndex) override {
    auto advertiser = std::make_unique<AvahiMDNSAdvertiser>(avahiClient);
    auto res =
        advertiser->advertise(serviceName, serviceType, serviceDomain,
                              serviceHost, port, txtRecords, interfaceIndex);

    if (!res) {
      return tl::make_unexpected(res.error());
    }

    return advertiser;
  }

  // Returns a pointer to the singleton instance
  static AvahiMDNSManager* getDefaultManager() {
    static AvahiMDNSManager* defaultManagerInstance = nullptr;

    if (!defaultManagerInstance) {
      defaultManagerInstance = new AvahiMDNSManager();
    }
    return defaultManagerInstance;
  }

 private:
  AvahiSimplePoll* avahiPoll = nullptr;
  AvahiClient* avahiClient = nullptr;
  bell::Semaphore connectedSemaphore;

  // Callback for Avahi client state changes, reports the running state, as well as logs any failures
  static void avahiClientCallback(AvahiClient* client, AvahiClientState state,
                                  void* userdata) {
    assert(client != nullptr);
    auto* connectedSem = static_cast<bell::Semaphore*>(userdata);

    switch (state) {
      case AVAHI_CLIENT_S_RUNNING:
        connectedSem->give();
        break;
      case AVAHI_CLIENT_FAILURE:
        BELL_LOG(error, LOG_TAG, "Avahi client failure {}",
                 avahi_strerror(avahi_client_errno(client)));
        throw std::runtime_error("Avahi client failure");
        break;
      default:
        BELL_LOG(debug, LOG_TAG, "Avahi client state: {}",
                 static_cast<int>(state));
        break;
    }
  }

  void taskLoop() override {
    if (avahi_simple_poll_iterate(avahiPoll, 1000) < 0) {
      BELL_LOG(error, LOG_TAG, "Avahi poll iterate failed, exitting loop {}",
               avahi_strerror(avahi_client_errno(avahiClient)));
      stopTask();
    }
  }
};

Manager* bell::mdns::getDefaultManager() {
  return AvahiMDNSManager::getDefaultManager();
}
