#include "bell/http/Client.h"

// System includes
#include <netinet/tcp.h>

// Own includes
#include "bell/Result.h"
#include "bell/http/Common.h"
#include "bell/http/Writer.h"
#include "bell/net/SocketStream.h"
#include "bell/net/TLSSocket.h"
#include "bell/net/URIParser.h"

using namespace bell::http;
namespace {
// Default connection pool with a size of 8, shared across all DefaultTransport instances
std::shared_ptr<ConnectionPool> defaultConnectionPoll =
    std::make_shared<ConnectionPool>(8);
}  // namespace

ConnectionPool::ConnectionPool(size_t maxConnections)
    : maxConnections(maxConnections) {}

ConnectionPool::~ConnectionPool() {}

bell::Result<std::shared_ptr<bell::Socket>> ConnectionPool::acquire(
    const std::string& host, int port) {
  std::scoped_lock lock(poolMutex);

  ConnectionKey key{host, port};
  auto it = pool.find(key);

  if (it != pool.end() && !it->second.empty()) {
    // Pop a socket (LIFO)
    auto& vec = it->second;
    auto entry = std::move(vec.back());
    vec.pop_back();

    PoolDeleter del{this->weak_from_this(), key};

    // Transfer ownership out of unique_ptr into the lease
    std::unique_ptr<bell::Socket> up = std::move(entry.first);
    bell::Socket* raw = up.release();

    std::shared_ptr<bell::Socket> lease(raw, std::move(del));

    return lease;
  }

  return bell::make_unexpected_errc<std::shared_ptr<bell::Socket>>(
      std::errc::no_such_file_or_directory);
}

void ConnectionPool::insert(const std::string& host, int port,
                            std::unique_ptr<Socket> socket) {
  std::scoped_lock lock(poolMutex);

  ConnectionKey key{host, port};

  pool[key].emplace_back(std::move(socket), std::chrono::system_clock::now());

  enforceMaxConnections();
}

void ConnectionPool::PoolDeleter::operator()(bell::Socket* s) const noexcept {
  if (!s)
    return;
  // Try to reinsert back into the pool; if pool is gone, delete the socket.
  auto p = pool.lock();
  if (p) {
    try {
      p->reinsert(key, std::unique_ptr<bell::Socket>(s));
      return;
    } catch (...) {
      // Fall through and delete on exception to avoid leaks.
    }
  } else {
  }
  delete s;
}

void ConnectionPool::reinsert(const ConnectionKey& key,
                              std::unique_ptr<bell::Socket> sock) noexcept {
  try {
    std::scoped_lock lk(poolMutex);
    auto& bucket = pool[key];
    bucket.emplace_back(std::move(sock), std::chrono::system_clock::now());
    enforceMaxConnections();
  } catch (...) {
    // Last-resort: if we cannot reinsert due to OOM or other issues,
    // drop the connection to avoid leaks.
  }
}

void ConnectionPool::enforceMaxConnections() {
  auto now = std::chrono::system_clock::now();
  const auto maxAge = std::chrono::seconds(connectionIdleTimeoutSec);
  size_t totalSize = 0;

  for (auto poolIt = pool.begin(); poolIt != pool.end();) {
    auto& conns = poolIt->second;

    for (auto connsIt = conns.begin(); connsIt != conns.end();) {
      if ((now - connsIt->second) > maxAge) {
        connsIt = conns.erase(connsIt);
      } else {
        ++connsIt;
      }
    }

    if (conns.empty()) {
      poolIt = pool.erase(poolIt);
    } else {
      totalSize += conns.size();
      ++poolIt;
    }
  }

  while (totalSize > maxConnections) {
    auto oldestPoolIt = pool.end();
    auto oldestTimestamp = std::chrono::system_clock::time_point::max();

    // Find the globally oldest connection
    for (auto it = pool.begin(); it != pool.end(); ++it) {
      if (!it->second.empty() && it->second.front().second < oldestTimestamp) {
        oldestTimestamp = it->second.front().second;
        oldestPoolIt = it;
      }
    }

    // Found oldest connection
    if (oldestPoolIt != pool.end()) {
      auto& connsToPrune = oldestPoolIt->second;

      // Remove the oldest connection
      connsToPrune.erase(connsToPrune.begin());
      totalSize--;

      // If the conns is now empty, remove it from the pool map.
      if (connsToPrune.empty()) {
        pool.erase(oldestPoolIt);
      }
    } else {
      break;
    }
  }
}

DefaultTransport::DefaultTransport(
    std::shared_ptr<ConnectionPool> connectionPoll) {
  if (connectionPoll) {
    this->connectionPool = std::move(connectionPoll);
  } else {
    this->connectionPool = defaultConnectionPoll;
  }
}

bell::Result<Response> DefaultTransport::execute(const Request& req) {
  std::shared_ptr<net::SocketStream> socketStream;

  int port = req.uri.port.value_or(req.uri.scheme == "https" ? 443 : 80);

  auto connection = connectionPool->acquire(*req.uri.host, port);

  if (connection) {
    std::cout << "Reusing existing connection to " << *req.uri.host << ":"
              << port << std::endl;
    socketStream = std::make_shared<net::SocketStream>(*connection);
  } else {
    if (req.uri.scheme == "https") {
      auto socket = std::make_unique<net::TLSSocket>();
      auto res = socket->connect(*req.uri.host, port,
                                 req.operationTimeoutMs.value_or(0));
      if (!res) {
        return tl::make_unexpected(res.error());
      }

      connectionPool->insert(*req.uri.host, port, std::move(socket));
      auto connection = connectionPool->acquire(*req.uri.host, port);
      if (!connection) {
        return tl::make_unexpected(connection.error());
      }
      socketStream = std::make_shared<net::SocketStream>(*connection);
    } else {
      auto socket = std::make_unique<net::TCPSocket>();
      // Set nodelay on socket
      (void)socket->setOption(IPPROTO_TCP, TCP_NODELAY, 1);
      auto res = socket->connect(*req.uri.host, port,
                                 req.operationTimeoutMs.value_or(0));
      if (!res) {
        return tl::make_unexpected(res.error());
      }

      connectionPool->insert(*req.uri.host, port, std::move(socket));
      auto connection = connectionPool->acquire(*req.uri.host, port);
      if (!connection) {
        return tl::make_unexpected(connection.error());
      }
      socketStream = std::make_shared<net::SocketStream>(*connection);
    }
  }

  // Create a writer for the request
  http::Writer writer(Direction::Request, socketStream);
  // Set the host header
  writer.setHeader("Host", *req.uri.host);

  std::string requestPath = *req.uri.path;
  // Handle query parameters if present
  if (req.uri.query.has_value()) {
    requestPath += "?" + *req.uri.query;
  }

  // Write the request
  auto res = writer.writeRequest(req.method, requestPath, req.headers,
                                 req.contentLength.value_or(0));

  if (!res) {
    std::cout << "Error during request write" << std::endl;
    return tl::make_unexpected(res.error());
  }

  // Process a body, if its present
  if (req.contentLength.value_or(0) > 0) {
    // Get the underlying output stream from your writer
    std::ostream& outStream = *writer.getStream();

    std::visit(
        [&outStream](auto&& bodyContent) {
          using T = std::decay_t<decltype(bodyContent)>;

          // Case 1: The body is a pre-buffered vector of bytes
          if constexpr (std::is_same_v<T, std::vector<std::byte>> ||
                        std::is_same_v<T, std::string_view> ||
                        std::is_same_v<T, tcb::span<std::byte>>) {
            if (!bodyContent.empty()) {
              outStream.write(reinterpret_cast<const char*>(bodyContent.data()),
                              bodyContent.size());
            }
          }
          // Case 2: The body is a stream (the zero-copy path)
          else if constexpr (std::is_same_v<T, std::istream*>) {
            if (bodyContent) {  // Check for non-null pointer
              std::istream& inStream = *bodyContent;
              std::array<char, 1024> buffer{};

              while (!inStream.eof()) {
                inStream.read(buffer.data(), sizeof(buffer));
                std::streamsize bytesRead = inStream.gcount();
                if (bytesRead > 0) {
                  outStream.write(buffer.data(), bytesRead);
                }
              }
            }
          }
          // Case 3 (std::monostate): Do nothing, there is no body.
        },
        req.body);
  }

  socketStream->flush();

  if (socketStream->bad()) {
    std::cout << "Stream bad after flush" << std::endl;

    return bell::make_unexpected_errc<Response>(std::errc::io_error);
  }

  // Create a reader for the response
  http::Reader reader(Direction::Response, socketStream);

  // Try to read the headers
  res = reader.readHeaders();
  if (!res) {
    std::cout << "Error during headers read" << std::endl;
    return tl::make_unexpected(res.error());
  }

  // for (const auto& header : reader.getAllHeaders()) {
  //   std::cout << header.first << ": " << header.second << std::endl;
  // }

  // Move the reader into the response
  return {std::move(reader)};
}

bell::Result<Request> Request::create(http::Method method,
                                      const std::string& url) {
  auto parsedUrl = bell::net::parseURI(url);
  if (!parsedUrl) {
    return tl::make_unexpected(http::Errc::InvalidURL);
  }
  return Request({method, *parsedUrl});
}

Response::Response(http::Reader responseReader)
    : bodyReader(std::move(responseReader)) {
  // Extract status code and message from the reader
  statusCode = *bodyReader.getStatusCode();
  headers = bodyReader.getAllHeaders();
  contentLength = bodyReader.getContentLength();
  statusMessage = *bodyReader.getStatusMessage();
}

bell::Result<std::string_view> Response::text() {
  return bodyReader.getBodyStringView();
}

bell::Result<std::vector<std::byte>> Response::bytes() {
  return bodyReader.getBodyBytes();
}

bell::Result<const std::byte*> Response::bytesPtr() {
  return bodyReader.getBodyBytesPtr();
}

bell::Result<size_t> Response::bytesLength() {
  return bodyReader.getBodyBytesLength();
}

std::istream* Response::stream() const {
  return bodyReader.getStream();
}

bell::Result<Response> Client::rawRequest(Request& req) {
  std::visit(
      [&req](auto&& bodyContent) {
        using T = std::decay_t<decltype(bodyContent)>;

        if constexpr (std::is_same_v<T, std::vector<std::byte>> ||
                      std::is_same_v<T, std::string_view> ||
                      std::is_same_v<T, tcb::span<std::byte>>) {
          req.contentLength = bodyContent.size();

        } else if constexpr (std::is_same_v<T, std::monostate>) {
          // No body, no content length
          req.contentLength = 0;
        }
      },
      req.body);

  return transport->execute(req);
}

bell::Result<Response> Client::get(const std::string& url,
                                   const Headers& headers) {
  auto req = Request::create(Method::GET, url);
  if (!req) {
    return tl::make_unexpected(req.error());
  }
  req->headers = headers;
  req->operationTimeoutMs = operationTimeoutMs;
  return rawRequest(*req);
}

bell::Result<Response> Client::post(const std::string& url,
                                    const Headers& headers, RequestBody body,
                                    std::optional<size_t> bodyLength) {
  auto req = Request::create(Method::POST, url);
  if (!req) {
    return tl::make_unexpected(req.error());
  }
  req->headers = headers;
  req->operationTimeoutMs = operationTimeoutMs;
  req->body = std::move(body);
  req->contentLength = bodyLength;

  return rawRequest(*req);
}

bell::Result<Response> Client::put(const std::string& url,
                                   const Headers& headers, RequestBody body,
                                   std::optional<size_t> bodyLength) {
  auto req = Request::create(Method::PUT, url);
  if (!req) {
    return tl::make_unexpected(req.error());
  }
  req->headers = headers;
  req->operationTimeoutMs = operationTimeoutMs;
  req->body = std::move(body);
  req->contentLength = bodyLength;

  return rawRequest(*req);
}
