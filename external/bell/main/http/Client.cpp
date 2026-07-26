#include "bell/http/Client.h"

// System includes
#include <netinet/tcp.h>

// Own includes
#include "bell/Logger.h"
#include "bell/Result.h"
#include "bell/http/Common.h"
#include "bell/http/Writer.h"
#include "bell/net/SocketStream.h"
#include "bell/net/TLSSocket.h"
#include "bell/net/URIParser.h"

using namespace bell::http;
namespace {
const char* LOG_TAG = "HTTPClient";
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
  // A socket explicitly closed by execute() (write failed on it, see
  // DefaultTransport::execute()'s stale-pooled-connection retry) must not
  // go back into the pool looking healthy - the next acquire() would just
  // hand it out again and fail the same way. Only reinsert live sockets.
  if (s->isValid()) {
    auto p = pool.lock();
    if (p) {
      try {
        p->reinsert(key, std::unique_ptr<bell::Socket>(s));
        return;
      } catch (...) {
        // Fall through and delete on exception to avoid leaks.
      }
    }
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

namespace {
bell::Result<std::shared_ptr<bell::net::Socket>> connectFresh(
    ConnectionPool& pool, const std::optional<std::string>& scheme,
    const std::string& host, int port,
    std::optional<int> operationTimeoutMs) {
  std::unique_ptr<bell::net::Socket> socket;
  bell::Result<> connectRes = bell::make_unexpected_errc(std::errc::io_error);

  if (scheme == "https") {
    auto tlsSocket = std::make_unique<bell::net::TLSSocket>();
    connectRes =
        tlsSocket->connect(host, port, operationTimeoutMs.value_or(0));
    socket = std::move(tlsSocket);
  } else {
    auto tcpSocket = std::make_unique<bell::net::TCPSocket>();
    (void)tcpSocket->setOption(IPPROTO_TCP, TCP_NODELAY, 1);
    connectRes =
        tcpSocket->connect(host, port, operationTimeoutMs.value_or(0));
    socket = std::move(tcpSocket);
  }

  if (!connectRes) {
    return tl::make_unexpected(connectRes.error());
  }

  pool.insert(host, port, std::move(socket));
  auto connection = pool.acquire(host, port);
  if (!connection) {
    return tl::make_unexpected(connection.error());
  }
  return connection;
}
}  // namespace

bell::Result<Response> DefaultTransport::execute(const Request& req) {
  int port = req.uri.port.value_or(req.uri.scheme == "https" ? 443 : 80);

  // A connection popped from the pool may have been silently closed by the
  // peer (server-side keep-alive idle timeout, commonly well under this
  // pool's own 5-minute connectionIdleTimeoutSec) since it was last used -
  // writing to it then fails at the TCP level ("Write failed: I/O error"),
  // even though nothing else is wrong. Reproduced on real hardware: a
  // Connect-state PUT silently never reached the server this way, and
  // since nothing above this layer retried, the app just moved on as if
  // the PUT had gone out. Retrying once on a brand-new (non-pooled)
  // connection is safe here because a write failure at this point means
  // the request was never received by the peer - nothing to duplicate.
  for (int attempt = 0; attempt < 2; attempt++) {
    bool reusedFromPool = false;
    std::shared_ptr<bell::net::Socket> connection;

    auto pooled = connectionPool->acquire(*req.uri.host, port);
    if (pooled) {
      connection = *pooled;
      reusedFromPool = true;
    } else {
      auto fresh = connectFresh(*connectionPool, req.uri.scheme, *req.uri.host,
                                port, req.operationTimeoutMs);
      if (!fresh) {
        return tl::make_unexpected(fresh.error());
      }
      connection = *fresh;
    }

    // req.operationTimeoutMs (via connectFresh) only bounds the initial
    // connect() - without this, a peer that stops responding mid-transfer
    // (e.g. a WiFi stall after the connection is already up) leaves the
    // blocking read()/recv() below with no timeout at all, hanging forever.
    // Re-applied on every request (not just fresh connections) since a
    // pooled socket may carry a stale timeout from a previous request.
    (void)connection->setReceiveTimeout(req.operationTimeoutMs.value_or(0));

    auto socketStream = std::make_shared<net::SocketStream>(connection);

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

    if (res) {
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
                  outStream.write(
                      reinterpret_cast<const char*>(bodyContent.data()),
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
    }

    bool writeFailed = !res || socketStream->bad();

    if (writeFailed) {
      // Make sure a dead connection can't go back into the pool looking
      // healthy - close it before the SocketStream/connection shared_ptr
      // drops and PoolDeleter reinserts it.
      connection->close();

      if (reusedFromPool && attempt == 0) {
        BELL_LOG(warn, LOG_TAG,
                 "Write failed on a pooled connection, retrying fresh: {}",
                 res ? "stream bad after flush" : res.error().message());
        continue;
      }

      if (!res) {
        BELL_LOG(error, LOG_TAG, "Error during request write: {}",
                 res.error());
        return tl::make_unexpected(res.error());
      }

      BELL_LOG(error, LOG_TAG, "Stream bad after flush");
      return bell::make_unexpected_errc<Response>(std::errc::io_error);
    }

    // Create a reader for the response
    http::Reader reader(Direction::Response, socketStream);

    // Try to read the headers
    res = reader.readHeaders();
    if (!res) {
      BELL_LOG(error, LOG_TAG, "Error during headers read: {}", res.error());
      return tl::make_unexpected(res.error());
    }

    // Move the reader into the response
    return {std::move(reader)};
  }

  return bell::make_unexpected_errc<Response>(std::errc::io_error);
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
  bell::Result<> bodyRes = {};

  std::visit(
      [&req, &bodyRes](auto&& bodyContent) {
        using T = std::decay_t<decltype(bodyContent)>;

        if constexpr (std::is_same_v<T, std::vector<std::byte>> ||
                      std::is_same_v<T, std::string_view> ||
                      std::is_same_v<T, tcb::span<std::byte>>) {
          req.contentLength = bodyContent.size();

        } else if constexpr (std::is_same_v<T, std::monostate>) {
          // No body, no content length
          req.contentLength = 0;
        } else if constexpr (std::is_same_v<T, std::istream*>) {
          // execute() only writes the body once it's already committed
          // Content-Length to the wire (this writer has no chunked-request
          // support), so a stream body with no length would otherwise be
          // silently sent as an empty request instead of erroring out.
          if (bodyContent && !req.contentLength.has_value()) {
            bodyRes = bell::make_unexpected_errc(std::errc::invalid_argument);
          }
        }
      },
      req.body);

  if (!bodyRes) {
    return tl::make_unexpected(bodyRes.error());
  }

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
