#include "CDNAudioFile.h"

#include <string.h>          // for memcpy
#include <algorithm>          // for min
#include <chrono>            // for steady_clock, used to time each fetch
#include <functional>        // for __base
#include <initializer_list>  // for initializer_list
#include <map>               // for operator!=, operator==
#include <stdexcept>         // for runtime_error
#include <string_view>       // for string_view
#include <type_traits>       // for remove_extent_t

#include "AccessKeyFetcher.h"  // for AccessKeyFetcher
#include "BellLogger.h"        // for AbstractLogger
#include "Crypto.h"
#include "Logger.h"            // for CSPOT_LOG
#include "Packet.h"            // for cspot
#include "SocketStream.h"      // for SocketStream
#include "URLParser.h"         // for URLParser::parse - host comparison in fetchRange()
#include "Utils.h"             // for bigNumAdd, bytesToHexString, string...
#include "WrappedSemaphore.h"  // for WrappedSemaphore
#ifdef BELL_ONLY_CJSON
#include "cJSON.h"
#else
#include "nlohmann/json.hpp"      // for basic_json<>::object_t, basic_json
#include "nlohmann/json_fwd.hpp"  // for json
#endif

using namespace cspot;

CDNAudioFile::CDNAudioFile(const std::string& cdnUrl,
                           const std::vector<uint8_t>& audioKey,
                           int bitrateKbps, CDNConnection& connection)
    : connection(connection), cdnUrl(cdnUrl), audioKey(audioKey) {
  this->crypto = std::make_unique<Crypto>();
  httpBuffer.resize(
      static_cast<size_t>(bitrateKbps * 1000.0 / 8.0 * HTTP_BUFFER_SECONDS));
}

size_t CDNAudioFile::getPosition() {
  return this->position;
}

void CDNAudioFile::seek(size_t newPos) {
  this->enableRequestMargin = true;
  this->position = newPos;
}

void CDNAudioFile::openStream() {
  CSPOT_LOG(info, "Opening HTTP stream to %s", this->cdnUrl.c_str());

  // Reuses `connection` (kept alive across tracks - see its own comment)
  // when eligible, instead of the fresh connection every track used to
  // pay unconditionally here. Not wrapped in try/catch: a fresh-connect
  // failure propagates out to TrackPlayer's own catch, which skips the
  // track - same as this call always did before.
  bool headerReused = fetchRange(
      this->cdnUrl,
      {bell::HTTPClient::RangeHeader::range(0, OPUS_HEADER_SIZE - 1)});

  // A server is allowed to ignore Range and answer 200 with the full file
  // instead of 206 with just the requested slice - would silently read the
  // wrong bytes as "the header" below otherwise. See F85/F86.
  if (this->connection.response->statusCode() != 206) {
    CSPOT_LOG(error, "CDN header request got HTTP %d instead of 206",
              this->connection.response->statusCode());
    throw std::runtime_error("CDN did not honor header range request");
  }

  this->connection.response->stream().read((char*)header.data(), OPUS_HEADER_SIZE);
  // std::istream::read() doesn't throw on a short read by default - it
  // just leaves fewer bytes than requested and sets failbit, silently.
  // Without this check, a truncated read (network hiccup, or the
  // connection being shutdown() out from under it - see TrackPlayer::
  // resetState()) would continue on with a partially-filled header
  // buffer instead of raising a catchable error.
  if (this->connection.response->stream().gcount() != OPUS_HEADER_SIZE) {
    throw std::runtime_error("CDN header read truncated");
  }

  // A short/truncated HTTP response (declared Content-Length smaller than
  // the Spotify Opus header) would underflow this size_t subtraction into
  // a huge value, which later drives an equally huge vector allocation
  // below. See docs/spotify_component_analysis.md, finding F27.
  size_t declaredLength = this->connection.response->totalLength();
  if (declaredLength < static_cast<size_t>(SPOTIFY_OPUS_HEADER)) {
    CSPOT_LOG(error, "CDN response too short (%u bytes)",
              (unsigned)declaredLength);
    throw std::runtime_error("CDN response shorter than expected header");
  }
  this->totalFileSize = declaredLength - SPOTIFY_OPUS_HEADER;

  this->decrypt(header.data(), OPUS_HEADER_SIZE, 0);

  // Location must be dividable by 16
  size_t footerStartLocation =
      (this->totalFileSize - OPUS_FOOTER_PREFFERED + SPOTIFY_OPUS_HEADER) -
      (this->totalFileSize - OPUS_FOOTER_PREFFERED + SPOTIFY_OPUS_HEADER) % 16;

  this->footer = std::vector<uint8_t>(
      this->totalFileSize - footerStartLocation + SPOTIFY_OPUS_HEADER);
  fetchRange(cdnUrl, {bell::HTTPClient::RangeHeader::last(footer.size())});

  if (this->connection.response->statusCode() != 206) {
    CSPOT_LOG(error, "CDN footer request got HTTP %d instead of 206",
              this->connection.response->statusCode());
    throw std::runtime_error("CDN did not honor footer range request");
  }

  this->connection.response->stream().read((char*)footer.data(),
                                            this->footer.size());
  if (static_cast<size_t>(this->connection.response->stream().gcount()) !=
      this->footer.size()) {
    throw std::runtime_error("CDN footer read truncated");
  }

  this->decrypt(footer.data(), footer.size(), footerStartLocation);
  // headerReused=1 here is the direct signal that this track's connection
  // opening didn't pay a fresh TLS handshake - it reused the previous
  // track's still-warm CDN connection (docs/dealer_websocket_migration.md
  // §21). Always 0 for the first track of a session (nothing to reuse
  // yet); should be 1 from the second track on.
  CSPOT_LOG(info, "Header and footer bytes received (headerReused=%d)",
           (int)headerReused);
  this->position = 0;
  this->lastRequestPosition = 0;
  this->lastRequestCapacity = 0;
}

NormalizationData CDNAudioFile::getNormalizationData() const {
  NormalizationData data{};
  memcpy(&data.trackGainDb, header.data() + SPOTIFY_NORMALIZATION_GAIN_OFFSET,
        sizeof(float));
  memcpy(&data.trackPeak, header.data() + SPOTIFY_NORMALIZATION_PEAK_OFFSET,
        sizeof(float));
  return data;
}

size_t CDNAudioFile::readBytes(uint8_t* dst, size_t bytes) {
  size_t offsetPosition = position + SPOTIFY_OPUS_HEADER;
  size_t actualFileSize = this->totalFileSize + SPOTIFY_OPUS_HEADER;

  if (position + bytes >= this->totalFileSize) {
    return 0;
  }

  // // Opus tries to read header, use prefetched data
  if (offsetPosition < OPUS_HEADER_SIZE &&
      bytes + offsetPosition <= OPUS_HEADER_SIZE) {
    memcpy(dst, this->header.data() + offsetPosition, bytes);
    position += bytes;
    return bytes;
  }

  // // Opus tries to read footer, use prefetched data
  if (offsetPosition >= (actualFileSize - this->footer.size())) {
    size_t toReadBytes = bytes;

    if ((position + bytes) > this->totalFileSize) {
      // Tries to read outside of bounds, truncate
      toReadBytes = this->totalFileSize - position;
    }

    size_t footerOffset =
        offsetPosition - (actualFileSize - this->footer.size());
    memcpy(dst, this->footer.data() + footerOffset, toReadBytes);

    position += toReadBytes;
    return toReadBytes;
  }

  // Data not in the headers. Make sense of whats going on.
  // Position in bounds :)
  if (offsetPosition >= this->lastRequestPosition &&
      offsetPosition < this->lastRequestPosition + this->lastRequestCapacity) {
    size_t toRead = bytes;

    if ((toRead + offsetPosition) >
        this->lastRequestPosition + lastRequestCapacity) {
      toRead = this->lastRequestPosition + lastRequestCapacity - offsetPosition;
    }

    memcpy(dst, this->httpBuffer.data() + offsetPosition - lastRequestPosition,
           toRead);
    position += toRead;

    return toRead;
  } else {
    size_t requestPosition = (offsetPosition) - ((offsetPosition) % 16);
    if (this->enableRequestMargin && requestPosition > SEEK_MARGIN_SIZE) {
      requestPosition = (offsetPosition - SEEK_MARGIN_SIZE) -
                        ((offsetPosition - SEEK_MARGIN_SIZE) % 16);
      this->enableRequestMargin = false;
    }

    auto rangeHeader = bell::HTTPClient::RangeHeader::range(
        requestPosition, requestPosition + httpBuffer.size() - 1);

    // Temporary diagnostic for the periodic audio dropout investigation -
    // see docs/spotify_component_analysis.md. Measures the full CDN
    // round-trip (connect/reuse + range request + body read) against the
    // downstream buffering margin.
    auto fetchStart = std::chrono::steady_clock::now();

    bool reused;
    try {
      reused = fetchRange(cdnUrl, {rangeHeader});
    } catch (const std::exception& e2) {
      CSPOT_LOG(error, "CDN reconnect failed: %s", e2.what());
      return 0;
    }

    // A server (or a proxy/edge in between) is allowed by the HTTP spec to
    // ignore a Range request and answer 200 with the full resource instead
    // of 206 with just the requested slice - librespot explicitly checks
    // for this too. Without this check, a 200 response's Content-Length
    // (the whole remaining file) would be trusted as-is below. See F85/F86.
    if (this->connection.response->statusCode() != 206) {
      CSPOT_LOG(error, "CDN range request got HTTP %d instead of 206",
                this->connection.response->statusCode());
      return 0;
    }

    this->lastRequestPosition = requestPosition;
    // Hard backstop regardless of the 206 check above: never trust the
    // server's declared length past what httpBuffer can actually hold -
    // the read() right below writes exactly this many bytes into it.
    this->lastRequestCapacity =
        std::min(this->connection.response->contentLength(), httpBuffer.size());

    try {
      this->connection.response->stream().read((char*)this->httpBuffer.data(),
                                                lastRequestCapacity);
      // See openStream()'s header/footer reads for why this check exists -
      // a short read here doesn't throw on its own by default.
      if (static_cast<size_t>(this->connection.response->stream().gcount()) !=
          lastRequestCapacity) {
        throw std::runtime_error("CDN range read truncated");
      }
    } catch (const std::exception& e) {
      CSPOT_LOG(error, "CDN read failed after reconnect: %s", e.what());
      return 0;
    }
    this->decrypt(this->httpBuffer.data(), lastRequestCapacity,

                  this->lastRequestPosition);

    auto fetchMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - fetchStart)
                       .count();
    CSPOT_LOG(info, "CDN fetch: %lldms, %u bytes, reused=%d",
              (long long)fetchMs, (unsigned)lastRequestCapacity, (int)reused);

    return readBytes(dst, bytes);
  }

  return bytes;
}

size_t CDNAudioFile::getSize() {
  return this->totalFileSize;
}

void CDNAudioFile::decrypt(uint8_t* dst, size_t nbytes, size_t pos) {
  auto calculatedIV = bigNumAdd(audioAESIV, pos / 16);

  this->crypto->aesCTRXcrypt(this->audioKey, calculatedIV, dst, nbytes);
}

bool CDNAudioFile::fetchRange(const std::string& url,
                              const bell::HTTPClient::Headers& headers) {
  auto host = bell::URLParser::parse(url).host;
  bool sameHost = connection.response != nullptr && connection.host == host;

  bool reused = false;
  if (sameHost) {
    try {
      connection.response->get(url, headers);
      reused = true;
    } catch (const std::exception& e) {
      // A stop/reset in progress (TrackPlayer::resetState()) is why this
      // connection just failed in the first place - shutdown()'d out from
      // under this exact call. Reconnecting fresh here would silently
      // retry the same request and defeat that interruption entirely.
      if (connection.shouldAbort && connection.shouldAbort()) {
        throw;
      }
      CSPOT_LOG(info,
               "CDN request failed on existing connection (%s), reconnecting...",
               e.what());
    }
  } else if (connection.response != nullptr) {
    // Not seen in practice (every track this session used the same
    // audio-ak.spotifycdn.com edge) but bell::HTTPClient::Response::get()
    // itself doesn't check for this - it only reconnects when the socket
    // looks dead, so trusting a reuse across a host change would send the
    // new track's request to the wrong server. See CDNConnection's comment.
    CSPOT_LOG(info, "CDN host changed (%s -> %s), reconnecting...",
             connection.host.c_str(), host.c_str());
  }

  if (!reused) {
    // Not caught here - propagates to the caller. openStream() lets it
    // through (TrackPlayer's own catch skips the track, same as before
    // this existed); readBytes() catches it itself and returns 0.
    connection.response = bell::HTTPClient::get(url, headers);
    connection.host = host;
  }

  // Only the body reads remain from here - record the fd now so the
  // owner (TrackPlayer::resetState(), called cross-thread) can
  // shutdown() it if a stop/reset arrives while one of those blocks.
  if (connection.activeFd) {
    connection.activeFd->store(connection.response->stream().getFd());
  }

  return reused;
}
