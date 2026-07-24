#include "Session.h"

#include <string>
#include <tao/json.hpp>
#include <tao/json/traits.hpp>
#include "api/CredentialsResolver.h"
#include "api/DealerClient.h"
#include "api/SpClient.h"
#include "bell/Logger.h"
#include "connect.pb.h"
#include "events/EventLoop.h"

using namespace cspot;

cspot::Session::Session(std::shared_ptr<AuthInfo> authInfo,
                        cspot::AudioOutputCallback audioOutputCallback)
    : authInfo(std::move(authInfo)) {
  // Prepare the session context
  eventLoop = std::make_shared<cspot::EventLoop>();
  socketPoll = std::make_shared<bell::SocketPollListener>();
  credentialsResolver = createDefaultCredentialsResolver(
      std::make_shared<bell::HTTPClient>(), this->authInfo);
  spClient = createDefaultSpClient(std::make_shared<bell::HTTPClient>(),
                                   credentialsResolver);
  dealerClient = std::make_shared<DealerClient>(eventLoop);
  apClient = std::make_unique<ApClient>(eventLoop, this->authInfo);

  connectStateHandler = std::make_shared<ConnectStateHandler>(
      eventLoop, this->authInfo, spClient);

  auto fileProvider = createDefaultFileProvider(eventLoop, spClient, apClient);
  auto audioDecoder = createAudioDecoder(std::move(audioOutputCallback));
  streamPlayer = std::make_shared<StreamPlayer>(
      eventLoop, std::move(fileProvider), std::move(audioDecoder));

  eventLoop->registerHandler(EventLoop::EventType::DEALER_MESSAGE,
                             std::bind(&cspot::Session::handleDealerMessage,
                                       this, std::placeholders::_1));

  eventLoop->registerHandler(EventLoop::EventType::DEALER_REQUEST,
                             std::bind(&cspot::Session::handleDealerRequest,
                                       this, std::placeholders::_1));
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

    authInfo->sessionId = *sessionId;
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

  auto apAddressRes = credentialsResolver->getApAddress(
      CredentialsResolver::AddressType::AccessPoint);
  if (!apAddressRes) {
    BELL_LOG(error, LOG_TAG, "Failed to resolve ap address: {}",
             apAddressRes.error());
    return tl::make_unexpected(apAddressRes.error());
  }

  // Start the ap client
  auto res = apClient->connectAndAuthenticate(*apAddressRes, socketPoll);
  if (!res) {
    BELL_LOG(error, LOG_TAG, "Failed to connect to AP: {}", res.error());
    return res;
  }

  auto dealerAddressRes = credentialsResolver->getApAddress(
      CredentialsResolver::AddressType::Dealer);
  if (!dealerAddressRes) {
    BELL_LOG(error, LOG_TAG, "Failed to resolve dealer address: {}",
             dealerAddressRes.error());
    return tl::make_unexpected(dealerAddressRes.error());
  }

  auto accessKeyRes = credentialsResolver->getAccessKey();
  if (!accessKeyRes) {
    BELL_LOG(error, LOG_TAG, "Failed to resolve access key: {}",
             accessKeyRes.error());
    return tl::make_unexpected(accessKeyRes.error());
  }
  // Start the dealer client
  auto dealerConnectRes =
      dealerClient->connect(*dealerAddressRes, *accessKeyRes, socketPoll);
  if (!dealerConnectRes) {
    BELL_LOG(error, LOG_TAG, "Failed to connect to dealer client: {}",
             dealerConnectRes.error());
    return dealerConnectRes;
  }

  return {};
}

void cspot::Session::runPoller() {
  while (true) {
    socketPoll->poll(1000);
    dealerClient->doHousekeeping();
  }
}
