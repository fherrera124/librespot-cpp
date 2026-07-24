#include <doctest/doctest.h>

#include <unistd.h>
#include <atomic>
#include <iostream>
#include <thread>

#include "bell/Logger.h"
#include "bell/net/SocketPollListener.h"
#include "bell/net/TCPSocket.h"
#include "bell/utils/Utils.h"

namespace {
std::atomic<bool> echoServerRunning = false;
std::mutex echoServerMutex;
void runEchoServer(std::shared_ptr<bell::net::TCPSocket> serverSocket) {
  std::scoped_lock lock(echoServerMutex);
  echoServerRunning = true;

  bell::net::SocketPollListener pollListener;

  std::vector<std::shared_ptr<bell::net::TCPSocket>> clientSockets;

  pollListener.registerSocket(
      serverSocket, bell::PollEvent::Readable,
      [&serverSocket, &pollListener, &clientSockets](auto& /*sock*/) {
        BELL_LOG(info, "Echo server", "Accepting new connection");

        auto sock = std::make_shared<bell::TCPSocket>(*serverSocket->accept());
        // Handle ::accept
        clientSockets.push_back(sock);

        pollListener.registerSocket(
            clientSockets.back(), bell::PollEvent::Readable, [](auto& sock) {
              // Handle client socket read
              std::string buffer;
              buffer.resize(1024);
              size_t bytesRead = *sock.read(
                  reinterpret_cast<std::byte*>(buffer.data()), buffer.size());
              if (bytesRead > 0) {
                BELL_LOG(info, "Echo server", "Received {} bytes", bytesRead);
                (void)sock.write(
                    reinterpret_cast<const std::byte*>(buffer.data()),
                    bytesRead);
              }
            });
      });
  while (echoServerRunning) {
    pollListener.poll(100);
  }

  // Stop the server
  serverSocket->close();
}
}  // namespace

TEST_CASE("bell::io::Socket and derieved classes tests") {
  int echoServerPort = 7542;
  auto echoServerSocket = std::make_shared<bell::net::TCPSocket>();

  // Bind the socket to the echo server port
  REQUIRE(echoServerSocket->bind("127.0.0.1", echoServerPort).has_value());

  // Bind should have opened the socket
  REQUIRE(echoServerSocket->isValid());

  // Listen on the socket, with a backlog of 5
  REQUIRE(echoServerSocket->listen(5));

  // Start the echo server runner
  std::thread echoServerRunner(runEchoServer, echoServerSocket);

  // Ensure the server is running
  bell::utils::sleepMs(500);

  // Test the TCP client for basic operations
  auto clientSocket = std::make_unique<bell::net::TCPSocket>();

  SUBCASE("Connect to echo server") {
    REQUIRE(
        clientSocket->connect("127.0.0.1", echoServerPort, 2000).has_value());
    REQUIRE(clientSocket->isValid());
  }

  SUBCASE("Write to and read from echo server") {
    REQUIRE(clientSocket->connect("127.0.0.1", echoServerPort, 2000));
    std::string message = "Hello, Echo Server!";
    auto bytesWritten = clientSocket->write(
        reinterpret_cast<const std::byte*>(message.data()), message.size());

    REQUIRE(bytesWritten.has_value());
    REQUIRE(bytesWritten == message.size());

    std::string readBuffer(1024, '\0');
    auto bytesRead = clientSocket->read(
        reinterpret_cast<std::byte*>(readBuffer.data()), readBuffer.size());

    REQUIRE(bytesRead);
    REQUIRE(bytesRead == message.size());
    REQUIRE(readBuffer.substr(0, *bytesRead) == message);
  }

  SUBCASE("Connect timeout handling") {
    // Test if the client handles connection timeouts correctly
    REQUIRE(clientSocket->setBlocking(false));
    REQUIRE_FALSE(clientSocket->connect("10.255.255.1", echoServerPort,
                                        500));  // Impossible address for demo
  }

  SUBCASE("Other basic operations") {
    // Example of closing the socket
    REQUIRE(clientSocket->connect("127.0.0.1", echoServerPort, 2000));
    REQUIRE(clientSocket->isValid());
    clientSocket->close();
    REQUIRE_FALSE(clientSocket->isValid());
  }

  {
    // Stop the echo server
    echoServerRunning = false;
    std::scoped_lock lock(echoServerMutex);
    echoServerRunner.join();
  }
}
