#include <doctest/doctest.h>
#include <unistd.h>

#include <atomic>

#include "bell/mdns/Browser.h"
#include "bell/mdns/Manager.h"
#include "bell/utils/Utils.h"

TEST_CASE("bell::mdns tests") {
  std::string serviceName = "mdns-discovery-test";
  std::atomic<bool> serviceAdded = false;
  std::atomic<bool> serviceAddrResolved = false;
  std::atomic<bool> serviceRemoved = false;

  auto browser = bell::mdns::getDefaultManager()->browse(
      "_bell._tcp", "", 0,
      [&serviceName, &serviceAdded, &serviceAddrResolved,
       &serviceRemoved](const bell::MDNSDiscoveryEvent& discoveryEvent) {
        // Save up events for the requested service
        if (discoveryEvent.service.name == serviceName) {
          switch (discoveryEvent.type) {
            case bell::MDNSDiscoveryEventType::Added:
              serviceAdded = true;
              break;
            case bell::MDNSDiscoveryEventType::Resolved:
              serviceAddrResolved = true;
              break;
            case bell::MDNSDiscoveryEventType::Removed:
              serviceRemoved = true;
              break;
            default:
              break;
          }
        }
      });

  SUBCASE("Properly registers and unregisters a service") {
    REQUIRE_FALSE(serviceAdded);
    REQUIRE_FALSE(serviceAddrResolved);
    REQUIRE_FALSE(serviceRemoved);

    {
      auto service = bell::mdns::getDefaultManager()->advertise(
          serviceName, "_bell._tcp", "", "", 1234,
          {{"dupa", "value"}, {"dupa2", "value2"}});
      bell::utils::sleepMs(1000);

      // Service should be added and addr resolved by now
      REQUIRE(serviceAdded == true);
      REQUIRE(serviceAddrResolved == true);

      // Service should not be removed yet
      REQUIRE_FALSE(serviceRemoved);
    }
    bell::utils::sleepMs(2000);

    // Service should be removed by now
    REQUIRE(serviceRemoved);
  }
}
