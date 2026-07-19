#pragma once

#include <atomic>      // for atomic
#include <chrono>      // for steady_clock::time_point
#include <functional>  // for function
#include <memory>      // for unique_ptr, shared_ptr
#include <mutex>       // for mutex
#include <string>      // for string

#include "BellTask.h"  // for Task
#include "Queue.h"      // for bell::Queue

typedef struct cJSON cJSON;

namespace bell {
class WebSocketTransport;
}

namespace cspot {
struct Context;
class Login5Client;
class ApResolve;
class ConnectStateHandler;

// Dealer WebSocket client (docs/dealer_websocket_migration.md): owns the
// connection lifecycle and receive loop, parses the JSON envelope, and
// dispatches by URI/type to this class's own ConnectStateHandler
// (getConnectState()), which owns the whole playback engine.
class DealerClient : public bell::Task {
 public:
  DealerClient(std::shared_ptr<cspot::Context> ctx);
  ~DealerClient();

  /**
  * @brief Stops the reconnect/receive loop and closes the connection.
  */
  void stop();

  bool isConnected();

  /**
  * @brief x-spotify-connection-id for connect-state PUTs (§6.4) - empty
  * until the hello message arrives.
  */
  std::string getConnectionId();

  /**
  * @brief The playback engine cspot_connect.cpp drives directly (local
  * play/pause/next/previous/seek button presses, position reads) - the
  * same ConnectStateHandler that also executes remote player/command
  * requests, so there's exactly one engine either input reaches. Valid for
  * as long as this DealerClient is (constructed in the constructor, never
  * null).
  */
  ConnectStateHandler* getConnectState() { return connectState.get(); }

 protected:
  void runTask() override;

 private:
  bool connectOnce();
  void handleMessage(const std::string& json);
  void sendReply(const std::string& key, bool success);

  // A "request" queued for CommandWorker (potentially slow, must not stall
  // the receive loop). `command` is borrowed (points into `root` or
  // `ownedRoot`); the worker owns freeing both once it's done.
  struct PendingCommand {
    std::string key;
    std::string ident;
    std::string endpoint;
    cJSON* command;
    cJSON* root;
    cJSON* ownedRoot;  // may be nullptr
  };
  struct PendingReply {
    std::string key;
    bool success;
  };

  // Runs handlePlayerCommand() for each PendingCommand off the receive
  // loop. Never touches `transport` (not thread-safe) - replies via
  // pendingReplies instead.
  class CommandWorker : public bell::Task {
   public:
    CommandWorker(ConnectStateHandler* connectState,
                  bell::Queue<PendingCommand>* pendingCommands,
                  bell::Queue<PendingReply>* pendingReplies);
    ~CommandWorker();
    void stop();

   protected:
    void runTask() override;

   private:
    ConnectStateHandler* connectState;
    bell::Queue<PendingCommand>* pendingCommands;
    bell::Queue<PendingReply>* pendingReplies;

    std::mutex taskLifetimeMutex;  // F93 pattern
    std::atomic<bool> running{true};
  };

  std::shared_ptr<cspot::Context> ctx;
  std::shared_ptr<Login5Client> login5;
  std::unique_ptr<bell::WebSocketTransport> transport;
  std::unique_ptr<ConnectStateHandler> connectState;

  bell::Queue<PendingCommand> pendingCommands;
  bell::Queue<PendingReply> pendingReplies;
  // After connectState: destroyed (task joined) before it is.
  std::unique_ptr<CommandWorker> commandWorker;

  std::mutex connectionIdMutex;
  std::string connectionId;

  std::chrono::time_point<std::chrono::steady_clock> lastPongTime;

  std::mutex taskLifetimeMutex;  // F93 pattern

  std::atomic<bool> running{true};
};
}  // namespace cspot
