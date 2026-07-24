#pragma once

// Standard includes
#include <memory>
#include <unordered_map>

// Bell includes
#include "bell/Result.h"
#include "bell/mdns/Advertiser.h"
#include "bell/mdns/Browser.h"

namespace bell::mdns {
/**
 * @brief Abstract base class for managing mDNS service discovery and registration. Implemented per platform.
 */
class Manager {
 public:
  Manager() = default;
  virtual ~Manager() = default;

  // Disable copy operations, as we are managing instances via std::unique_ptr
  Manager(const Manager&) = delete;
  Manager& operator=(const Manager&) = delete;

  /**
   * @brief Start browsing for mDNS services.
   *
   * @param serviceType Type of service to browse for (e.g., "_http._tcp").
   * @param serviceDomain Domain to browse, or empty for default domain.
   * @param interfaceIndex Interface index to use for browsing (0 for all interfaces).
   * @param onEvent Callback function to handle discovery events.
   * @param autoResolveService Automatically resolve discovered services.
   * @param autoResolveAddresses Automatically resolve service's addresses.
   * @param resolveIPv6 Whether to resolve IPv6 addresses.
   * @return Result<std::unique_ptr<Browser>> Result containing a unique pointer to the Browser instance.
   */
  virtual bell::Result<std::unique_ptr<Browser>> browse(
      const std::string& serviceType, const std::string& regDomain,
      int interfaceIndex, const Browser::DiscoveryEventCallback& onEvent,
      bool autoResolveService = true, bool autoResolveAddresses = true,
      bool resolveIPv6 = true) = 0;
  /**
   * @brief Advertise a service using mDNS.
   *
   * @param serviceName Name of the service to advertise.
   * @param serviceType Service type (e.g., "_http._tcp").
   * @param serviceDomain Domain to advertise the service in, or empty for default domain.
   * @param serviceHost Hostname of the service, or empty for default hostname.
   * @param port Port number of the service.
   * @param txtRecords Optional TXT records to include with the service advertisement.
   * @param interfaceIndex Interface index to use for advertising (0 for all interfaces).
   * @return Result<std::unique_ptr<Advertiser>> Result containing a unique pointer to the advertiser handle.
   */
  virtual bell::Result<std::unique_ptr<Advertiser>> advertise(
      const std::string& serviceName, const std::string& serviceType,
      const std::string& serviceDomain, const std::string& serviceHost,
      uint16_t port,
      const std::unordered_map<std::string, std::string>& txtRecords = {},
      int interfaceIndex = 0) = 0;
};

// Returns a default mDNS manager instance, which is a singleton.
Manager* getDefaultManager();
}  // namespace bell::mdns

namespace bell {
using MDNSManager = bell::mdns::Manager;
}  // namespace bell
