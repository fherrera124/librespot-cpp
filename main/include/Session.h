#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <vector>

#include "AudioSink.h"
#include "AuthInfo.h"
#include "ConnectStateHandler.h"
#include "api/CredentialsResolver.h"
#include "api/DealerClient.h"
#include "api/SpClient.h"
#include "bell/Result.h"
#include "bell/net/SocketPollListener.h"
#include "events/EventLoop.h"
#include "proto/MetadataPb.h"
#include "tracks/StreamPlayer.h"

namespace cspot {

// Tuning for the CDN fetch/decode pipeline - see AudioDecoder.h and
// FileProvider.h for what each field controls. Namespace-scope, not nested
// in Session: a nested struct's default member initializers can't build a
// default *argument* value for the enclosing class's own constructor (the
// enclosing class isn't complete yet at that point).
struct AudioConfig {
  size_t prefetchDepth = 2;
  std::chrono::milliseconds targetChunkDuration{6500};
  // Tried in order against each track's offered qualities - first match wins.
  std::vector<AudioFormat> qualityPreference = {AudioFormat_OGG_VORBIS_320,
                                                AudioFormat_OGG_VORBIS_160,
                                                AudioFormat_OGG_VORBIS_96};
};

class Session {
 public:
  // audioSink/playbackNotificationCallback default to no-ops so host
  // targets that don't care about a given one don't need to pass anything.
  Session(std::shared_ptr<AuthInfo> authInfo,
          std::shared_ptr<AudioSink> audioSink =
              std::make_shared<NullAudioSink>(),
          cspot::PlaybackNotificationCallback playbackNotificationCallback =
              [](const PlaybackNotificationEvent&) {},
          AudioConfig audioConfig = AudioConfig());

  bell::Result<> start();

  // Blocks polling the AP/dealer sockets forever, UNLESS restartRequested
  // is set or the AP declines our login - at which point this returns so
  // the caller can rebuild the Session (a fresh pairing) or give up on
  // these credentials (see credentialsRejected()). Each poll() call blocks
  // up to the next subsystem deadline (ping/pong watchdogs, reconnect
  // backoff) rather than a fixed tick - a caller setting restartRequested
  // from another thread must also call wake() (see below) to be noticed
  // promptly instead of waiting for that deadline.
  void runPoller(std::atomic<bool>& restartRequested);

  // Interrupts a blocking runPoller() immediately - for callers that set
  // restartRequested from a thread other than the one running runPoller().
  // Safe to call from any thread.
  void wake();

  // Forwards to ConnectStateHandler::putInactive(). Callers tearing this
  // Session down to rebuild a new one should call this first, best-effort.
  bell::Result<> putInactive();

  // True if runPoller() returned because the AP explicitly rejected our
  // login credentials - see ApClient::loginWasDeclined()'s own comment.
  bool credentialsRejected() const;

  // --- Local control ---
  // Thin passthrough to connectStateHandler - see
  // ConnectStateHandler::requestPlayPause() et al. for the actual
  // semantics/thread-safety. connectStateHandler is always valid once a
  // Session exists, so there's no "not ready yet" case to handle here.
  bool requestPlayPause(bool play);
  bool requestNext();
  bool requestPrevious();
  bool requestSeek(uint32_t positionMs);
  bool requestSetRepeatContext(bool enabled);
  uint32_t getPositionMs();

 private:
  const char* LOG_TAG = "Session";

  std::shared_ptr<cspot::AuthInfo> authInfo;
  std::shared_ptr<cspot::EventLoop> eventLoop;
  std::shared_ptr<bell::SocketPollListener> socketPoll;
  std::shared_ptr<cspot::CredentialsResolver> credentialsResolver;
  std::shared_ptr<cspot::DealerClient> dealerClient;
  std::shared_ptr<cspot::SpClient> spClient;
  std::shared_ptr<cspot::ApClient> apClient;
  std::shared_ptr<cspot::ConnectStateHandler> connectStateHandler;
  std::shared_ptr<cspot::StreamPlayer> streamPlayer;

  // Dealer reconnect backoff state, driven from runPoller(). Matches
  // master's DealerSession constants (5s base, doubling, 60s cap) - see
  // connectDealer()'s own comment for why we retry forever instead of
  // giving up like go-librespot's default ~15min MaxElapsedTime.
  int dealerBackoffMs = 5000;
  // Earliest time runPoller() may call connectDealer() again after seeing
  // dealerClient in Disconnected/Failed - not consulted while it's
  // Connecting, since that state itself (not a timer) is what prevents
  // runPoller() from preempting an in-flight WS handshake.
  std::chrono::steady_clock::time_point nextDealerReconnectAttempt{};

  // Same backoff pattern as the dealer's, applied to the AP connection -
  // see ApClient::State/doHousekeeping() and ApConnection::disconnect().
  int apBackoffMs = 5000;
  std::chrono::steady_clock::time_point nextApReconnectAttempt{};

  // Resolves the dealer address + access key and connects dealerClient.
  // Used both for the initial connect (start()) and for every reconnect
  // attempt from runPoller() - re-resolving each time (rather than reusing
  // stale values) mirrors master's "never cache the URL" dealer reconnect.
  bell::Result<> connectDealer();

  // Resolves the AP address and connects/authenticates apClient. Used both
  // for the initial connect (start()) and for every reconnect attempt from
  // runPoller(), same shape as connectDealer().
  bell::Result<> connectAp();

  void handleDealerMessage(EventLoop::Event&& event);
  void handleDealerRequest(EventLoop::Event&& event);
};
}  // namespace cspot
