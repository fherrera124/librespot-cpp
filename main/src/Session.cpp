#include "Session.h"

#include <algorithm>
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
                        cspot::AudioOutputCallback audioOutputCallback,
                        cspot::VolumeChangedCallback volumeChangedCallback)
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
      eventLoop, this->authInfo, spClient, std::move(volumeChangedCallback));

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

    // This, not the WS connect itself, is what makes the device
    // selectable in the app - confirmed against both go-librespot
    // (daemon/player.go) and this repo's own master branch
    // (DealerSession.cpp), which both use NEW_DEVICE here, not
    // NEW_CONNECTION.
    auto res = connectStateHandler->putState(PutStateReason_NEW_DEVICE);
    if (!res) {
      BELL_LOG(error, LOG_TAG, "Failed to announce connect state: {}",
               res.error());
      return;
    }
  } else if (uri->starts_with("hm://connect-state/v1/cluster")) {
    auto payloadsField = messageJson.find("payloads");
    auto* payloads = payloadsField ? payloadsField->find(0) : nullptr;
    if (!payloads) {
      BELL_LOG(error, LOG_TAG, "Cluster update without a payload");
      return;
    }

    auto res =
        connectStateHandler->handleClusterUpdate(payloads->get_string());
    if (!res) {
      BELL_LOG(error, LOG_TAG, "Failed to handle cluster update: {}",
               res.error());
    }
  } else if (uri->starts_with("hm://connect-state/v1/connect/volume")) {
    auto payloadsField = messageJson.find("payloads");
    auto* payloads = payloadsField ? payloadsField->find(0) : nullptr;
    if (!payloads) {
      BELL_LOG(error, LOG_TAG, "Volume command without a payload");
      return;
    }

    auto res = connectStateHandler->handleSetVolume(payloads->get_string());
    if (!res) {
      BELL_LOG(error, LOG_TAG, "Failed to handle set volume: {}",
               res.error());
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

bell::Result<> cspot::Session::connectDealer() {
  // Re-resolved on every call (initial connect and every reconnect alike),
  // never cached here - both cache internally with their own expiry, so
  // this is cheap when still valid and self-refreshes when not. Mirrors
  // master's DealerSession::connectOnce() comment: "Never cache the URL:
  // re-resolves the host and re-fetches the token every attempt".
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

  auto dealerConnectRes =
      dealerClient->connect(*dealerAddressRes, *accessKeyRes, socketPoll);
  if (!dealerConnectRes) {
    BELL_LOG(error, LOG_TAG, "Failed to connect to dealer client: {}",
             dealerConnectRes.error());
    return dealerConnectRes;
  }

  return {};
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

  return connectDealer();
}

void cspot::Session::runPoller() {
  constexpr int kDealerBackoffBaseMs = 5000;
  constexpr int kDealerBackoffMaxMs = 60000;

  while (true) {
    socketPoll->poll(1000);
    dealerClient->doHousekeeping();

    if (!dealerClient->isConnected()) {
      auto now = std::chrono::steady_clock::now();
      if (now >= nextDealerReconnectAttempt) {
        BELL_LOG(info, LOG_TAG, "Dealer disconnected, reconnecting...");
        auto reconnectRes = connectDealer();
        if (reconnectRes) {
          BELL_LOG(info, LOG_TAG, "Dealer reconnected");
          dealerBackoffMs = kDealerBackoffBaseMs;
        } else {
          BELL_LOG(error, LOG_TAG, "Dealer reconnect failed, retrying in {}ms",
                   dealerBackoffMs);
          nextDealerReconnectAttempt =
              now + std::chrono::milliseconds(dealerBackoffMs);
          dealerBackoffMs =
              std::min(dealerBackoffMs * 2, kDealerBackoffMaxMs);
        }
      }
    }
  }
}
