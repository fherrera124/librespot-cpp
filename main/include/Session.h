#pragma once

#include <chrono>
#include <memory>

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
  // audioOutputCallback/volumeChangedCallback default to no-ops so host
  // targets (no audio support yet) don't need to pass anything - only
  // ESP32 targets inject real ones (see targets/esp32/main/main.cpp).
  Session(std::shared_ptr<AuthInfo> authInfo,
          cspot::AudioOutputCallback audioOutputCallback =
              [](tcb::span<const std::byte>, const SpotifyId&) {},
          cspot::VolumeChangedCallback volumeChangedCallback =
              [](uint16_t) {});

  bell::Result<> start();

  void runPoller();

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

  // Resolves the dealer address + access key and connects dealerClient.
  // Used both for the initial connect (start()) and for every reconnect
  // attempt from runPoller() - re-resolving each time (rather than reusing
  // stale values) mirrors master's "never cache the URL" dealer reconnect.
  bell::Result<> connectDealer();

  void handleDealerMessage(EventLoop::Event&& event);
  void handleDealerRequest(EventLoop::Event&& event);
};
}  // namespace cspot
