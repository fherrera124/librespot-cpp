#pragma once

namespace bell::mdns {
/**
 * @brief Abstract base class for a handle representing a registered mDNS service.
 */
class Advertiser {
 public:
  virtual ~Advertiser() = default;
  Advertiser() = default;

  // Disable copy operations, as we are managing instances via std::unique_ptr
  Advertiser(const Advertiser&) = delete;
  Advertiser& operator=(const Advertiser&) = delete;

  /**
   * @brief Stop advertising the service. Automatically called on destruction.
   */
  virtual void stopAdvertising() = 0;
};
}  // namespace bell::mdns
