#pragma once

#include <array>
#include <memory>

// Websocketpp includes
#include <websocketpp/client.hpp>
#include <websocketpp/config/core.hpp>

// Tao JSON
#include <tao/json/value.hpp>

// Bell includes
#include <bell/Logger.h>
#include <bell/Result.h>
#include <bell/net/SocketPollListener.h>
#include <bell/net/TCPSocket.h>
#include <bell/net/TLSSocket.h>

// Own includes
#include "events/EventLoop.h"

namespace cspot {
class DealerClient {
 public:
  DealerClient(std::shared_ptr<cspot::EventLoop> eventLoop);

  bell::Result<> connect(
      const std::string& dealerAddress, const std::string& accessKey,
      const std::shared_ptr<bell::SocketPollListener>& socketPoll);

  bell::Result<> replyToRequest(bool success, const std::string& requestKey);

  // Used for keep alive messages
  void doHousekeeping();

 private:
  const char* LOG_TAG = "DealerClient";
  using WSClient = websocketpp::client<websocketpp::config::core>;
  using WSConnection = websocketpp::connection<websocketpp::config::core>;

  std::mutex accessMutex;

  std::shared_ptr<cspot::EventLoop> eventLoop;

  std::array<std::byte, 1024 * 8L> inputBuffer{};

  std::shared_ptr<bell::TLSSocket> socket;

  // Flag used to notify when the connection was established
  bool connectionReady = false;

  // websocketpp related structs
  WSClient wsClient;
  WSConnection::ptr wsConnection;

  // Handles websocketpp specific events
  void onWSMessage(websocketpp::connection_hdl conn, WSClient::message_ptr msg);
  void onWSOpen(websocketpp::connection_hdl conn);
  void onWSFail(websocketpp::connection_hdl conn);
  void onWSClose(websocketpp::connection_hdl conn);
  std::error_code wsWriteHandler(websocketpp::connection_hdl hdl,
                                 char const* data, size_t size);
};
}  // namespace cspot
