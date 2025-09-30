#pragma once

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
  Session(std::shared_ptr<AuthInfo> authInfo);

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

  void handleDealerMessage(EventLoop::Event&& event);
  void handleDealerRequest(EventLoop::Event&& event);
};
}  // namespace cspot
