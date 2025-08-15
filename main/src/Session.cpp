#include "Session.h"

#include <string>
#include <tao/json.hpp>
#include <tao/json/traits.hpp>
#include "bell/Logger.h"
#include "connect.pb.h"
#include "events/EventLoop.h"

using namespace cspot;

cspot::Session::Session(std::shared_ptr<LoginBlob> loginBlob)
    : loginBlob(std::move(loginBlob)) {
  // Prepare the session context
  sessionContext = std::make_shared<SessionContext>();
  sessionContext->loginBlob = this->loginBlob;
  sessionContext->eventLoop = std::make_shared<cspot::EventLoop>();
  sessionContext->credentialsResolver =
      std::make_shared<CredentialsResolver>(this->loginBlob);

  // Prepare the dealer client
  dealerClient = std::make_shared<DealerClient>(sessionContext);
  spClient = std::make_shared<SpClient>(sessionContext);
  apClient = std::make_shared<ApClient>(sessionContext);
  connectStateHandler =
      std::make_shared<ConnectStateHandler>(sessionContext, spClient, apClient);

  sessionContext->eventLoop->registerHandler(
      EventLoop::EventType::DEALER_MESSAGE,
      std::bind(&cspot::Session::handleDealerMessage, this,
                std::placeholders::_1));

  sessionContext->eventLoop->registerHandler(
      EventLoop::EventType::DEALER_REQUEST,
      std::bind(&cspot::Session::handleDealerRequest, this,
                std::placeholders::_1));
}

void cspot::Session::handleDealerMessage(EventLoop::Event&& event) {
  auto dealerMessageEvent = std::move(event);
  auto& messageJson = std::get<tao::json::value>(dealerMessageEvent.payload);

  auto uri = messageJson.optional<std::string>("uri");

  if (!uri) {
    BELL_LOG(info, LOG_TAG, "Received message without URI");
    return;
  }

  if (uri->starts_with("hm://pusher/v1/connections")) {
    // Extract session ID
    auto headers = messageJson.at("headers");

    auto sessionId = headers.optional<std::string>("Spotify-Connection-Id");
    if (!sessionId) {
      BELL_LOG(info, LOG_TAG, "Received message without session ID");
      return;
    }

    sessionContext->sessionId = *sessionId;
    BELL_LOG(info, LOG_TAG, "Session ID: {}", *sessionId);

    // Announce spotify connect state
    auto res = connectStateHandler->putState(PutStateReason_NEW_CONNECTION);
    if (!res) {
      BELL_LOG(error, LOG_TAG, "Failed to announce connect state: {}",
               res.error());
      return;
    }
  } else {
    BELL_LOG(info, LOG_TAG, "Received message with URI: {}", *uri);
  }
}

void cspot::Session::handleDealerRequest(EventLoop::Event&& event) {
  auto dealerRequestEvent = std::move(event);
  auto& messageJson = std::get<tao::json::value>(dealerRequestEvent.payload);

  auto messageIdent = messageJson.optional<std::string>("message_ident");
  if (!messageIdent) {
    BELL_LOG(info, LOG_TAG, "Received message without message_ident");
    return;
  }

  auto requestKey = messageJson.optional<std::string>("key");
  if (!requestKey) {
    BELL_LOG(info, LOG_TAG, "Received message without request key");
    return;
  }

  bool requestSuccess = false;

  if (messageIdent == "hm://connect-state/v1/player/command") {
    auto res = connectStateHandler->handlePlayerCommand(messageJson);
    if (!res) {
      BELL_LOG(error, LOG_TAG, "Failed to handle player command: {}",
               res.error());
      requestSuccess = false;
    } else {
      requestSuccess = true;
    }
  }

  auto replyRes = dealerClient->replyToRequest(requestSuccess, *requestKey);
  if (!replyRes) {
    BELL_LOG(error, LOG_TAG, "Failed to reply to dealer request: {}",
             replyRes.error());
  }
}

bell::Result<> cspot::Session::start() {
  // Start the ap client
  auto res = apClient->connectAndAuthenticate();
  if (!res) {
    BELL_LOG(error, LOG_TAG, "Failed to connect to AP: {}", res.error());
    return res;
  }

  // Start the dealer client
  res = dealerClient->connect();
  if (!res) {
    BELL_LOG(error, LOG_TAG, "Failed to connect to dealer client: {}",
             res.error());
    return res;
  }

  return {};
}
