#include "bell/net/MQTTClient.h"

// Bell includes
#include "bell/Logger.h"
#include "bell/net/TCPSocket.h"
#include "bell/net/TLSSocket.h"
#include "mqtt_pal.h"

using namespace bell;

namespace {
void publishCallbackShim(void** state,
                         struct mqtt_response_publish* publishResponse) {
  auto* client = static_cast<net::MQTTClient*>(*state);
  client->cPublishCallback(publishResponse);
}

// Read callback for the MQTT lib
ssize_t mqttPalRead(void* context, uint8_t* buf, size_t len) {
  auto* socket = static_cast<net::Socket*>(context);
  auto res = socket->read(reinterpret_cast<std::byte*>(buf), len);

  if (!res) {
    BELL_LOG(error, "mqtt_pal", "Failed to read from socket: {}", res.error());
    return -1;
  }

  return static_cast<ssize_t>(*res);
}

// Write callback for the MQTT lib
ssize_t mqttPalWrite(void* context, const uint8_t* buf, size_t len) {
  auto* socket = static_cast<net::Socket*>(context);
  auto res = socket->write(reinterpret_cast<const std::byte*>(buf), len);
  if (!res) {
    BELL_LOG(error, "mqtt_pal", "Failed to write to socket: {}", res.error());
    return -1;
  }

  return static_cast<ssize_t>(*res);
}
}  // namespace

net::MQTTClient::~MQTTClient() {
  if (isConnected()) {
    mqtt_disconnect(&client);
    socket->close();

    callbacksContext.user_context = nullptr;
  }
}

void net::MQTTClient::connect(const std::string& host, uint16_t port,
                              const std::string& username,
                              const std::string& password, int timeoutMs,
                              bool secure, int keepAlive) {
  // Connect to the broker
  if (secure) {
    auto tlsSocket = std::make_unique<net::TLSSocket>();
    (void)tlsSocket->connect(host, port, timeoutMs);
    socket = std::move(tlsSocket);
  } else {
    auto tcpSocket = std::make_unique<net::TCPSocket>();
    (void)tcpSocket->connect(host, port, timeoutMs);
    socket = std::move(tcpSocket);
  }

  if (!socket->isValid()) {
    throw std::runtime_error("Failed to connect to MQTT broker");
  }

  // Make the socket non-blocking
  (void)socket->setBlocking(false);

  // Pass pointer to this object to the publish callback
  client.publish_response_callback_state = this;
  callbacksContext.user_context = socket.get();
  callbacksContext.read_cb = mqttPalRead;
  callbacksContext.write_cb = mqttPalWrite;

  if (mqtt_init(&client, &callbacksContext, sendbuf.data(), sendbuf.size(),
                recvbuf.data(), recvbuf.size(),
                publishCallbackShim) != MQTT_OK) {
    socket->close();
    throw std::runtime_error("Cannot initialize MQTT structure");
  }

  const char* clientId = nullptr;
  uint8_t connect_flags = MQTT_CONNECT_CLEAN_SESSION;
  if (mqtt_connect(&client, clientId, nullptr, nullptr, 0,
                   username.empty() ? nullptr : username.c_str(),
                   password.empty() ? nullptr : password.c_str(), connect_flags,
                   keepAlive) != MQTT_OK) {
    socket->close();
    throw std::runtime_error("MQTT connect failed");
  }
}

void MQTTClient::sync() {
  if (!isConnected()) {
    throw std::runtime_error("MQTT client is not connected");
  }

  mqtt_sync(&client);
}

void MQTTClient::publish(const std::string& topic, const std::string& message,
                         QOS qos) {
  if (!isConnected()) {
    throw std::runtime_error("MQTT client is not connected");
  }

  int err = mqtt_publish(&client, topic.c_str(), message.c_str(),
                         message.size(), static_cast<uint8_t>(qos));

  if (err != MQTT_OK) {
    BELL_LOG(error, "mqtt", "MQTT publish failed: {}", err);
    throw std::runtime_error("MQTT publish failed");
  }
}

void MQTTClient::subscribe(const std::string& topic, QOS qos,
                           const PublishHandler& handler) {
  std::scoped_lock lock(accessMutex);
  if (!isConnected()) {
    throw std::runtime_error("MQTT client is not connected");
  }

  // Insert a new handler for the topic
  if (handler) {
    topicCallbacks.insert_or_assign(topic, handler);
  }

  mqtt_subscribe(&client, topic.c_str(), (uint8_t)qos);
}

void MQTTClient::unsubscribe(const std::string& topic) {
  std::scoped_lock lock(accessMutex);
  if (!isConnected()) {
    throw std::runtime_error("MQTT client is not connected");
  }

  mqtt_unsubscribe(&client, topic.c_str());

  // Remove the handler for the topic
  topicCallbacks.erase(topic);
}

void MQTTClient::disconnect() {
  std::scoped_lock lock(accessMutex);
  if (!isConnected()) {
    throw std::runtime_error("MQTT client is not connected");
  }

  mqtt_disconnect(&client);
  socket->close();

  // Clear the topic callbacks
  topicCallbacks.clear();
}

bool net::MQTTClient::isConnected() const {
  return socket && socket->isValid();
}

void MQTTClient::cPublishCallback(
    struct mqtt_response_publish* publishResponse) {
  std::scoped_lock lock(accessMutex);

  std::string_view topic{
      reinterpret_cast<const char*>(publishResponse->topic_name),
      publishResponse->topic_name_size};

  const auto& handler = topicCallbacks.find(topic);

  if (handler != topicCallbacks.end()) {
    // Call the handler
    handler->second(std::string_view(reinterpret_cast<const char*>(
                                         publishResponse->application_message),
                                     publishResponse->application_message_size),
                    publishResponse->packet_id,
                    static_cast<QOS>(publishResponse->qos_level));
  }
}
