#include "api/DealerClient.h"

// Library includes
#include <tao/json.hpp>

#include "bell/Result.h"
#include "bell/net/SocketPollListener.h"
#include "events/EventLoop.h"
#include "tl/expected.hpp"

using namespace cspot;

namespace {
const auto pingInterval = std::chrono::seconds(30);
// const auto pongTimeout = std::chrono::seconds(10);
}  // namespace

DealerClient::DealerClient(std::shared_ptr<cspot::EventLoop> eventLoop)
    : eventLoop(std::move(eventLoop)) {
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

bell::Result<> DealerClient::connect(
    const std::string& dealerAddress, const std::string& accessKey,
    const std::shared_ptr<bell::SocketPollListener>& socketPoll) {
  connectionReady = false;
  this->socketPoll = socketPoll;

  std::string connectionUrl =
      fmt::format("{}/?access_token={}", dealerAddress, accessKey);

  // Get everything before ":" in the dealer address
  std::string::size_type pos = dealerAddress.find(':');
  if (pos == std::string::npos) {
    BELL_LOG(error, LOG_TAG, "Dealer address does not contain port");
    return bell::make_unexpected_errc(std::errc::invalid_argument);
  }
  // Get the host part of the dealer address
  std::string dealerHost = dealerAddress.substr(0, pos);

  socket = std::make_shared<bell::net::TLSSocket>();
  auto connectRes = socket->connect(dealerHost, 443, 3000);
  if (!connectRes) {
    BELL_LOG(error, LOG_TAG, "Dealer connect error: {}", connectRes.error());
    return tl::make_unexpected(connectRes.error());
  }

  // Mark transport as secure
  wsClient.set_secure(true);
  // alevel::none is 0 - clearing it is a no-op, so every access channel
  // (frame_header/frame_payload byte dumps included) stayed on and spammed
  // the log on every single WS message. alevel::all is the actual "disable
  // everything" mask.
  wsClient.clear_access_channels(websocketpp::log::alevel::all);

  // Register readable listener
  socketPoll->registerSocket(
      socket, bell::PollEvent::Readable, [this](auto& sock) {
        if (wsConnection) {
          auto res = sock.read(inputBuffer.data(), inputBuffer.size());
          if (!res) {
            // res holds a real error here - safe to call .error(). A
            // successful read of 0 bytes (peer closed) falls through to
            // the branch below instead: res.error() on an expected that
            // actually holds a value is UB, and was a real crash (a
            // LoadProhibited panic reading a garbage std::error_code
            // category vtable) reproduced on real hardware when the
            // dealer server closed the WebSocket connection.
            BELL_LOG(error, LOG_TAG, "Socket read error: {}",
                     res.error().message());
            // A dead socket stays "readable" forever (reading it just
            // keeps failing), so without this the poller re-invokes this
            // same callback as fast as it can - a real hardware hang
            // reproduced as a task watchdog reset (the hot loop starves
            // the idle task). Stop being polled once the socket's gone.
            connectionReady = false;
            this->socketPoll->unregisterSocket(this->socket,
                                               bell::PollEvent::All);
          } else if (*res == 0) {
            BELL_LOG(info, LOG_TAG, "Dealer socket closed by peer");
            connectionReady = false;
            this->socketPoll->unregisterSocket(this->socket,
                                               bell::PollEvent::All);
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
  socketPoll->registerSocket(
      socket, bell::PollEvent::Writeable,
      [this, connectionUrl, socketPoll](auto& /*sock*/) {
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
        socketPoll->unregisterSocket(socket, bell::PollEvent::Writeable);
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
  // Stops doHousekeeping()'s ping (and replyToRequest(), if a stray
  // dealer request ever arrives after this) from calling send() on a
  // connection websocketpp itself already knows is dead - was spamming
  // "Error sending response: invalid state" every 30s after a close.
  connectionReady = false;
}

std::error_code DealerClient::wsWriteHandler(websocketpp::connection_hdl hdl,
                                             char const* data, size_t size) {
  if (socket->isValid()) {
    auto result = socket->write(reinterpret_cast<const std::byte*>(data), size);

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
      eventLoop->post(EventLoop::EventType::DEALER_MESSAGE, jsonMessage);
    } else if (type == "request") {
      eventLoop->post(EventLoop::EventType::DEALER_REQUEST, jsonMessage);
    } else if (type == "pong") {
      lastPongTime = std::chrono::system_clock::now();
      BELL_LOG(debug, LOG_TAG, "Received pong");
    } else {
      BELL_LOG(debug, LOG_TAG, "Unknown message type: {}", type);
    }
  }
}

bell::Result<> DealerClient::replyToRequest(bool success,
                                            const std::string& requestKey) {
  std::scoped_lock lock(accessMutex);

  if (!connectionReady) {
    return bell::make_unexpected_errc(std::errc::not_connected);
  }

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
  if (std::chrono::system_clock::now() >= (lastPingTime + pingInterval)) {
    std::scoped_lock lock(accessMutex);

    if (wsConnection && connectionReady) {
      tao::json::value pingMsg = {{"type", "ping"}};
      std::string responseStr = tao::json::to_string(pingMsg);
      auto err =
          wsConnection->send(responseStr, websocketpp::frame::opcode::TEXT);

      if (err) {
        BELL_LOG(error, LOG_TAG, "Error sending response: {}", err.message());
      } else {
        lastPingTime = std::chrono::system_clock::now();
      }
    }
  }
}
