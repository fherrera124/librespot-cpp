// Own header
#include "bell/mdns/Manager.h"

// Standard includes
#include <set>
#include <utility>
#include <vector>

// Library includes
#include <dns_sd.h>

// Bell includes
#include "bell/Result.h"
#include "bell/mdns/Error.h"
#include "bell/net/IpAddress.h"
#include "bell/net/SocketPollListener.h"
#include "bell/net/UDPSocket.h"
#include "bell/utils/Task.h"
#include "tl/expected.hpp"

using namespace bell::mdns;

namespace {
// Mapping DNS-SD error codes to std::error_code
inline std::error_code translateDnsSdError(DNSServiceErrorType err) {
  switch (err) {
    case kDNSServiceErr_NoError:
      return make_error_code(MdnsErrc::success);
    case kDNSServiceErr_BadParam:
      return make_error_code(bell::mdns::MdnsErrc::invalid_parameters);
    case kDNSServiceErr_BadReference:
      return std::make_error_code(std::errc::bad_file_descriptor);
    case kDNSServiceErr_Timeout:
      return std::make_error_code(std::errc::timed_out);
    case kDNSServiceErr_NoMemory:
      return std::make_error_code(std::errc::not_enough_memory);
    default:
      return make_error_code(bell::mdns::MdnsErrc::unknown);
  }
}

// Parse the TXT record data into a map of key-value pairs
bool parseTXTRecords(const char* txtRecord, size_t txtLen,
                     std::unordered_map<std::string, std::string>& txtRecords) {
  // Parse the TXT record data here
  txtRecords.clear();

  size_t i = 0;

  while (i < txtLen) {
    // Get the length of the current key-value pair
    int length = txtRecord[i];
    i++;

    if (i + length > txtLen) {
      // This should not happen if the input is well-formed
      return false;
    }

    // Extract the key-value pair
    std::string pair(reinterpret_cast<const char*>(txtRecord + i), length);
    i += length;

    // Find the position of the '=' character
    size_t pos = pair.find('=');
    if (pos != std::string::npos) {
      // Split into key and value
      std::string key = pair.substr(0, pos);
      std::string value = pair.substr(pos + 1);
      txtRecords[key] = value;
    } else {
      // If there's no '=', then it's just a key with an empty value
      txtRecords[pair] = "";
    }
  }

  return true;
}
}  // namespace

class BonjourBrowser : public Browser {
 public:
  // Constructor
  BonjourBrowser(std::shared_ptr<bell::SocketPollListener> socketPoll)
      : socketPoll(std::move(socketPoll)) {}

  // Context passed as a pointer to the address and service resolve methods
  struct ResolveContext {
    ResolveContext() = default;

    // Delete the copy and move constructors
    ResolveContext(const ResolveContext&) = delete;
    ResolveContext& operator=(const ResolveContext&) = delete;

    uint32_t serviceHash = 0;  // Used to identify the service being resolved
    DNSServiceRef resolveRef = nullptr;
    BonjourBrowser* browserPtr = nullptr;

    // Destructor, used to deallocate the DNS service reference
    ~ResolveContext() {
      if (resolveRef) {
        DNSServiceRefDeallocate(resolveRef);
      }
    };
  };

  bell::Result<> browse(const std::string& regType,
                        const std::string& regDomain, int interfaceIndex,
                        const DiscoveryEventCallback& onEvent,
                        bool autoResolveService = true,
                        bool autoResolveAddresses = true,
                        bool resolveIPv6 = true) {
    this->autoResolveAddresses = autoResolveAddresses;
    this->autoResolveService = autoResolveService;
    this->resolveIpv6 = resolveIPv6;
    this->onEvent = onEvent;

    if (!ref) {
      auto res = DNSServiceCreateConnection(&ref);
      auto err = translateDnsSdError(res);
      if (err) {
        return tl::make_unexpected(err);
      }

      // Get a socket file descriptor for the browse socket
      dnssd_sock_t sock = DNSServiceRefSockFD(ref);
      if (sock == -1) {
        return bell::make_unexpected_errc(std::errc::bad_file_descriptor);
      }

      // Wrap the socket in a UDP socket handle, so we can use it with poll
      wrappedDnsSdSocket = std::make_shared<bell::UDPSocket>(sock);

      // Register the socket with the poller
      socketPoll->registerSocket(
          wrappedDnsSdSocket, bell::PollEvent::Readable,
          [this](auto& /**/) { DNSServiceProcessResult(ref); });
    }

    // Wrap the C-style callback
    DNSServiceBrowseReply browseReplyShim =
        [](DNSServiceRef, DNSServiceFlags flags, uint32_t interface,
           DNSServiceErrorType err, const char* serviceName,
           const char* regType, const char* regDomain, void* context) {
          // Simply pass the data to member func
          static_cast<BonjourBrowser*>(context)->handleBrowseReply(
              flags, interface, err, serviceName, regType, regDomain);
        };

    browseRef = ref;

    // Ref will be initialized by DNSServiceBrowse
    auto res = DNSServiceBrowse(&browseRef, kDNSServiceFlagsShareConnection,
                                interfaceIndex, regType.c_str(),
                                regDomain.empty() ? nullptr : regDomain.c_str(),
                                browseReplyShim, this);
    auto err = translateDnsSdError(res);
    if (err) {
      return tl::make_unexpected(err);
    }

    return {};
  }

  // Callback for DNSServiceBrowse
  void handleBrowseReply(DNSServiceFlags flags, uint32_t interface,
                         DNSServiceErrorType dnsSdErr, const char* serviceName,
                         const char* regType, const char* regDomain) {
    std::scoped_lock lock(serviceMutex);

    auto err = translateDnsSdError(dnsSdErr);

    ServiceRecord service{
        .name = serviceName,
        .regType = regType,
        .domain = regDomain,
        .interfaceIndex = interface,
    };

    if (err) {
      DiscoveryEvent event{
          .type = EventType::BrowseFailure,
          .service = service,
          .error = err,
      };
      onEvent(event);
    } else {
      if (flags & kDNSServiceFlagsAdd) {
        registeredServices.insert(service);

        DiscoveryEvent event{
            .type = EventType::Added,
            .service = service,
        };
        onEvent(event);

        // If autoResolveService is true, start resolving the service
        if (autoResolveService) {
          auto res = resolveService(service);

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
      } else {
        // Clean up any pending operations related to the service
        // Clean up service resolve operations
        std::erase_if(resolveContexts, [&service](const auto& ctx) {
          return ctx->serviceHash == service.getHash();
        });

        // Clean up address resolve operations
        std::erase_if(addressResolveContexts, [&service](const auto& ctx) {
          return ctx->serviceHash == service.getHash();
        });

        // Remove the service from the list of registered services
        registeredServices.erase(service);

        DiscoveryEvent event{
            .type = EventType::Removed,
            .service = service,
        };

        onEvent(event);
      }
    }
  }

  void handleResolveReply(DNSServiceFlags /*flags*/, uint32_t /*interface*/,
                          DNSServiceErrorType dnsSdError,
                          const char* /*fullname*/, const char* hostname,
                          uint16_t port, uint16_t txtLen,
                          const unsigned char* txtRecord,
                          ResolveContext* context) {
    std::scoped_lock lock(serviceMutex);
    // Find the service record by its hash
    auto serviceItr =
        std::find_if(registeredServices.begin(), registeredServices.end(),
                     [context](const ServiceRecord& s) {
                       return s.getHash() == context->serviceHash;
                     });

    if (serviceItr != registeredServices.end()) {
      serviceItr->serviceResolved = true;
      serviceItr->hostname = hostname;
      serviceItr->port = ntohs(port);

      if (!parseTXTRecords(reinterpret_cast<const char*>(txtRecord), txtLen,
                           serviceItr->txtRecords)) {
        // Invalid TXT record
      }

      auto err = translateDnsSdError(dnsSdError);

      if (err) {
        // Resolve failed, notify
        DiscoveryEvent event{
            .type = EventType::ResolveFailure,
            .service = *serviceItr,
            .error = err,
        };
        onEvent(event);
      } else {
        // Resolve succeeded, notify
        DiscoveryEvent event{.type = EventType::Resolved,
                             .service = *serviceItr};
        onEvent(event);

        if (autoResolveAddresses) {
          auto res = resolveAddress(*serviceItr);

          if (!res) {
            // Resolve address failed, notify
            DiscoveryEvent event{
                .type = EventType::AddressResolveFailure,
                .service = *serviceItr,
                .error = res.error(),
            };
            onEvent(event);
          }
        }
      }

    } else {
      // Associated service no longer registered
    }

    // Remove the context from the list of active contexts
    std::erase_if(resolveContexts,
                  [context](const auto& ctx) { return ctx.get() == context; });
  }

  bell::Result<> resolveService(const ServiceRecord& service) override {
    std::scoped_lock lock(serviceMutex);

    // Wrap the C-Style callback
    DNSServiceResolveReply resolveReplyShim =
        [](DNSServiceRef /*ref*/, DNSServiceFlags flags, uint32_t interface,
           DNSServiceErrorType err, const char* fullname, const char* hostname,
           uint16_t port, uint16_t txtLen, const unsigned char* txtRecord,
           void* context) {
          ResolveContext* resolveContext =
              static_cast<ResolveContext*>(context);

          // Simply pass the data to member func
          resolveContext->browserPtr->handleResolveReply(
              flags, interface, err, fullname, hostname, port, txtLen,
              txtRecord, resolveContext);
        };

    // Prepare the resolve context
    auto resolveContext = std::make_unique<ResolveContext>();
    resolveContext->resolveRef = ref;
    resolveContext->browserPtr = this;
    resolveContext->serviceHash = service.getHash();

    DNSServiceErrorType res = DNSServiceResolve(
        &resolveContext->resolveRef, kDNSServiceFlagsShareConnection,
        service.interfaceIndex, service.name.c_str(), service.regType.c_str(),
        service.domain.c_str(), resolveReplyShim, resolveContext.get());

    auto err = translateDnsSdError(res);
    if (err) {
      return tl::make_unexpected(err);
    }

    // Add the resolve context to the cached list
    resolveContexts.push_back(std::move(resolveContext));

    // Successfully started resolution
    return {};
  }

  void handleGetAddrInfoReply(DNSServiceFlags /*flags*/,
                              uint32_t /*interfaceIndex*/,
                              DNSServiceErrorType dnsSdError,
                              const char* /*hostname*/,
                              const struct sockaddr* address, uint32_t /*ttl*/,
                              ResolveContext* context) {
    std::scoped_lock lock(serviceMutex);

    // Find the service record by its hash
    auto serviceItr =
        std::find_if(registeredServices.begin(), registeredServices.end(),
                     [context](const ServiceRecord& s) {
                       return s.getHash() == context->serviceHash;
                     });

    if (serviceItr != registeredServices.end()) {
      auto err = translateDnsSdError(dnsSdError);
      if (err) {
        DiscoveryEvent event{
            .type = EventType::AddressResolveFailure,
            .service = *serviceItr,
            .error = err,
        };
        onEvent(event);
      } else {
        // Parse the resolved address
        auto addr = bell::IpAddress(address);
        if (addr.getType() == bell::IpAddress::Type::IPv4) {
          serviceItr->ipv4 = addr;
        } else if (addr.getType() == bell::IpAddress::Type::IPv6) {
          serviceItr->ipv6 = addr;
        }

        DiscoveryEvent event{
            .type = EventType::AddressResolved,
            .service = *serviceItr,
            .error = err,
        };
        onEvent(event);
      }
    }
  }

  bell::Result<> resolveAddress(const ServiceRecord& service) override {
    std::scoped_lock lock(serviceMutex);

    // Wrap C-style callback
    DNSServiceGetAddrInfoReply resolveAddrInfoReply =
        [](DNSServiceRef /*ref*/, DNSServiceFlags flags, uint32_t interface,
           DNSServiceErrorType err, const char* hostname,
           const struct sockaddr* address, uint32_t ttl, void* context) {
          auto* resolveContext = static_cast<ResolveContext*>(context);

          // Pass data to member function
          resolveContext->browserPtr->handleGetAddrInfoReply(
              flags, interface, err, hostname, address, ttl, resolveContext);
        };

    // Prepare the resolve context
    auto resolveContext = std::make_unique<ResolveContext>();
    resolveContext->resolveRef = ref;
    resolveContext->browserPtr = this;
    resolveContext->serviceHash = service.getHash();

    DNSServiceErrorType res = DNSServiceGetAddrInfo(
        &resolveContext->resolveRef, kDNSServiceFlagsShareConnection,
        service.interfaceIndex,
        resolveIpv6 ? kDNSServiceProtocol_IPv4 | kDNSServiceProtocol_IPv6
                    : kDNSServiceProtocol_IPv4,
        service.hostname.c_str(), resolveAddrInfoReply, resolveContext.get());

    auto err = translateDnsSdError(res);
    if (err) {
      return tl::make_unexpected(err);
    }

    // Add the resolve context to the cached list
    addressResolveContexts.push_back(std::move(resolveContext));

    // Successfully started resolution
    return {};
  }

  void stopDiscovery() override {
    std::scoped_lock lock(serviceMutex);

    if (wrappedDnsSdSocket) {
      // Unregister the dns-sd socket from the event poll
      socketPoll->unregisterSocket(wrappedDnsSdSocket);

      // Take the FD, so destructor of bell::UDPSocket does not close the FD automatically
      (void)wrappedDnsSdSocket->takeFd();

      // Unregister dns-sd event
      DNSServiceRefDeallocate(ref);
    }
  }

 private:
  // Pointer to the event callback
  DiscoveryEventCallback onEvent = {};

  // Socket poll pointer, used to register and unregister the dns-sd socket
  std::shared_ptr<bell::SocketPollListener> socketPoll;

  // Wrapper for the dns-sd socket
  std::shared_ptr<bell::UDPSocket> wrappedDnsSdSocket = nullptr;

  // DNS service reference
  DNSServiceRef ref = nullptr;
  DNSServiceRef browseRef = nullptr;

  // Browser options
  bool autoResolveService = true;
  bool autoResolveAddresses = true;
  bool resolveIpv6 = true;

  // Set of registered services
  std::set<ServiceRecord> registeredServices;

  // Access mutex
  std::recursive_mutex serviceMutex;

  // Holds currently active resolve contexts
  std::vector<std::unique_ptr<ResolveContext>> resolveContexts;

  // Holds currently active address resolve contexts
  std::vector<std::unique_ptr<ResolveContext>> addressResolveContexts;
};

class BonjourAdvertiser : public Advertiser {
 public:
  BonjourAdvertiser(std::shared_ptr<bell::SocketPollListener> socketPoll)
      : socketPoll(std::move(socketPoll)) {}

  ~BonjourAdvertiser() override { stopAdvertising(); }

  // Remoce the copy constructor and assignment operator
  BonjourAdvertiser(const BonjourAdvertiser&) = delete;
  BonjourAdvertiser& operator=(const BonjourAdvertiser&) = delete;

  bell::Result<> advertise(
      const std::string& serviceName, const std::string& serviceType,
      const std::string& serviceDomain, const std::string& serviceHost,
      uint16_t port,
      const std::unordered_map<std::string, std::string>& txtRecords,
      int interfaceIndex) {

    TXTRecordRef txtRecord;

    TXTRecordCreate(&txtRecord, 0, nullptr);
    for (const auto& data : txtRecords) {
      // Convert the key-value pair to a TXT record
      TXTRecordSetValue(&txtRecord, data.first.c_str(), data.second.size(),
                        data.second.c_str());
    }

    auto res = DNSServiceRegister(
        &ref, 0, interfaceIndex, serviceName.c_str(), serviceType.c_str(),
        serviceDomain.empty() ? nullptr : serviceDomain.c_str(),
        serviceHost.empty() ? nullptr : serviceHost.c_str(), htons(port),
        TXTRecordGetLength(&txtRecord), TXTRecordGetBytesPtr(&txtRecord),
        nullptr, nullptr);

    // Free the TXT record
    TXTRecordDeallocate(&txtRecord);

    auto err = translateDnsSdError(res);

    if (err) {
      return tl::make_unexpected(err);
    }

    // Get a socket file descriptor for the browse socket
    dnssd_sock_t sock = DNSServiceRefSockFD(ref);
    if (sock == -1) {
      return bell::make_unexpected_errc(std::errc::bad_file_descriptor);
    }

    // Wrap the socket in a UDP socket handle, so we can use it with poll
    wrappedDnsSdSocket = std::make_shared<bell::UDPSocket>(sock);

    // Register the socket with the poller
    socketPoll->registerSocket(
        wrappedDnsSdSocket, bell::PollEvent::Readable,
        [this](auto& /**/) { DNSServiceProcessResult(ref); });

    return {};
  }

  void stopAdvertising() override {
    if (ref) {
      // Unregister the dns-sd socket from the event poll
      socketPoll->unregisterSocket(wrappedDnsSdSocket);

      // Take the FD, so destructor of bell::UDPSocket does not close the FD automatically
      (void)wrappedDnsSdSocket->takeFd();

      // Unregister dns-sd event
      DNSServiceRefDeallocate(ref);
    }
  }

 private:
  std::shared_ptr<bell::SocketPollListener> socketPoll;
  std::shared_ptr<bell::UDPSocket> wrappedDnsSdSocket = nullptr;
  DNSServiceRef ref = nullptr;
};

class BonjourManager : public Manager, public bell::Task {
 public:
  BonjourManager()
      : bell::Task("bonjour_manager", 0, 0, bell::TaskCore::CoreAny) {
    socketPoll = std::make_shared<bell::SocketPollListener>();

    startTask();
  }

  bell::Result<std::unique_ptr<Browser>> browse(
      const std::string& serviceType, const std::string& serviceDomain,
      int interfaceIndex, const Browser::DiscoveryEventCallback& onEvent,
      bool autoResolveService = true, bool autoResolveAddresses = true,
      bool resolveIPv6 = true) override {
    auto browser = std::make_unique<BonjourBrowser>(socketPoll);
    auto res =
        browser->browse(serviceType, serviceDomain, interfaceIndex, onEvent,
                        autoResolveService, autoResolveAddresses, resolveIPv6);
    if (!res) {
      // Return an error code indicating service discovery failure
      return tl::make_unexpected(MdnsErrc::service_discovery_failed);
    }

    return {std::move(browser)};
  }

  bell::Result<std::unique_ptr<Advertiser>> advertise(
      const std::string& serviceName, const std::string& serviceType,
      const std::string& serviceDomain, const std::string& serviceHost,
      uint16_t port,
      const std::unordered_map<std::string, std::string>& txtRecords,
      int interfaceIndex) override {
    auto advertiser = std::make_unique<BonjourAdvertiser>(socketPoll);

    auto res =
        advertiser->advertise(serviceName, serviceType, serviceDomain,
                              serviceHost, port, txtRecords, interfaceIndex);

    if (!res) {
      return tl::make_unexpected(res.error());
    }

    return advertiser;
  }

  // Returns a pointer to the singleton instance
  static BonjourManager* getDefaultManager() {
    static BonjourManager* defaultManagerInstance = nullptr;

    if (!defaultManagerInstance) {
      defaultManagerInstance = new BonjourManager();
    }
    return defaultManagerInstance;
  }

 private:
  std::shared_ptr<bell::SocketPollListener> socketPoll;

  void taskLoop() override {
    // Run the socket poll
    socketPoll->poll();
  }
};

Manager* bell::mdns::getDefaultManager() {
  return BonjourManager::getDefaultManager();
}
