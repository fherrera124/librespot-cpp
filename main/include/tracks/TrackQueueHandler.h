#pragma once

#include <optional>
#include <string>

#include "api/SpClient.h"
#include "bell/Result.h"
#include "connect.pb.h"
#include "events/EventLoop.h"
#include "proto/ConnectPb.h"

namespace cspot {
// Context tracks IDs or URIs can sometimes be missing or invalid
struct TrackId {
  std::optional<std::string> uid = std::nullopt;
  std::optional<std::string> uri = std::nullopt;

  TrackId() = default;

  TrackId(const std::string& uid, const std::string& uri) {
    if (!uid.empty()) {
      this->uid = uid;
    }
    if (!uri.empty()) {
      this->uri = uri;
    }
  }

  bool operator==(const TrackId& other) const {
    if (uid.has_value() && other.uid.has_value()) {
      return uid.value() == other.uid.value();
    }

    if (uri.has_value() && other.uri.has_value()) {
      return uri.value() == other.uri.value();
    }

    return false;
  }

  bool operator==(const cspot_proto::ContextTrack& other) const {
    if (!other.uid.empty() && uid.has_value()) {
      return uid.value() == other.uid;
    }
    if (!other.uri.empty() && uri.has_value()) {
      return uri.value() == other.uri;
    }
    return false;
  }
};

class TrackQueueHandler {
 public:
  virtual ~TrackQueueHandler() = default;

  virtual bell::Result<> loadContext(
      const std::string& contextUri,
      std::optional<std::string> currentTrackUri = std::nullopt,
      std::optional<std::string> currentTrackUid = std::nullopt) = 0;

  virtual void setQueue(
      const std::vector<cspot_proto::ContextTrack>& queue) = 0;

  virtual void setPlayingQueue(bool isPlayingQueue) = 0;

  virtual std::optional<cspot_proto::ProvidedTrack> currentTrack() = 0;

  // virtual std::optional<cspot_proto::ContextIndex> currentContextIndex() = 0;

  // virtual bell::Result<cspot_proto::ProvidedTrack> next();
  // virtual bell::Result<cspot_proto::ProvidedTrack> previous();

  // virtual bell::Result<> enableShuffle(bool shuffle);
};

std::unique_ptr<TrackQueueHandler> createDefaultTrackQueueHandler(
    std::shared_ptr<SpClient> spClient, std::shared_ptr<EventLoop> eventLoop);
};  // namespace cspot
