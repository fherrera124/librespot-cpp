#pragma once

#include <memory>

#include "ConnectStateHandler.h"
#include "LoginBlob.h"
#include "SessionContext.h"
#include "api/DealerClient.h"
#include "api/SpClient.h"
#include "bell/Result.h"
#include "events/EventLoop.h"

namespace cspot {
class Session {
 public:
  Session(std::shared_ptr<LoginBlob> loginBlob);

  bell::Result<> start();

  void runPoller() { sessionContext->socketPoll.poll(); }

 private:
  const char* LOG_TAG = "Session";

  std::shared_ptr<LoginBlob> loginBlob;
  std::shared_ptr<SessionContext> sessionContext;
  std::shared_ptr<DealerClient> dealerClient;
  std::shared_ptr<SpClient> spClient;
  std::shared_ptr<ApClient> apClient;
  std::shared_ptr<ConnectStateHandler> connectStateHandler;

  void handleDealerMessage(EventLoop::Event&& event);
  void handleDealerRequest(EventLoop::Event&& event);
};
}  // namespace cspot
