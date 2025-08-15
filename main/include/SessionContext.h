#pragma once

#include "LoginBlob.h"
#include "api/CredentialsResolver.h"
#include "bell/net/SocketPollListener.h"
#include "events/EventLoop.h"

namespace cspot {
struct SessionContext {
  std::shared_ptr<LoginBlob> loginBlob;
  std::shared_ptr<EventLoop> eventLoop;
  std::shared_ptr<CredentialsResolver> credentialsResolver;

  // Socket poll, used to multiplex the dealer and ap connection sockets
  bell::SocketPollListener socketPoll;

  std::string sessionId;
};
}  // namespace cspot
