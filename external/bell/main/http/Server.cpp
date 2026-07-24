#include "bell/http/Server.h"

#include "bell/Logger.h"
#include "bell/http/Common.h"
#include "bell/http/Reader.h"
#include "bell/net/SocketStream.h"
#include "bell/net/TCPSocket.h"
#include "bell/utils/Task.h"
#include "bell/utils/Utils.h"
#include "tl/expected.hpp"

#include <sys/select.h>
#include <unistd.h>

using namespace bell;

http::Server::Server(int maxConnections)
    : utils::Task("bell::net::HTTPServer", 16 * 1024),
      maxConnections(maxConnections) {
  notFoundHandler = [](const auto& /*requestReader*/,
                       const auto& responseWriter, const auto& /*params*/) {
    (void)responseWriter->writeResponseWithBody(404, {}, "Not found");
  };
}

http::Server::~Server() {
  stopTask();
  if (listenSocket.isValid()) {
    listenSocket.close();
  }
}

bell::Result<> http::Server::listen(int port) {
  // Stop the task if it's already running
  stopTask();

  if (listenSocket.isValid()) {
    listenSocket.close();
  }

  // Try to bind to the specified port
  auto listenRes = listenSocket.bind("", port);
  if (!listenRes) {
    return tl::make_unexpected(listenRes.error());
  }

  // Set the socket to non-blocking mode
  auto res = listenSocket.setBlocking(false);
  if (!res) {
    return tl::make_unexpected(res.error());
  }

  // Start listening for incoming connections
  res = listenSocket.listen(maxConnections);
  if (!res) {
    return tl::make_unexpected(res.error());
  }

  // Prepare master fd set for select
  FD_ZERO(&masterFdSet);
  FD_SET(listenSocket.getFd(), &masterFdSet);
  maxFd = listenSocket.getFd();

  startTask();  // Will begin the task loop
  BELL_LOG(info, LOG_TAG, "Server listening on port {}", *listenRes);

  return {};
}

void http::Server::registerCustom404(const RequestHandler& handler) {
  notFoundHandler = handler;
}

void http::Server::acceptConnection() {
  // Accept the connection
  auto acceptedSock = listenSocket.accept();

  if (acceptedSock) {

    auto setBlockingRes = acceptedSock->setBlocking(false);
    if (!setBlockingRes) {
      BELL_LOG(error, LOG_TAG, "Error setBlocking on accepted socket: {}",
               setBlockingRes.error());
      return;
    }

    int clientFd = acceptedSock->getFd();
    FD_SET(clientFd, &masterFdSet);
    BELL_LOG(debug, LOG_TAG, "Accepted connection");
    connections.push_back({
        std::make_shared<net::TCPSocket>(std::move(*acceptedSock)),
        false,
    });
  } else {
    BELL_LOG(error, LOG_TAG, "Error accepting connection: {}",
             acceptedSock.error());
  }
}

void http::Server::registerHandler(Method method, const std::string& path,
                                   const RequestHandler& handler) {
  router.insert(method, path, handler);
}

void http::Server::registerGet(const std::string& path,
                               const RequestHandler& handler) {
  registerHandler(Method::GET, path, handler);
}

void http::Server::registerPost(const std::string& path,
                                const RequestHandler& handler) {
  registerHandler(Method::POST, path, handler);
}

void http::Server::closeConnection(int fd) {
  for (auto& connection : connections) {
    if (connection.socket->getFd() == fd) {
      // Mark the connection as closed
      connection.closed = true;
      return;
    }
  }
}

void http::Server::readFromClient(const Connection& connection) {
  // Wrap the socket in a stream, try to parse the request
  net::SocketStream socketStream(connection.socket);

  auto reader = std::make_unique<http::Reader>(Direction::Request,
                                               &socketStream, &readBuffer);
  auto readerRes = reader->readHeaders();

  if (!readerRes) {
    BELL_LOG(error, LOG_TAG, "Error reading headers: {}", readerRes.error());
    closeConnection(connection.socket->getFd());
    return;
  }

  auto writer =
      std::make_unique<http::Writer>(Direction::Response, &socketStream);
  writer->setHeader("Connection", "close");

  // Find the handler for the request
  auto handler = router.find(*reader->getMethod(), *reader->getPath());

  if (!handler) {
    notFoundHandler(reader, writer, {});
  } else {
    try {
      handler->first(reader, writer, handler->second);
    } catch (const std::exception& e) {
      BELL_LOG(error, LOG_TAG, "Error occured in the request handler: {}",
               e.what());
      (void)writer->writeResponseWithBody(500, {}, "Internal server error");
    }
  }

  if (!writer->hasWrittenHeaders() || !writer->hasWrittenBody()) {
    BELL_LOG(error, LOG_TAG, "Handler did not write response");
  }

  closeConnection(connection.socket->getFd());
}

void http::Server::taskLoop() {
  fd_set readFdSet = masterFdSet;

  auto selectTV = bell::utils::millisecondsToTimeval(1000);

  maxFd = listenSocket.getFd();
  for (const auto& it : connections) {
    if (it.socket->getFd() > maxFd) {
      maxFd = it.socket->getFd();
    }
  }

  // Wait for activity on the sockets
  if (::select(maxFd + 1, &readFdSet, nullptr, nullptr, &selectTV) < 0) {
    BELL_LOG(error, LOG_TAG, "Error in select errno={}, closing the server",
             strerror(errno));
    taskRunning = false;
    return;
  }

  // Check for new connections
  if (FD_ISSET(listenSocket.getFd(), &readFdSet)) {
    acceptConnection();
  }

  // Handle data from each connected client
  for (auto it = connections.begin(); it != connections.end();) {
    int clientFd = it->socket->getFd();
    if (FD_ISSET(clientFd, &readFdSet)) {
      readFromClient(*it);
    }

    if (it->closed) {
      BELL_LOG(debug, LOG_TAG, "Closing connection");
      FD_CLR(it->socket->getFd(), &masterFdSet);
      it->socket->close();
      it = connections.erase(it);
    } else {
      ++it;
    }
  }
}
