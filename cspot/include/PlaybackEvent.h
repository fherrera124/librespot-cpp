#pragma once

#include <functional>
#include <memory>
#include <variant>

#include "TrackQueue.h"  // for TrackInfo

namespace cspot {

// Playback-engine events reported up to cspot_connect.cpp. Originally
// extracted out of SpircHandler during Fase 6's "corte completo"
// (docs/dealer_websocket_migration.md) so both engines could share it during
// the transition; SpircHandler is gone now and ConnectStateHandler is the
// only producer.
enum class EventType {
  PLAY_PAUSE,
  VOLUME,
  TRACK_INFO,
  DISC,
  NEXT,
  PREV,
  SEEK,
  DEPLETED,
  FLUSH,
  PLAYBACK_START,
  REPEAT_CONTEXT
};

typedef std::variant<TrackInfo, int, bool> EventData;

struct Event {
  EventType eventType;
  EventData data;
};

typedef std::function<void(std::unique_ptr<Event>)> EventHandler;

}  // namespace cspot
