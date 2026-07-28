#pragma once

#include <atomic>
#include <chrono>
#include <unordered_map>
#include <utility>
#include "AuthInfo.h"
#include "api/ApConnection.h"
#include "events/EventLoop.h"
#include "proto/SpotifyId.h"

namespace cspot {
class ApClient {
 public:
  // Connecting: connectAndAuthenticate() was called, handshake/auth is in
  // flight - neither confirmed nor failed yet.
  // Connected: APWelcome path completed (ApConnection reached
  // CONNECTED_SHANNON and authenticate() succeeded).
  // Failed: the connection ended (read/write error, or doHousekeeping()'s
  // ping watchdog gave up on a silently dead link).
  // Callers (Session::runPoller()) are responsible for reconnecting on
  // Failed, mirroring DealerClient::State.
  enum class State { Connecting, Connected, Failed };

  ApClient(std::shared_ptr<cspot::EventLoop> eventLoop,
           std::shared_ptr<cspot::AuthInfo> authInfo);

  bell::Result<> connectAndAuthenticate(
      const std::string& apAddress,
      const std::shared_ptr<bell::SocketPollListener>& socketPoll);

  bell::Result<> requestAudioKey(const SpotifyId& trackId,
                                 const std::vector<std::byte>& fileId);

  // Times out a connection the AP has gone silent on (no Ping packet in
  // over pingTimeout - see the .cpp), forcing it into Failed so
  // Session::runPoller() can reconnect. A dead peer that never sends a
  // TCP FIN/RST (common on a flaky WiFi/NAT path) would otherwise look
  // connected forever, since nothing else here reads from the AP unless
  // handleRead() actually fires.
  void doHousekeeping();

  State state() const;

  // True once the AP has explicitly rejected the login credentials this
  // connection attempt sent (LoginDeclined packet) - distinct from state()
  // == Failed for every other reason (network error, ping timeout), which
  // Session::runPoller() retries with backoff. Retrying a declined login
  // with the exact same (permanently invalid, e.g. revoked/expired)
  // credentials would just fail identically forever - callers should
  // check this instead of blindly reconnecting. Reset to false at the
  // start of every connectAndAuthenticate() attempt.
  bool loginWasDeclined() const { return loginDeclined; }

  // Empty until the AP sends its CountryCode packet, shortly after
  // connecting. Used to resolve region-restricted tracks to a playable
  // alternative (see FileProvider.cpp).
  const std::string& getCountryCode() const { return countryCode; }

 private:
  const char* LOG_TAG = "ApClient";

  std::shared_ptr<cspot::EventLoop> eventLoop;
  std::shared_ptr<cspot::AuthInfo> authInfo;
  std::unique_ptr<ApConnection> apConnection;

  std::string countryCode;

  // Set from apPacketHandler() on a LoginDeclined packet - see
  // loginWasDeclined()'s own comment.
  std::atomic<bool> loginDeclined{false};

  // Updated whenever a Ping packet arrives (apPacketHandler) and reset with
  // a full grace period at the start of every (re)connect attempt in
  // connectAndAuthenticate() - same pattern as DealerClient's
  // lastPingTime/lastPongTime.
  std::chrono::steady_clock::time_point lastPingTime{};

  uint32_t audioKeySequence = 0;

  std::unordered_map<uint32_t, std::pair<SpotifyId, std::vector<std::byte>>>
      audioKeyRequests;

  void apPacketHandler(uint8_t packetType, const std::byte* data, size_t len);
};
};  // namespace cspot
