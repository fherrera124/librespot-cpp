#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "BellTask.h"        // for Task
#include "TCPServerSocket.h"  // for TCPServerSocket

namespace bell {

// Basic, portable (ESP/Linux/Apple/Win32) HTTP/1.1 server built on bell's
// own classes - no CivetWeb/third-party dependency, unlike BellHTTPServer
// (which needs BELL_DISABLE_WEBSERVER off). For simple local use (zeroconf
// pairing and similar), not a general-purpose web server:
//   - No WebSocket support - stays exclusive to the CivetWeb-backed
//     BellHTTPServer for whoever genuinely needs that.
//   - No keep-alive - every response closes the connection, same as what
//     esp_http_server-based callers of this were already relying on.
//   - Sequential - one connection handled at a time on its own bell::Task,
//     no thread pool. Fine for low-frequency, human-triggered local
//     requests; not meant for real traffic volume.
class SimpleHTTPServer : public bell::Task {
 public:
  struct HTTPResponse {
    std::string body;
    std::string contentType = "application/json";
    int status = 200;
  };
  typedef std::unordered_map<std::string, std::string> Params;
  // `body` is already fully read (Content-Length already satisfied) - no
  // streaming API, not needed for this use case.
  typedef std::function<HTTPResponse(const std::string& body,
                                     const Params& params)>
      Handler;

  explicit SimpleHTTPServer(uint16_t port);
  ~SimpleHTTPServer();

  void registerGet(const std::string& route, Handler handler);
  void registerPost(const std::string& route, Handler handler);

 protected:
  void runTask() override;

 private:
  // Same trie-with-params (":name") and catch-all ("*") matching as
  // BellHTTPServer::Router (bell/main/io/BellHTTPServer.cpp) - copied
  // rather than shared because that one is tied to CivetWeb's HTTPHandler
  // signature/mg_connection*, and both are pure routing logic with no
  // framework dependency of their own either way.
  struct Router {
    struct Node {
      std::unordered_map<std::string, std::unique_ptr<Node>> children;
      Handler value = nullptr;
      std::string paramName;
      bool isParam = false;
      bool isCatchAll = false;
    };
    Node root;

    static std::vector<std::string> split(const std::string& path);
    void insert(const std::string& route, Handler handler);
    std::pair<Handler, Params> find(const std::string& route);
  };

  void handleConnection(int clientFd);

  TCPServerSocket serverSocket;
  Router getRouter, postRouter;

  std::atomic<bool> running{true};
  // F93 pattern (see TrackPlayer.h/DealerClient.h) - held by runTask() for
  // its whole life, taken by the destructor after stop() so the object
  // can't be freed under a still-running task.
  std::mutex taskLifetimeMutex;
};

}  // namespace bell
