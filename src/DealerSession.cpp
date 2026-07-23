#include "DealerSession.h"

#include <algorithm>  // for min
#include <chrono>     // for steady_clock - JSON ping cadence (§25)
#include <cstring>    // for strcmp, strncmp
#include <exception>  // for exception

#include "ApResolve.h"
#include "BellLogger.h"    // for AbstractLogger
#include "BellUtils.h"     // for BELL_SLEEP_MS
#include "PlayerEngine.h"
#include "CSpotContext.h"  // for Context
#include "Crypto.h"        // for Crypto::base64Decode
#include "Login5Client.h"
#include "Logger.h"  // for CSPOT_LOG
#include "WebSocketTransport.h"  // for bell::WebSocketTransport

// See docs/dealer_websocket_migration.md §5.2.
#include "cJSON.h"

using namespace cspot;

namespace {
constexpr int RECEIVE_POLL_MS = 1000;
constexpr int RECONNECT_BACKOFF_BASE_MS = 5000;
constexpr int RECONNECT_BACKOFF_MAX_MS = 60000;

// Application-level keepalive (§25): {"type":"ping"}, separate from the
// transport's own WS-protocol ping. Answered with {"type":"pong"}.
constexpr int JSON_PING_INTERVAL_MS = 30000;
// Missing pong past this (§44) declares the connection stale.
constexpr int JSON_PONG_TIMEOUT_MS = 10000;

// bell::WebSocketTransport::create() tuning (§22/§11/F67).
constexpr uint32_t DEALER_PING_INTERVAL_MS = 30000;
constexpr uint32_t DEALER_PING_TIMEOUT_MS = 7000;
constexpr size_t DEALER_MAX_MESSAGE_SIZE = 256 * 1024;
}  // namespace

DealerSession::DealerSession(std::shared_ptr<cspot::Context> ctx)
    : bell::Task("cspotDealer", 32 * 1024, 1, 0), ctx(ctx) {
  login5 = std::make_shared<Login5Client>(ctx);
  connectState = std::make_unique<PlayerEngine>(ctx, login5);
  commandWorker = std::make_unique<CommandWorker>(
      connectState.get(), &pendingCommands, &pendingReplies);
  startTask();
}

DealerSession::~DealerSession() {
  stop();
}

void DealerSession::stop() {
  // Never touches transport directly - not thread-safe, only runTask()'s
  // own thread may. Its 1000ms receiveMessage() poll notices shouldStop()
  // on its own, no explicit wake needed.
  if (commandWorker) {
    commandWorker->stop();
  }
  if (connectState) {
    connectState->stop();
  }
  stopAndWait();
}

DealerSession::CommandWorker::CommandWorker(
    PlayerEngine* connectState,
    bell::Queue<PendingCommand>* pendingCommands,
    bell::Queue<PendingReply>* pendingReplies)
    : bell::Task("cspotDealerCmd", 32 * 1024, 1, 0),
      connectState(connectState),
      pendingCommands(pendingCommands),
      pendingReplies(pendingReplies) {
  startTask();
}

DealerSession::CommandWorker::~CommandWorker() {
  stop();
}

void DealerSession::CommandWorker::stop() {
  stopAndWait();
}

void DealerSession::CommandWorker::runTask() {
  PendingCommand cmd;
  while (!shouldStop()) {
    if (!pendingCommands->wtpop(cmd, 1000)) {
      continue;  // timeout or stop()-forced exit - re-check shouldStop()
    }

    bool success = connectState->handlePlayerCommand(cmd.endpoint, cmd.command);

    CSPOT_LOG(info, "Dealer: request key=%s ident=%s endpoint=%s -> success=%d",
             cmd.key.c_str(), cmd.ident.c_str(), cmd.endpoint.c_str(),
             (int)success);

    if (cmd.ownedRoot != nullptr) {
      cJSON_Delete(cmd.ownedRoot);
    }
    cJSON_Delete(cmd.root);

    pendingReplies->push({cmd.key, success});
  }
}

bool DealerSession::isConnected() {
  return transport != nullptr && transport->isConnected();
}

std::string DealerSession::getConnectionId() {
  std::lock_guard<std::mutex> lock(connectionIdMutex);
  return connectionId;
}

// Never cache the URL: re-resolves the host and re-fetches the token every
// attempt (§6.5).
bool DealerSession::connectOnce() {
  std::vector<std::string> dealerHosts;
  try {
    ApResolve apResolve("");
    dealerHosts = apResolve.fetchDealerAddresses();
  } catch (const std::exception& e) {
    CSPOT_LOG(error, "Dealer: resolving dealer address failed: %s", e.what());
    return false;
  }

  auto token = login5->getToken();
  if (token.empty()) {
    CSPOT_LOG(error, "Dealer: no login5 token available");
    return false;
  }

  // Try every candidate before giving up (F17 pattern).
  for (const auto& dealerHost : dealerHosts) {
    auto url = "wss://" + dealerHost + "/?access_token=" + token;

    transport = bell::WebSocketTransport::create(
        DEALER_PING_INTERVAL_MS, DEALER_PING_TIMEOUT_MS,
        DEALER_MAX_MESSAGE_SIZE);
    if (transport->connect(url)) {
      CSPOT_LOG(info, "Dealer: connected to %s", dealerHost.c_str());
      return true;
    }
    CSPOT_LOG(error, "Dealer: WebSocket connect to %s failed, trying next",
              dealerHost.c_str());
  }

  return false;
}

void DealerSession::runTask() {
  int backoffMs = RECONNECT_BACKOFF_BASE_MS;
  bool everConnected = false;

  while (!shouldStop()) {
    if (!connectOnce()) {
      CSPOT_LOG(info, "Dealer: retrying in %dms", backoffMs);
      // Segmented so stop() doesn't have to wait out the full backoff.
      for (int slept = 0; !shouldStop() && slept < backoffMs; slept += 250) {
        BELL_SLEEP_MS(250);
      }
      backoffMs = std::min(backoffMs * 2, RECONNECT_BACKOFF_MAX_MS);
      continue;
    }
    backoffMs = RECONNECT_BACKOFF_BASE_MS;

    if (everConnected) {
      CSPOT_LOG(debug, "Dealer: re-established dealer connection");
    } else {
      CSPOT_LOG(debug, "Dealer: starting dealer recv loop");
      everConnected = true;
    }

    int idleMs = 0;
    constexpr int IDLE_LOG_INTERVAL_MS = 30000;

    auto lastJsonPing = std::chrono::steady_clock::now();
    lastPongTime = std::chrono::steady_clock::now();

    std::string message;
    while (!shouldStop() && transport->isConnected()) {
      if (transport->receiveMessage(message, RECEIVE_POLL_MS)) {
        // Dispatch itself now happens off this thread - see handleMessage()
        // (the public, externally-driven entry point) and messageQueue's
        // own comment.
        messageQueue.push(message);
        idleMs = 0;
      } else {
        idleMs += RECEIVE_POLL_MS;
        if (idleMs >= IDLE_LOG_INTERVAL_MS) {
          CSPOT_LOG(info, "Dealer: connected, idle %ds (no server pushes)",
                    idleMs / 1000);
          idleMs = 0;
        }
      }

      // Only this thread may call transport->sendText() - replies come back
      // via this queue instead of CommandWorker sending them itself.
      PendingReply reply;
      while (pendingReplies.pop(reply)) {
        sendReply(reply.key, reply.success);
      }

      auto now = std::chrono::steady_clock::now();
      if (std::chrono::duration_cast<std::chrono::milliseconds>(
              now - lastJsonPing).count() >= JSON_PING_INTERVAL_MS) {
        auto sinceLastPong = std::chrono::duration_cast<
            std::chrono::milliseconds>(now - lastPongTime).count();
        if (sinceLastPong >= JSON_PING_INTERVAL_MS + JSON_PONG_TIMEOUT_MS) {
          CSPOT_LOG(error,
                    "Dealer: did not receive last pong from dealer, %lds "
                    "passed",
                    (long)(sinceLastPong / 1000));
          transport->disconnect();
          break;
        }

        if (transport->sendText(R"({"type":"ping"})")) {
          CSPOT_LOG(debug, "Dealer: sent dealer ping");
        } else {
          CSPOT_LOG(error, "Dealer: failed sending dealer ping");
        }
        lastJsonPing = now;
      }
    }

    // Idempotent - a no-op if disconnect() already ran above.
    transport->disconnect();

    {
      std::lock_guard<std::mutex> lock(connectionIdMutex);
      connectionId.clear();
    }

    if (!shouldStop()) {
      CSPOT_LOG(info, "Dealer: connection lost, reconnecting");
    }
  }
}

bool DealerSession::handleMessage() {
  std::string message;
  if (messageQueue.wtpop(message, 200)) {
    dispatchMessage(message);
    return true;
  }
  return false;
}

void DealerSession::dispatchMessage(const std::string& json) {
  cJSON* root = cJSON_Parse(json.c_str());
  if (root == nullptr) {
    CSPOT_LOG(error, "Dealer: unparseable message (%d bytes)",
              (int)json.size());
    return;
  }

  cJSON* typeItem = cJSON_GetObjectItem(root, "type");
  const char* type =
      (typeItem != nullptr && typeItem->valuestring != nullptr)
          ? typeItem->valuestring
          : "";

  if (strcmp(type, "message") == 0) {
    cJSON* uriItem = cJSON_GetObjectItem(root, "uri");
    const char* uri =
        (uriItem != nullptr && uriItem->valuestring != nullptr)
            ? uriItem->valuestring
            : "";
    cJSON* headers = cJSON_GetObjectItem(root, "headers");

    // gzip out of scope for now (§6.3) - logged and dropped, not guessed at.
    cJSON* transferEncoding =
        headers != nullptr
            ? cJSON_GetObjectItem(headers, "Transfer-Encoding")
            : nullptr;
    if (transferEncoding != nullptr &&
        transferEncoding->valuestring != nullptr) {
      CSPOT_LOG(info,
                "Dealer: message %s has Transfer-Encoding: %s "
                "(unsupported, dropped - see §6.3)",
                uri, transferEncoding->valuestring);
      cJSON_Delete(root);
      return;
    }

    if (strncmp(uri, "hm://pusher/v1/connections/", 27) == 0) {
      cJSON* idItem =
          headers != nullptr
              ? cJSON_GetObjectItem(headers, "Spotify-Connection-Id")
              : nullptr;
      if (idItem != nullptr && idItem->valuestring != nullptr) {
        {
          std::lock_guard<std::mutex> lock(connectionIdMutex);
          connectionId = idItem->valuestring;
        }
        CSPOT_LOG(info, "Dealer: got Spotify-Connection-Id (%d chars)",
                  (int)connectionId.size());

        // NEW_DEVICE PUT (§5.3) - this, not the WS connect, is what makes
        // the device selectable in the app.
        connectState->setConnectionId(idItem->valuestring);
        bool registered =
            connectState->putState(connectstate_PutStateReason_NEW_DEVICE);
        CSPOT_LOG(info,
                 "Dealer: device registered (NEW_DEVICE PUT %s) - should be "
                 "selectable in the app now",
                 registered ? "ok" : "FAILED");
      } else {
        CSPOT_LOG(error, "Dealer: hello message without connection id");
      }
    } else if (strncmp(uri, "hm://connect-state/v1/cluster", 30) == 0) {
      cJSON* payloadsItem = cJSON_GetObjectItem(root, "payloads");
      cJSON* firstPayload =
          payloadsItem != nullptr ? cJSON_GetArrayItem(payloadsItem, 0)
                                  : nullptr;
      if (firstPayload != nullptr && firstPayload->valuestring != nullptr) {
        auto raw = Crypto::base64Decode(firstPayload->valuestring);
        connectState->handleClusterUpdate(raw);
      } else {
        CSPOT_LOG(error, "Dealer: cluster update without a payload");
      }
    } else if (strncmp(uri, "hm://connect-state/v1/connect/volume", 37) == 0) {
      cJSON* payloadsItem = cJSON_GetObjectItem(root, "payloads");
      cJSON* firstPayload =
          payloadsItem != nullptr ? cJSON_GetArrayItem(payloadsItem, 0)
                                  : nullptr;
      if (firstPayload != nullptr && firstPayload->valuestring != nullptr) {
        auto raw = Crypto::base64Decode(firstPayload->valuestring);
        connectState->handleSetVolume(raw);
      } else {
        CSPOT_LOG(error, "Dealer: volume command without a payload");
      }
    } else {
      CSPOT_LOG(info, "Dealer: message uri=%s (%d bytes, unhandled in 5.2)",
                uri, (int)json.size());
    }
  } else if (strcmp(type, "request") == 0) {
    cJSON* identItem = cJSON_GetObjectItem(root, "message_ident");
    std::string ident =
        (identItem != nullptr && identItem->valuestring != nullptr)
            ? identItem->valuestring
            : "?";
    cJSON* keyItem = cJSON_GetObjectItem(root, "key");
    std::string key = (keyItem != nullptr && keyItem->valuestring != nullptr)
                          ? keyItem->valuestring
                          : "";

    cJSON* headers = cJSON_GetObjectItem(root, "headers");
    cJSON* transferEncoding =
        headers != nullptr
            ? cJSON_GetObjectItem(headers, "Transfer-Encoding")
            : nullptr;

    std::string endpoint = "?";
    // Once queued, cleanup/reply becomes CommandWorker's job, not ours.
    bool queued = false;
    if (transferEncoding != nullptr && transferEncoding->valuestring != nullptr) {
      endpoint = "<gzip, unsupported - see §6.3>";
    } else {
      // "payload" is usually the command object directly, not always
      // {"compressed": "<base64>"} - try compressed first, fall back.
      cJSON* payloadItem = cJSON_GetObjectItem(root, "payload");
      cJSON* compressedItem =
          payloadItem != nullptr
              ? cJSON_GetObjectItem(payloadItem, "compressed")
              : nullptr;

      cJSON* commandRoot = nullptr;  // borrowed from root, or owned (parsed)
      cJSON* ownedRoot = nullptr;    // non-null only if we must cJSON_Delete it

      if (compressedItem != nullptr && compressedItem->valuestring != nullptr) {
        auto raw = Crypto::base64Decode(compressedItem->valuestring);
        if (raw.empty()) {
          CSPOT_LOG(error, "Dealer: request payload.compressed base64-decoded "
                          "to 0 bytes (%d chars in)",
                    (int)strlen(compressedItem->valuestring));
        } else {
          std::string commandJson((char*)raw.data(), raw.size());
          ownedRoot = cJSON_Parse(commandJson.c_str());
          if (ownedRoot == nullptr) {
            CSPOT_LOG(error, "Dealer: request payload didn't parse as JSON "
                            "(%d bytes decoded): %s",
                      (int)raw.size(), commandJson.c_str());
          }
          commandRoot = ownedRoot;
        }
      } else if (payloadItem != nullptr) {
        commandRoot = payloadItem;
      } else {
        char* rawEnvelope = cJSON_PrintUnformatted(root);
        CSPOT_LOG(error, "Dealer: request has no \"payload\" at all: %s",
                 rawEnvelope != nullptr ? rawEnvelope : "?");
        if (rawEnvelope != nullptr) {
          free(rawEnvelope);
        }
      }

      if (commandRoot != nullptr) {
        // Recorded before executing - echoed back on every subsequent PUT
        // (PlayerEngine::setLastCommand()).
        cJSON* messageIdItem = cJSON_GetObjectItem(commandRoot, "message_id");
        cJSON* sentByItem =
            cJSON_GetObjectItem(commandRoot, "sent_by_device_id");
        connectState->setLastCommand(
            (messageIdItem != nullptr && cJSON_IsNumber(messageIdItem))
                ? (uint32_t)messageIdItem->valuedouble
                : 0,
            (sentByItem != nullptr && sentByItem->valuestring != nullptr)
                ? sentByItem->valuestring
                : "");

        cJSON* command = cJSON_GetObjectItem(commandRoot, "command");
        cJSON* endpointItem =
            command != nullptr ? cJSON_GetObjectItem(command, "endpoint")
                               : nullptr;
        if (endpointItem != nullptr && endpointItem->valuestring != nullptr) {
          endpoint = endpointItem->valuestring;
          // Handed off to CommandWorker - a slow command must never stall
          // this task's own receive loop/pings (see DealerSession.h).
          pendingCommands.push(PendingCommand{key, ident, endpoint, command,
                                              root, ownedRoot});
          queued = true;
        } else {
          char* rawCommand = cJSON_PrintUnformatted(commandRoot);
          CSPOT_LOG(error,
                   "Dealer: request payload has no command.endpoint: %s",
                   rawCommand != nullptr ? rawCommand : "?");
          if (rawCommand != nullptr) {
            free(rawCommand);
          }
        }
      }

      if (!queued && ownedRoot != nullptr) {
        cJSON_Delete(ownedRoot);
      }
    }

    if (!queued) {
      // Every "request" must get a reply or the app waits out a timeout.
      CSPOT_LOG(info, "Dealer: request key=%s ident=%s endpoint=%s -> success=0",
               key.c_str(), ident.c_str(), endpoint.c_str());
      sendReply(key, false);
      cJSON_Delete(root);
    }
    return;
  } else if (strcmp(type, "pong") == 0) {
    // Reply to runTask()'s JSON keepalive - feeds the missed-pong watchdog.
    CSPOT_LOG(debug, "Dealer: received dealer pong");
    lastPongTime = std::chrono::steady_clock::now();
  } else {
    CSPOT_LOG(info, "Dealer: unknown message type '%s' (%d bytes)", type,
              (int)json.size());
  }

  cJSON_Delete(root);
}

void DealerSession::sendReply(const std::string& key, bool success) {
  if (key.empty() || transport == nullptr) {
    return;
  }

  cJSON* reply = cJSON_CreateObject();
  cJSON_AddStringToObject(reply, "type", "reply");
  cJSON_AddStringToObject(reply, "key", key.c_str());
  cJSON* payload = cJSON_CreateObject();
  cJSON_AddBoolToObject(payload, "success", success);
  cJSON_AddItemToObject(reply, "payload", payload);

  char* str = cJSON_PrintUnformatted(reply);
  if (str != nullptr) {
    if (!transport->sendText(str)) {
      CSPOT_LOG(error, "Dealer: failed sending reply for key=%s",
               key.c_str());
    }
    free(str);
  }
  cJSON_Delete(reply);
}
