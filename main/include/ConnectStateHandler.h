#pragma once

// Protobufs
#include "TrackQueue.h"
#include "api/ApClient.h"
#include "bell/Result.h"
#include "connect.pb.h"

#include "SessionContext.h"
#include "api/SpClient.h"

namespace cspot {

class ConnectStateHandler {
 public:
  ConnectStateHandler(std::shared_ptr<SessionContext> sessionContext,
                      std::shared_ptr<SpClient> spClient,
                      std::shared_ptr<ApClient> apClient);

  bell::Result<> handlePlayerCommand(tao::json::value& messageJson);

  bell::Result<> putState(
      PutStateReason reason = PutStateReason_PLAYER_STATE_CHANGED);

 private:
  const char* LOG_TAG = "ConnectStateHandler";

  std::shared_ptr<SessionContext> sessionContext;
  std::shared_ptr<SpClient> spClient;
  std::shared_ptr<ApClient> apClient;
  std::shared_ptr<TrackQueue> trackQueue;

  // Holds the protobuf state
  cspot_proto::PutStateRequest putStateRequestProto;

  void initialize();

  bell::Result<> handleTransferCommand(std::string_view payloadDataStr,
                                       const tao::json::value& options);

  bell::Result<> handleSkipNextCommand();

  bell::Result<> handleSkipPrevCommand();
};
}  // namespace cspot
