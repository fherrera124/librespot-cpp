#pragma once

#ifndef BELL_DISABLE_MQTT

// Standard includes
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

// Bell includes
#include "bell/net/Socket.h"

// Library includes
#include "mqtt.h"

namespace bell::net {
/// MQTTClient is a thin wrapper around the MQTT client library.
class MQTTClient {
 public:
  MQTTClient() = default;
  ~MQTTClient();

  enum class QOS {
    AT_MOST_ONCE = MQTT_PUBLISH_QOS_0,
    AT_LEAST_ONCE = MQTT_PUBLISH_QOS_1,
    EXACTLY_ONCE = MQTT_PUBLISH_QOS_2
  };

  /**
   * @brief Handler for when a message is published.
   */
  using PublishHandler =
      std::function<void(std::string_view message, int packetId, QOS qos)>;

  /**
   * @brief Connect to an MQTT broker.
   *
   * @param host The host to connect to.
   * @param port The port to connect to.
   * @param username The username to authenticate with.
   * @param password The password to authenticate with.
   * @param timeoutMs The timeout for the connection, in milliseconds.
   * @param secure Whether to use a secure connection (TLS).
   * @param keepAlive The keep-alive time in seconds.
   */
  void connect(const std::string& host, uint16_t port,
               const std::string& username = "",
               const std::string& password = "", int timeoutMs = 0,
               bool secure = false, int keepAlive = 400);

  /**
   * @brief Disconnect from the MQTT broker.
   */
  void disconnect();

  /**
   * @brief Synchronize with the MQTT broker.
   */
  void sync();

  /**
   * @brief Publish a message to a topic.
   *
   * @param topic The topic to publish to.
   * @param message The message to publish.
   * @param qos The quality of service to publish with.
   */
  void publish(const std::string& topic, const std::string& message,
               QOS qos = QOS::AT_MOST_ONCE);

  /**
   * @brief Subscribe to a topic.
   *
   * @param topic The topic to subscribe to.
   * @param qos The quality of service to subscribe with.
   * @param onPublish The callback to call when a message is published.
   */
  void subscribe(const std::string& topic, QOS qos = QOS::AT_MOST_ONCE,
                 const PublishHandler& handler = {});

  /**
   * @brief Unsubscribe from a topic.
   *
   * @param topic The topic to unsubscribe from.
   */
  void unsubscribe(const std::string& topic);

  /**
   * @brief Check if the MQTT client is connected.
   *
   * @return True if the MQTT client is connected, false otherwise.
   */
  bool isConnected() const;

  // Directly mapped from mqtt client's publish_callback field
  void cPublishCallback(struct mqtt_response_publish* publishResponse);

 private:
  std::unique_ptr<net::Socket> socket;
  std::recursive_mutex accessMutex;

  // Holds the callbacks for each subscribed topic
  // Use std::less<> so we can use string_view for lookup
  std::map<std::string, PublishHandler, std::less<>> topicCallbacks;

  // mqtt lib internals
  struct mqtt_client client {};
  std::array<uint8_t, 2048> sendbuf{};
  std::array<uint8_t, 1024> recvbuf{};
  mqtt_callbacks_context callbacksContext{};
};
}  // namespace bell::net

namespace bell {
using MQTTClient = net::MQTTClient;
}

#endif  // BELL_DISABLE_MQTT
