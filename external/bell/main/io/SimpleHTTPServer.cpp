#include "SimpleHTTPServer.h"

#include <algorithm>  // for min
#include <cstdio>     // for snprintf
#include <cstdlib>    // for strtoul
#include <cstring>    // for memcpy
#include <exception>

#ifdef _WIN32
#include <winsock2.h>
#include "win32shim.h"  // for strncasecmp
#else
#include <strings.h>   // for strncasecmp
#include <sys/socket.h>
#include <sys/time.h>  // for struct timeval (SO_RCVTIMEO/SO_SNDTIMEO)
#include <unistd.h>
#endif

#include "BellLogger.h"
#include "picohttpparser.h"

using namespace bell;

namespace {
// Bounded on purpose - this is for small local requests (zeroconf pairing
// and similar), not general-purpose upload handling.
constexpr size_t MAX_REQUEST_SIZE = 8192;
constexpr size_t MAX_HEADERS = 32;
}  // namespace

std::vector<std::string> SimpleHTTPServer::Router::split(
    const std::string& path) {
  std::vector<std::string> parts;
  size_t start = 0;
  while (start <= path.size()) {
    size_t slash = path.find('/', start);
    std::string part = path.substr(start, slash - start);
    if (!part.empty()) {
      parts.push_back(part);
    }
    if (slash == std::string::npos) {
      break;
    }
    start = slash + 1;
  }
  return parts;
}

void SimpleHTTPServer::Router::insert(const std::string& route,
                                      Handler handler) {
  auto parts = split(route);
  Node* current = &root;

  for (auto& part : parts) {
    if (!part.empty() && part[0] == ':') {
      current->isParam = true;
      current->paramName = part.substr(1);
      part = "";
    } else if (!part.empty() && part[0] == '*') {
      current->isCatchAll = true;
      current->value = handler;
      return;
    }

    if (!current->children.count(part)) {
      current->children[part] = std::make_unique<Node>();
    }
    current = current->children[part].get();
  }
  current->value = handler;
}

std::pair<SimpleHTTPServer::Handler, SimpleHTTPServer::Params>
SimpleHTTPServer::Router::find(const std::string& route) {
  auto parts = split(route);
  Node* current = &root;
  Params params;

  for (auto& part : parts) {
    if (current->children.count(part)) {
      current = current->children[part].get();
    } else if (current->isParam) {
      params[current->paramName] = part;
      if (current->children.count("")) {
        current = current->children[""].get();
      } else {
        return {nullptr, {}};
      }
    } else if (current->isCatchAll) {
      return {current->value, params};
    } else {
      return {nullptr, {}};
    }
  }

  return {current->value, params};
}

SimpleHTTPServer::SimpleHTTPServer(uint16_t port)
    : bell::Task("bellHttpServer", 8 * 1024, 0, 0) {
  serverSocket.open(port);
  startTask();
}

SimpleHTTPServer::~SimpleHTTPServer() {
  stopAndWait();
}

void SimpleHTTPServer::registerGet(const std::string& route,
                                   Handler handler) {
  getRouter.insert(route, handler);
}

void SimpleHTTPServer::registerPost(const std::string& route,
                                    Handler handler) {
  postRouter.insert(route, handler);
}

void SimpleHTTPServer::runTask() {
  while (!shouldStop()) {
    int clientFd = serverSocket.acceptWithTimeout(500);
    if (clientFd < 0) {
      continue;
    }
    // Sequential on purpose - see the class comment.
    handleConnection(clientFd);
  }
}

namespace {
void closeFd(int fd) {
#ifdef _WIN32
  closesocket(fd);
#else
  ::close(fd);
#endif
}

void writeAll(int fd, const char* data, size_t len) {
  size_t sent = 0;
  while (sent < len) {
    int n = send(fd, data + sent, (int)(len - sent), 0);
    if (n <= 0) {
      return;
    }
    sent += (size_t)n;
  }
}

void sendResponse(int fd, const SimpleHTTPServer::HTTPResponse& response) {
  char headerBuf[256];
  int headerLen = snprintf(
      headerBuf, sizeof(headerBuf),
      "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
      "Connection: close\r\n\r\n",
      response.status, response.status == 200 ? "OK" : "Error",
      response.contentType.c_str(), response.body.size());
  if (headerLen > 0) {
    writeAll(fd, headerBuf, (size_t)headerLen);
  }
  if (!response.body.empty()) {
    writeAll(fd, response.body.data(), response.body.size());
  }
}
}  // namespace

namespace {
// accept() hands back a socket with no read/write timeout of its own - a
// client that connects and then stalls (drops off WiFi mid-request, etc.)
// would otherwise block this class's single-threaded accept loop in
// recv()/send() forever, deafening the server to every other client until
// reboot (Slowloris-style). Local pairing/control traffic has no business
// taking anywhere near this long.
void setClientTimeout(int fd, int timeoutMs) {
#ifdef _WIN32
  DWORD timeout = (DWORD)timeoutMs;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout,
             sizeof(timeout));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout,
             sizeof(timeout));
#else
  struct timeval tv;
  tv.tv_sec = timeoutMs / 1000;
  tv.tv_usec = (timeoutMs % 1000) * 1000;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));
#endif
}
}  // namespace

void SimpleHTTPServer::handleConnection(int clientFd) {
  // TEMP DIAGNOSTIC (ZeroConf "zombie server" investigation, 2026-07-20):
  // confirms whether anything ever reaches this server at all during a
  // "stuck connecting" incident - remove once resolved.
  BELL_LOG(info, "SimpleHTTPServer", "connection accepted (fd=%d)", clientFd);

  constexpr int CLIENT_TIMEOUT_MS = 2000;
  setClientTimeout(clientFd, CLIENT_TIMEOUT_MS);

  std::vector<char> buf(MAX_REQUEST_SIZE);
  size_t bufLen = 0;

  const char* method = nullptr;
  size_t methodLen = 0;
  const char* path = nullptr;
  size_t pathLen = 0;
  int minorVersion = 0;
  struct phr_header headers[MAX_HEADERS];
  size_t numHeaders;

  int parseResult = -2;
  while (parseResult == -2) {
    if (bufLen >= buf.size()) {
      sendResponse(clientFd, {"", "text/plain", 431});  // headers too large
      closeFd(clientFd);
      return;
    }
    int n = recv(clientFd, buf.data() + bufLen, (int)(buf.size() - bufLen), 0);
    if (n <= 0) {
      // TEMP DIAGNOSTIC, see above.
      BELL_LOG(info, "SimpleHTTPServer",
              "connection closed before a full request arrived (fd=%d, "
              "recv=%d)",
              clientFd, n);
      closeFd(clientFd);  // peer gone/timed out mid-request - nothing to reply to
      return;
    }
    size_t prevLen = bufLen;
    bufLen += (size_t)n;
    numHeaders = MAX_HEADERS;
    parseResult = phr_parse_request(buf.data(), bufLen, &method, &methodLen,
                                    &path, &pathLen, &minorVersion, headers,
                                    &numHeaders, prevLen);
  }

  if (parseResult == -1) {
    sendResponse(clientFd, {"", "text/plain", 400});
    closeFd(clientFd);
    return;
  }

  std::string methodStr(method, methodLen);
  std::string pathStr(path, pathLen);
  size_t queryPos = pathStr.find('?');
  std::string routePath =
      queryPos == std::string::npos ? pathStr : pathStr.substr(0, queryPos);

  size_t contentLength = 0;
  for (size_t i = 0; i < numHeaders; i++) {
    if (headers[i].name != nullptr &&
        strncasecmp(headers[i].name, "Content-Length", headers[i].name_len) ==
            0 &&
        headers[i].name_len == strlen("Content-Length")) {
      contentLength =
          (size_t)strtoul(std::string(headers[i].value, headers[i].value_len)
                              .c_str(),
                          nullptr, 10);
    }
  }

  if (contentLength > MAX_REQUEST_SIZE) {
    sendResponse(clientFd, {"", "text/plain", 413});  // payload too large
    closeFd(clientFd);
    return;
  }

  std::string body;
  if (contentLength > 0) {
    body.resize(contentLength);
    size_t bodyStart = (size_t)parseResult;
    size_t alreadyHave = bufLen > bodyStart ? bufLen - bodyStart : 0;
    alreadyHave = std::min(alreadyHave, contentLength);
    if (alreadyHave > 0) {
      memcpy(body.data(), buf.data() + bodyStart, alreadyHave);
    }
    size_t received = alreadyHave;
    while (received < contentLength) {
      int n = recv(clientFd, body.data() + received,
                   (int)(contentLength - received), 0);
      if (n <= 0) {
        closeFd(clientFd);
        return;
      }
      received += (size_t)n;
    }
  }

  Router& router = (methodStr == "GET") ? getRouter : postRouter;
  auto found = router.find(routePath);

  HTTPResponse response;
  if (found.first == nullptr) {
    response.status = 404;
    response.contentType = "text/plain";
    response.body = "Not Found";
  } else {
    try {
      response = found.first(body, found.second);
    } catch (const std::exception& e) {
      BELL_LOG(error, "SimpleHTTPServer", "handler threw: %s", e.what());
      response.status = 500;
      response.contentType = "text/plain";
      response.body = "Internal Server Error";
    }
  }

  // TEMP DIAGNOSTIC, see handleConnection()'s top.
  BELL_LOG(info, "SimpleHTTPServer", "request: %s %s -> %d", methodStr.c_str(),
          routePath.c_str(), response.status);

  sendResponse(clientFd, response);
  closeFd(clientFd);
}
