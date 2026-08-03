#pragma once

#include <atomic>
#include <chrono>
#include <memory>

#include "AudioSink.h"
#include "AuthInfo.h"
#include "ConnectStateHandler.h"
#include "api/CredentialsResolver.h"
#include "api/DealerClient.h"
#include "api/SpClient.h"
#include "bell/Result.h"
#include "bell/net/SocketPollListener.h"
#include "events/EventLoop.h"
#include "tracks/StreamPlayer.h"

namespace cspot {
class Session {
 public:
  // audioSink/playbackNotificationCallback default to no-ops so host
  // targets that don't care about a given one don't need to pass anything.
  // prefetchDepth: forwarded to createAudioDecoder() - see its own
  // comment (AudioDecoder.h) for what it controls.
  Session(std::shared_ptr<AuthInfo> authInfo,
          std::shared_ptr<AudioSink> audioSink =
              std::make_shared<NullAudioSink>(),
          cspot::PlaybackNotificationCallback playbackNotificationCallback =
              [](const PlaybackNotificationEvent&) {},
          size_t prefetchDepth = 2);

  bell::Result<> start();

  // Blocks polling the AP/dealer sockets forever, UNLESS restartRequested
  // is set (checked once per ~1s poll tick) or the AP declines our login -
  // at which point this returns so the caller can rebuild the Session (a
  // fresh pairing) or give up on these credentials (see
  // credentialsRejected()).
  void runPoller(std::atomic<bool>& restartRequested);

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
