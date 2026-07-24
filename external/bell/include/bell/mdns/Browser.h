#pragma once

// Standard includes
#include <functional>
#include <unordered_map>

// Bell includes
#include "bell/Result.h"
#include "bell/net/IpAddress.h"

namespace bell::mdns {
/**
 * @brief Base class for an active mDNS service discovery handle.
 */
class Browser {
 public:
  virtual ~Browser() = default;
  Browser() = default;

  // Disable copy operations, as we are managing instances via std::unique_ptr
  Browser(const Browser&) = delete;
  Browser& operator=(const Browser&) = delete;

  // Structure representing an mDNS service record
  struct ServiceRecord {
    // Basic service record information
    std::string name;
    std::string regType;
    std::string domain;
    uint32_t interfaceIndex;

    // Resolved service information
    mutable bool serviceResolved = false;
    mutable std::string hostname{};
    mutable uint16_t port = 0;
    mutable std::unordered_map<std::string, std::string> txtRecords{};

    // Resolved address information
    mutable std::optional<net::IpAddress> ipv4{};
    mutable std::optional<net::IpAddress> ipv6{};

    // Equality operator for Record
    bool operator==(const ServiceRecord& other) const {
      return name == other.name && regType == other.regType &&
             domain == other.domain && interfaceIndex == other.interfaceIndex;
    }

    // Returns an unique hash value for the ServiceRecord
    uint32_t getHash() const {
      return std::hash<std::string>{}(name) ^
             std::hash<std::string>{}(regType) ^
             std::hash<std::string>{}(domain) ^
             std::hash<uint32_t>{}(interfaceIndex);
    }

    // Overload comparison operator for ServiceRecord
    bool operator<(const ServiceRecord& other) const {
      return name < other.name ||
             (name == other.name && regType < other.regType);
    }
  };

  // Event type enumeration
  enum class EventType {
    Added,                  //< Service has been added
    Removed,                //< Service has been removed
    Resolved,               //< Service has been resolved
    ResolveFailure,         //< Service resolution has failed
    AddressResolved,        //< Address has been resolved
    AddressResolveFailure,  //< Address resolution has failed
    BrowseFailure,          //< Browse failure
  };

  /**
   * @brief Query a resolve of the provided service record.
   */
  virtual bell::Result<> resolveService(const ServiceRecord& service) = 0;

  /**
   * @brief Query a resolve of the provided service's address.
   */
  virtual bell::Result<> resolveAddress(const ServiceRecord& service) = 0;

  /**
   * @brief Stop the discovery process. Automatically called on destruction.
   */
  virtual void stopDiscovery() = 0;

  // Structure representing a discovery event
  struct DiscoveryEvent {
    EventType type{};       //< Event type
    ServiceRecord service;  // Service record associated with the event
    std::error_code error;  // Error code associated with the event, if any
  };

  // Callback type for discovery events
  using DiscoveryEventCallback = std::function<void(const DiscoveryEvent&)>;
};
}  // namespace bell::mdns

namespace bell {
using MDNSServiceRecord = bell::mdns::Browser::ServiceRecord;
using MDNSDiscoveryEvent = bell::mdns::Browser::DiscoveryEvent;
using MDNSDiscoveryEventType = bell::mdns::Browser::EventType;
}  // namespace bell
