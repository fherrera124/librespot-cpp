#pragma once

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
class PlayerEngine;

// Dealer WebSocket client (docs/dealer_websocket_migration.md): owns the
// connection lifecycle and receive loop, parses the JSON envelope, and
// dispatches by URI/type to this class's own PlayerEngine
// (getConnectState()), which owns the whole playback engine.
class DealerSession : public bell::Task {
 public:
  DealerSession(std::shared_ptr<cspot::Context> ctx);
  ~DealerSession();

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
  * same PlayerEngine that also executes remote player/command
  * requests, so there's exactly one engine either input reaches. Valid for
  * as long as this DealerSession is (constructed in the constructor, never
  * null).
  */
  PlayerEngine* getConnectState() { return connectState.get(); }

  // Pops one message (bounded ~200ms wait, mirrors MercurySession::
  // handlePacket()) and dispatches it if one arrived - the external
  // entry point SpotifyConnectReceiver's own dispatch loop drives,
  // exactly like Mercury's own handlePacket(). Safe to call from any
  // thread: dispatchMessage() never touches `transport` (see its own
  // comment) - the only thing that may is this task's own runTask().
  bool handleMessage();

 protected:
  void runTask() override;

 private:
  bool connectOnce();
  // The real parse/dispatch logic - decides by URI/type and updates
  // PlayerEngine/Context state, or queues a player/command for
  // CommandWorker. Never calls transport->sendText() itself (replies go
  // through pendingReplies instead), which is what makes it safe to run
  // from any thread, not just runTask()'s own.
  void dispatchMessage(const std::string& json);
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
    CommandWorker(PlayerEngine* connectState,
                  bell::Queue<PendingCommand>* pendingCommands,
                  bell::Queue<PendingReply>* pendingReplies);
    ~CommandWorker();
    void stop();

   protected:
    void runTask() override;
    // Wakes a blocked wtpop() immediately, instead of waiting out its 1000ms
    // poll.
    void onStopRequested() override { pendingCommands->clear(); }

   private:
    PlayerEngine* connectState;
    bell::Queue<PendingCommand>* pendingCommands;
    bell::Queue<PendingReply>* pendingReplies;
  };

  std::shared_ptr<cspot::Context> ctx;
  std::shared_ptr<Login5Client> login5;
  std::unique_ptr<bell::WebSocketTransport> transport;
  std::unique_ptr<PlayerEngine> connectState;

  // Raw messages received by runTask()'s own thread, drained externally by
  // handleMessage() - same producer/consumer shape as MercurySession's own
  // packetQueue.
  bell::Queue<std::string> messageQueue;

  bell::Queue<PendingCommand> pendingCommands;
  bell::Queue<PendingReply> pendingReplies;
  // After connectState: destroyed (task joined) before it is.
  std::unique_ptr<CommandWorker> commandWorker;

  std::mutex connectionIdMutex;
  std::string connectionId;

  std::chrono::time_point<std::chrono::steady_clock> lastPongTime;
};
}  // namespace cspot
