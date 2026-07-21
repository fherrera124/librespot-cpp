#include "HTTPClient.h"

#include <string.h>   // for memcpy
#include <algorithm>  // for transform
#include <cassert>    // for assert
#include <cctype>     // for tolower
#include <ostream>    // for operator<<, basic_ostream
#include <stdexcept>  // for runtime_error

#include "BellSocket.h"  // for bell

using namespace bell;

void HTTPClient::Response::connect(const std::string& url) {
  urlParser = bell::URLParser::parse(url);

  // Open socket of type
  this->socketStream.open(urlParser.host, urlParser.port,
                          urlParser.schema == "https");
}

HTTPClient::Response::~Response() {
  if (this->socketStream.isOpen()) {
    this->socketStream.close();
  }
}

void HTTPClient::Response::rawRequest(const std::string& url,
                                      const std::string& method,
                                      const std::vector<uint8_t>& content,
                                      Headers& headers) {
  urlParser = bell::URLParser::parse(url);

  // Prepare a request
  const char* reqEnd = "\r\n";

  for (int attempt = 0;; attempt++) {
    // Reconnect if the socket was never opened, was closed, or is left in
    // a failed state from a previous request on this same Response object
    // (e.g. a keep-alive connection reused across multiple
    // CDNAudioFile::readBytes() calls, dropped by the server in between -
    // see F58). See F82.
    if (!socketStream.isOpen() || !socketStream.good()) {
      socketStream.close();
      socketStream.clear();
      socketStream.open(urlParser.host, urlParser.port,
                        urlParser.schema == "https");
      if (!socketStream.good()) {
        throw std::runtime_error("Cannot connect to " + urlParser.host);
      }
    }

    socketStream << method << " " << urlParser.path << " HTTP/1.1" << reqEnd;
    socketStream << "Host: " << urlParser.host << ":" << urlParser.port << reqEnd;
    socketStream << "Connection: keep-alive" << reqEnd;

    // Default Accept only if the caller didn't provide their own: sending
    // both (this default + a caller's, e.g. "application/x-protobuf")
    // makes Spotify's envoy edge answer clienttoken/login5 with an empty
    // 200 (Content-Length: 0) - reproduced and bisected byte-by-byte
    // against the real endpoint. See docs/dealer_websocket_migration.md,
    // Fase 1b.
    bool callerProvidesAccept = false;
    for (auto& header : headers) {
      std::string headerName = header.first;
      std::transform(headerName.begin(), headerName.end(), headerName.begin(),
                     [](unsigned char c) { return std::tolower(c); });
      if (headerName == "accept") {
        callerProvidesAccept = true;
        break;
      }
    }
    if (!callerProvidesAccept) {
      socketStream << "Accept: */*" << reqEnd;
    }

    // Write content. PUT/POST need Content-Length even when the body is
    // empty (e.g. PlayerEngine's inactive PUT) - some servers
    // (confirmed against Spotify's spclient) reply 411 Length Required
    // without it. GET's behavior is left as-is (no header when empty).
    if (content.size() > 0 || method == "PUT" || method == "POST") {
      socketStream << "Content-Length: " << content.size() << reqEnd;
    }

    // Write headers
    for (auto& header : headers) {
      socketStream << header.first << ": " << header.second << reqEnd;
    }

    socketStream << reqEnd;

    // Write request body
    if (content.size() > 0) {
      socketStream.write((const char*)content.data(), content.size());
    }

    socketStream.flush();

    // Parse response
    try {
      readResponseHeaders();
      return;
    } catch (const std::runtime_error&) {
      // Retry once, only if the connection itself died (stream not
      // good()) - a genuine protocol error (malformed response, response
      // too large) leaves the stream good() and is rethrown immediately
      // instead of being retried pointlessly. See F82.
      if (attempt >= 1 || socketStream.good()) {
        throw;
      }
      socketStream.close();
    }
  }
}

void HTTPClient::Response::readResponseHeaders() {
  char *method, *path;
  const char* msgPointer;

  size_t msgLen;
  int pret, minorVersion, status;

  size_t prevbuflen = 0, numHeaders;
  this->httpBufferAvailable = 0;

  // readRawBody() only reads when rawBody is still empty - on a Response
  // object reused for a second request (keep-alive, e.g.
  // PlayerEngine's PUT connection), a previous non-empty body
  // (error message, etc.) left over here would make readRawBody() skip
  // reading the NEW response entirely and silently keep serving the OLD
  // body to any .body()/.bytes() caller. contentSize also needs resetting
  // - a request with no body would otherwise stay stuck at the last
  // request's size and misread the socket. CDNAudioFile.cpp never hit
  // this because it reads its own stream directly, never via body()/
  // bytes(), on a reused connection.
  this->rawBody.clear();
  this->contentSize = 0;
  this->isChunked = false;
  this->bodyRead = false;

  while (1) {
    socketStream.getline((char*)httpBuffer.data() + httpBufferAvailable,
                         httpBuffer.size() - httpBufferAvailable);

    // FIX: Un fail() también ocurre si la línea de la cabecera es demasiado
    // larga para el buffer, no solo cuando se cierra el socket. Añadido el
    // chequeo de eof() para evitar atrapar errores erróneos.
    if (socketStream.fail() && !socketStream.eof()) {
      throw std::runtime_error("Connection closed or header line too long");
    }

    prevbuflen = httpBufferAvailable;
    size_t count = socketStream.gcount();
    
    // FIX: Reemplazado el antiguo hack destructivo de memcpy.
    // getline lee hasta el delimitador '\n', lo descarta, e inserta un '\0'
    // en su lugar al final de los caracteres extraídos.
    // Para que picohttpparser funcione, solo necesitamos restaurar ese '\n'.
    // Esto previene que cabeceras estilo UNIX (sin '\r') sean truncadas y corrompidas.
    if (count > 0) {
      char* writePtr = (char*)httpBuffer.data() + httpBufferAvailable;
      writePtr[count - 1] = '\n'; 
      httpBufferAvailable += count;
    } else if (socketStream.fail() || socketStream.eof()) {
      // Si leímos 0 bytes y la conexión falló o cerró
      throw std::runtime_error("Connection closed while reading HTTP response");
    }

    // Parse the request
    numHeaders = sizeof(phResponseHeaders) / sizeof(phResponseHeaders[0]);

    pret =
        phr_parse_response((const char*)httpBuffer.data(), httpBufferAvailable,
                           &minorVersion, &status, &msgPointer, &msgLen,
                           phResponseHeaders, &numHeaders, prevbuflen);

    if (pret > 0) {
      this->status = status;
      break; /* successfully parsed the request */
    } else if (pret == -1)
      throw std::runtime_error("Cannot parse http response");

    /* request is incomplete, continue the loop */
    assert(pret == -2);
    if (httpBufferAvailable == httpBuffer.size())
      throw std::runtime_error("Response too large");
  }

  this->responseHeaders = {};

  // phr_parse_response() hands back name/value as raw pointers *into*
  // httpBuffer - trusted blindly before this. picohttpparser itself is
  // well-tested, but any bug in our own httpBufferAvailable/prevbuflen
  // bookkeeping around the reused buffer (keep-alive connections parse
  // into it repeatedly) turning a pointer bad would otherwise be a
  // silent out-of-bounds read instead of a loud, catchable failure -
  // same class of fix as everywhere else in this project this session
  // (turn UB into an exception). Found reviewing bell upstream PR #48
  // (feelfreelinux/bell, commit d755889), which added the same guard
  // for the same reason.
  const uint8_t* bufferStart = httpBuffer.data();
  const uint8_t* bufferEnd = bufferStart + httpBuffer.size();
  for (int headerIndex = 0; headerIndex < numHeaders; headerIndex++) {
    const uint8_t* name = (const uint8_t*)phResponseHeaders[headerIndex].name;
    const uint8_t* value = (const uint8_t*)phResponseHeaders[headerIndex].value;
    size_t nameLen = phResponseHeaders[headerIndex].name_len;
    size_t valueLen = phResponseHeaders[headerIndex].value_len;
    if (name < bufferStart || name + nameLen > bufferEnd || value < bufferStart ||
        value + valueLen > bufferEnd) {
      throw std::runtime_error("Parsed header points outside response buffer");
    }
    this->responseHeaders.push_back(
        ValueHeader{std::string((const char*)name, nameLen),
                    std::string((const char*)value, valueLen)});
  }

  std::string contentLengthValue = std::string(header("content-length"));
  if (contentLengthValue.size() > 0) {
    // A malformed Content-Length from the peer used to throw
    // std::invalid_argument/out_of_range straight out of std::stoi() -
    // still caught by every real caller's `catch (const std::exception&)`,
    // but NOT by rawRequest()'s own `catch (const std::runtime_error&)`
    // retry-once logic just above this call, so a malformed header skipped
    // the intended single retry and surfaced as a different, more cryptic
    // error type than every other failure in this function. See
    // docs/dealer_websocket_migration.md §48.
    try {
      this->contentSize = std::stoi(contentLengthValue);
      this->hasContentSize = true;
    } catch (const std::exception&) {
      throw std::runtime_error("Malformed Content-Length in HTTP response");
    }
  }

  // Transfer-Encoding: chunked has no Content-Length - readRawBody() used
  // to just skip reading in that case (contentSize stayed 0), silently
  // handing back an empty body with no error. See
  // docs/dealer_websocket_migration.md §48.
  std::string transferEncoding = std::string(header("transfer-encoding"));
  std::transform(transferEncoding.begin(), transferEncoding.end(),
                 transferEncoding.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  if (transferEncoding.find("chunked") != std::string::npos) {
    this->isChunked = true;
  }
}

void HTTPClient::Response::get(const std::string& url, Headers headers) {
  std::string method = "GET";
  return this->rawRequest(url, method, {}, headers);
}

void HTTPClient::Response::post(const std::string& url, Headers headers,
                                const std::vector<uint8_t>& body) {
  std::string method = "POST";
  return this->rawRequest(url, method, body, headers);
}

void HTTPClient::Response::put(const std::string& url, Headers headers,
                               const std::vector<uint8_t>& body) {
  std::string method = "PUT";
  return this->rawRequest(url, method, body, headers);
}

size_t HTTPClient::Response::contentLength() {
  return contentSize;
}

std::string_view HTTPClient::Response::header(const std::string& headerName) {
  for (auto& header : this->responseHeaders) {
    std::string headerValue = header.first;
    std::transform(headerValue.begin(), headerValue.end(), headerValue.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (headerName == headerValue) {
      return header.second;
    }
  }

  return "";
}

size_t HTTPClient::Response::totalLength() {
  auto rangeHeader = header("content-range");

  if (rangeHeader.find("/") != std::string::npos) {
    // Same hardening as readResponseHeaders()'s Content-Length parse (§48)
    // - a malformed Content-Range shouldn't throw an uncaught
    // invalid_argument/out_of_range out of this accessor.
    try {
      return std::stoi(
          std::string(rangeHeader.substr(rangeHeader.find("/") + 1)));
    } catch (const std::exception&) {
      throw std::runtime_error("Malformed Content-Range in HTTP response");
    }
  }

  return this->contentLength();
}

void HTTPClient::Response::readRawBody() {
  if (bodyRead) {
    return;
  }
  if (isChunked) {
    readChunkedBody();
  } else if (contentSize > 0) {
    rawBody = std::vector<uint8_t>(contentSize);
    socketStream.read((char*)rawBody.data(), contentSize);
  }
  bodyRead = true;
}

void HTTPClient::Response::readChunkedBody() {
  // RFC 7230 §4.1: each chunk is a hex size line (optional ";extension",
  // ignored here) + CRLF, then exactly that many body bytes + CRLF, ending
  // with a "0" size chunk, then optional trailer headers (ignored, this
  // client doesn't need them) up to the final empty line. §48 - was
  // entirely unhandled before (readRawBody() only knew about
  // Content-Length, so a chunked response silently yielded an empty body).
  rawBody.clear();
  
  // FIX: Se incrementó el tamaño del buffer de 64 a 256. 
  // Esto previene que si un proxy o CDN decide agregar extensiones de chunk 
  // (ej. "1a2b;trace-id=abcdef..."), el getline() sobrepase el buffer, lance 
  // un falso timeout y corte abruptamente la conexión.
  std::vector<char> lineBuf(256);

  while (true) {
    socketStream.getline(lineBuf.data(), lineBuf.size());
    if (socketStream.fail()) {
      throw std::runtime_error(
          "Connection closed while reading chunk size");
    }

    std::string sizeLine(lineBuf.data());
    size_t extSep = sizeLine.find(';');
    if (extSep != std::string::npos) {
      sizeLine = sizeLine.substr(0, extSep);
    }

    size_t chunkSize;
    try {
      // stoul() stops at the first non-hex character (e.g. a trailing
      // "\r" left by getline(), which only strips the "\n" delimiter) -
      // no need to trim it manually.
      chunkSize = std::stoul(sizeLine, nullptr, 16);
    } catch (const std::exception&) {
      throw std::runtime_error("Malformed chunk size in HTTP response");
    }

    if (chunkSize == 0) {
      // Final chunk - drain any trailer headers up to the terminating
      // empty line, then done.
      while (true) {
        socketStream.getline(lineBuf.data(), lineBuf.size());
        if (socketStream.fail()) {
          throw std::runtime_error(
              "Connection closed while reading chunk trailer");
        }
        std::string trailerLine(lineBuf.data());
        if (trailerLine.empty() || trailerLine == "\r") {
          break;
        }
      }
      break;
    }

    size_t oldSize = rawBody.size();
    rawBody.resize(oldSize + chunkSize);
    socketStream.read((char*)rawBody.data() + oldSize, chunkSize);
    if (socketStream.fail()) {
      throw std::runtime_error("Connection closed while reading chunk data");
    }

    char crlf[2];
    socketStream.read(crlf, 2);
    if (socketStream.fail()) {
      throw std::runtime_error(
          "Connection closed while reading chunk terminator");
    }
  }
}

std::string_view HTTPClient::Response::body() {
  readRawBody();
  return std::string_view((char*)rawBody.data(), rawBody.size());
}

std::vector<uint8_t> HTTPClient::Response::bytes() {
  readRawBody();
  return rawBody;
}