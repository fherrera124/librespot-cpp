#pragma once

#include <cstddef>  // for size_t
#include <cstdint>  // for uint8_t
#include <memory>   // for shared_ptr, unique_ptr
#include <string>   // for string
#include <vector>   // for vector

#include "Crypto.h"      // for Crypto
#include "HTTPClient.h"  // for HTTPClient

namespace bell {
class WrappedSemaphore;
}  // namespace bell

namespace cspot {
class AccessKeyFetcher;

// A CDN connection (TCP+TLS session to Spotify's audio CDN) kept alive
// across tracks, not just within one - see docs/dealer_websocket_
// migration.md §21. One instance, owned by TrackPlayer (which processes
// tracks strictly one at a time, so there's never a concurrency conflict
// over it), handed to each track's CDNAudioFile by reference. Before this,
// every track paid its own fresh TLS handshake even though it's almost
// always the same CDN edge as the previous one.
struct CDNConnection {
  std::unique_ptr<bell::HTTPClient::Response> response;
  // Host `response` is currently connected to, if any. HTTPClient::
  // Response::get()/rawRequest() only reconnects when the socket itself
  // looks dead, not when the URL's host changed - every track observed on
  // real hardware this session used the same audio-ak.spotifycdn.com
  // edge, but that's not guaranteed (CDN failover), so CDNAudioFile checks
  // this explicitly before ever trusting a reuse instead of assuming it.
  std::string host;
};

class CDNAudioFile {

 public:
  CDNAudioFile(const std::string& cdnUrl, const std::vector<uint8_t>& audioKey,
              int bitrateKbps, CDNConnection& connection);

  /**
  * @brief Opens connection to the provided cdn url, and fetches track metadata.
  */
  void openStream();

  /**
  * @brief Read and decrypt part of the cdn stream
  *
  * @param dst buffer where to read received data to
  * @param amount of bytes to read
  *
  * @returns amount of bytes read
  */
  size_t readBytes(uint8_t* dst, size_t bytes);

  /**
  * @brief Returns current position in CDN stream
  */
  size_t getPosition();

  /**
  * @brief returns total size of the audio file in bytes
  */
  size_t getSize();

  /**
  * @brief Seeks the track to provided position
  * @param position position where to seek the track
  */
  void seek(size_t position);

 private:
  const int OPUS_HEADER_SIZE = 8 * 1024;
  const int OPUS_FOOTER_PREFFERED = 1024 * 12;  // 12K should be safe
  const int SEEK_MARGIN_SIZE = 1024 * 4;

  // Target seconds of audio per CDN range-request, independent of
  // bitrate. Retune by changing only this constant - see F83/F84.
  static constexpr double HTTP_BUFFER_SECONDS = 2.4;
  const int SPOTIFY_OPUS_HEADER = 167;

  // Used to store opus metadata, speeds up read
  std::vector<uint8_t> header = std::vector<uint8_t>(OPUS_HEADER_SIZE);
  std::vector<uint8_t> footer;

  // General purpose buffer to read data - sized once in the constructor;
  // httpBuffer.size() is the source of truth for that size elsewhere.
  std::vector<uint8_t> httpBuffer;

  // AES IV for decrypting the audio stream
  const std::vector<uint8_t> audioAESIV = {0x72, 0xe0, 0x67, 0xfb, 0xdd, 0xcb,
                                           0xcf, 0x77, 0xeb, 0xe8, 0xbc, 0x64,
                                           0x3f, 0x63, 0x0d, 0x93};
  std::unique_ptr<Crypto> crypto;

  // Owned by TrackPlayer, outlives this track - see CDNConnection's own
  // comment.
  CDNConnection& connection;

  size_t position = 0;
  size_t totalFileSize = 0;
  // These two describe what's currently sitting in httpBuffer for THIS
  // track's own byte-range addressing - unlike the connection itself,
  // that's genuinely per-track (a fresh CDNAudioFile's default 0/0
  // correctly forces a real range request on its first readBytes(), even
  // though connection.response may already be warm).
  size_t lastRequestPosition = 0;
  size_t lastRequestCapacity = 0;

  bool enableRequestMargin = false;

  std::string cdnUrl;
  std::vector<uint8_t> audioKey;

  void decrypt(uint8_t* dst, size_t nbytes, size_t pos);

  // Issues a ranged GET for `url` against `connection`, reusing it when
  // it's connected to the same host (reconnecting fresh otherwise - also
  // the fallback if a reuse attempt throws, a connection that looked alive
  // but wasn't; proactive idle-based staleness recycling was tried and
  // removed, see docs/dealer_websocket_migration.md §50 - the real fix was
  // TCP keepalive, §49). Centralizes what openStream()'s two fetches (header/
  // footer) and readBytes()'s range fetches all need identically -
  // previously duplicated, and only readBytes() had it at all.
  // Non-reuse-eligible connect failures propagate as exceptions (same
  // contract bell::HTTPClient::get() already has) - callers that need a
  // graceful fallback (readBytes()) catch it themselves; openStream()
  // deliberately doesn't, matching its pre-existing behavior of letting
  // TrackPlayer's own catch skip the track.
  // @returns whether the existing connection was reused (false = a fresh
  // connect just happened)
  bool fetchRange(const std::string& url, const bell::HTTPClient::Headers& headers);
};
}  // namespace cspot
