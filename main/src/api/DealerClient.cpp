#include "api/DealerClient.h"

// Library includes
#include <tao/json.hpp>

#include "SessionContext.h"
#include "bell/Result.h"
#include "tl/expected.hpp"

using namespace cspot;

DealerClient::DealerClient(
    std::shared_ptr<cspot::SessionContext> sessionContext)
    : sessionContext(std::move(sessionContext)) {
  // Bind websocket client handlers
  wsClient.set_open_handler(websocketpp::lib::bind(
      &DealerClient::onWSOpen, this, websocketpp::lib::placeholders::_1));
  wsClient.set_close_handler(websocketpp::lib::bind(
      &DealerClient::onWSClose, this, websocketpp::lib::placeholders::_1));
  wsClient.set_message_handler(websocketpp::lib::bind(
      &DealerClient::onWSMessage, this, websocketpp::lib::placeholders::_1,
      websocketpp::lib::placeholders::_2));
  wsClient.set_fail_handler(websocketpp::lib::bind(
      &DealerClient::onWSFail, this, websocketpp::lib::placeholders::_1));
  wsClient.set_write_handler(websocketpp::lib::bind(
      &DealerClient::wsWriteHandler, this, websocketpp::lib::placeholders::_1,
      websocketpp::lib::placeholders::_2, websocketpp::lib::placeholders::_3));
}

bell::Result<> DealerClient::connect() {
  connectionReady = false;

  auto accessKey = sessionContext->credentialsResolver->getAccessKey();
  if (!accessKey) {
    BELL_LOG(error, LOG_TAG, "Could not get access key: {}", accessKey.error());
    return tl::make_unexpected(accessKey.error());
  }
  auto dealerAddress = sessionContext->credentialsResolver->getApAddress(
      CredentialsResolver::AddressType::Dealer);
  if (!dealerAddress) {
    BELL_LOG(error, LOG_TAG, "Could not get dealer address: {}",
             dealerAddress.error());
    return tl::make_unexpected(dealerAddress.error());
  }

  std::string dealerAddressStr = *dealerAddress;

  std::string connectionUrl =
      fmt::format("{}/?access_token={}", dealerAddressStr, *accessKey);

  // Get everything before ":" in the dealer address
  std::string::size_type pos = dealerAddressStr.find(':');
  if (pos == std::string::npos) {
    BELL_LOG(error, LOG_TAG, "Dealer address does not contain port");
    return bell::make_unexpected_errc(std::errc::invalid_argument);
  }
  // Get the host part of the dealer address
  std::string dealerHost = dealerAddressStr.substr(0, pos);

  socket = std::make_shared<bell::net::TLSSocket>();
  auto connectRes = socket->connect(dealerHost, 443, 3000);
  if (!connectRes) {
    BELL_LOG(error, LOG_TAG, "Dealer connect error: {}", connectRes.error());
    return tl::make_unexpected(connectRes.error());
  }

  // Mark transport as secure
  wsClient.set_secure(true);
  wsClient.clear_access_channels(websocketpp::log::alevel::none);

  // Register readable listener
  sessionContext->socketPoll.registerSocket(
      socket, bell::PollEvent::Readable, [this](auto& sock) {
        if (wsConnection) {
          auto res = sock.read(inputBuffer.data(), inputBuffer.size());
          if (!res || *res == 0) {
            BELL_LOG(error, LOG_TAG, "Socket read error: {}",
                     res.error().message());
          } else {
            std::scoped_lock lock(accessMutex);

            // Read incoming bytes
            if (*res > 0 && wsConnection) {
              this->wsConnection->read_all(
                  reinterpret_cast<char*>(inputBuffer.data()), *res);
            }
          }
        }
      });

  // Register writeable / connected listener
  sessionContext->socketPoll.registerSocket(
      socket, bell::PollEvent::Writeable,
      [this, connectionUrl](auto& /*sock*/) {
        if (!wsConnection) {
          websocketpp::lib::error_code ec;
          wsConnection = wsClient.get_connection("wss://" + connectionUrl, ec);

          if (ec) {
            BELL_LOG(error, LOG_TAG, "Websocket connection error: {}",
                     ec.message());
          } else {
            auto err = socket->lastError();

            if (err) {
              BELL_LOG(error, LOG_TAG, "Socket error: {}", err.message());
              //   // throw std::runtime_error("Sock connect failed");
            } else {
              wsClient.connect(wsConnection);
            }
          }
        }

        // We are connected, unregister the writeable event
        sessionContext->socketPoll.unregisterSocket(socket,
                                                    bell::PollEvent::Writeable);
      });

  return {};
}

void DealerClient::onWSFail(websocketpp::connection_hdl conn) {  // NOLINT
  connectionReady = true;

  BELL_LOG(error, LOG_TAG, "Dealer connection failed {}");
}

void DealerClient::onWSOpen(websocketpp::connection_hdl /*conn*/) {  // NOLINT
  BELL_LOG(info, LOG_TAG, "Dealer connection success");

  connectionReady = true;
}

void DealerClient::onWSClose(websocketpp::connection_hdl /*conn*/) {

  BELL_LOG(info, LOG_TAG, "Websocket connection closed");
}

std::error_code DealerClient::wsWriteHandler(websocketpp::connection_hdl hdl,
                                             char const* data, size_t size) {
  if (socket->isValid()) {
    auto result = socket->write(reinterpret_cast<const uint8_t*>(data), size);

    if (!result || *result != size) {
      BELL_LOG(error, LOG_TAG, "Could not write to socket");
      return websocketpp::error::make_error_code(
          websocketpp::error::bad_connection);
    }

  } else {
    return websocketpp::error::make_error_code(
        websocketpp::error::invalid_state);
  }

  return {};
}

void DealerClient::onWSMessage(websocketpp::connection_hdl conn,
                               WSClient::message_ptr msg) {  // NOLINT

  if (msg->get_opcode() != websocketpp::frame::opcode::TEXT) {
    BELL_LOG(debug, LOG_TAG, "Did not receive a text message, ignoring");
    return;
  }

  tao::json::value jsonMessage;

  try {
    jsonMessage = tao::json::from_string(msg->get_payload());
  } catch (const std::exception& e) {
    BELL_LOG(error, LOG_TAG, "Could not parse JSON message: {}", e.what());
    return;
  }

  if (jsonMessage.is_object() && jsonMessage["type"].is_string()) {
    std::string type = jsonMessage["type"].get_string();

    if (type == "message") {
      sessionContext->eventLoop->post(EventLoop::EventType::DEALER_MESSAGE,
                                      jsonMessage);
    } else if (type == "request") {
      sessionContext->eventLoop->post(EventLoop::EventType::DEALER_REQUEST,
                                      jsonMessage);
    } else {
      BELL_LOG(debug, LOG_TAG, "Unknown message type: {}", type);
    }
  }
}

bell::Result<> DealerClient::replyToRequest(bool success,
                                            const std::string& requestKey) {
  std::scoped_lock lock(accessMutex);

  tao::json::value response = {
      {"type", "reply"},
      {"key", requestKey},
      {"payload", {{"success", success}}},
  };

  std::string responseStr = tao::json::to_string(response);
  auto err = wsConnection->send(responseStr, websocketpp::frame::opcode::TEXT);

  if (err) {
    BELL_LOG(error, LOG_TAG, "Error sending response: {}", err.message());
    return tl::make_unexpected(err);
  }

  return {};
}

void DealerClient::doHousekeeping() {
  // TODO:
}
