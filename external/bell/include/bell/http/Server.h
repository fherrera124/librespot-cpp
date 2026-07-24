#pragma once

#include <array>
#include <functional>
#include <vector>

#include "bell/Result.h"
#include "bell/http/RadixRouter.h"
#include "bell/http/Reader.h"
#include "bell/http/Writer.h"
#include "bell/net/SocketStream.h"
#include "bell/net/TCPSocket.h"
#include "bell/utils/Task.h"

namespace bell::http {
class Server : bell::utils::Task {
 public:
  Server(int maxConnections = 5);
  ~Server() override;
  using RequestHandler = std::function<void(
      const std::unique_ptr<Reader>& requestReader,
      const std::unique_ptr<Writer>& responseWriter,
      const std::unordered_map<std::string, std::string>& routeParams)>;

  bell::Result<> listen(int port = 8080);

  void registerHandler(Method method, const std::string& path,
                       const RequestHandler& handler);

  void registerGet(const std::string& path, const RequestHandler& handler);

  void registerPost(const std::string& path, const RequestHandler& handler);

  void registerCustom404(const RequestHandler& handler);

 private:
  const char* LOG_TAG = "http::Server";

  // Default timeout for HTTP operations
  const int defaultHttpOperationTimeout = 5000;

  RadixRouter<RequestHandler> router;

  // Maximum number of connections to accept
  int maxConnections;

  // TCP socket used for listening for incoming connections
  bell::net::TCPSocket listenSocket;

  // Type used to represent an active connection
  struct Connection {
    // Client socket
    std::shared_ptr<bell::net::TCPSocket> socket;
    bool closed = false;
  };

  // ::select() related members
  int maxFd = 0;
  fd_set masterFdSet{};

  const int maxReadBufferSize = 16 * 1024;
  std::vector<char> readBuffer{};

  std::vector<Connection> connections{};

  RequestHandler notFoundHandler;

  void acceptConnection();

  void readFromClient(const Connection& connection);

  void closeConnection(int fd);

  void taskLoop() override;
};
}  // namespace bell::http

namespace bell {
using HTTPServer = http::Server;
using HTTPRequestParams = const std::unordered_map<std::string, std::string>&;
}  // namespace bell
