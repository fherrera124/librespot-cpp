#pragma once

#include <bell/utils/Semaphore.h>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>
#include <tao/json/value.hpp>
#include <unordered_map>
#include <utility>
#include <variant>

#include "bell/utils/Task.h"
#include "events/EventModels.h"

namespace cspot {
class EventLoop : public bell::Task {
 public:
  EventLoop();
  ~EventLoop() override { stopTask(); };

  enum class EventType {
    DEALER_REQUEST,
    DEALER_MESSAGE,
    FILE_PROVIDED,
    CURRENT_TRACK_METADATA,
    AUDIO_KEY,
    QUEUE_UPDATED,
    PLAYER_PLAY,
    PLAYER_FLUSH,
    // Posted by StreamPlayer on natural end-of-track (taskLoop()'s EOF
    // check) - handled by ConnectStateHandler, which is the sole owner of
    // "what's the next track" (TrackQueueHandler's context/queue/shuffle
    // state). Carries no payload (std::monostate). Unlike TRACK_UNPLAYABLE
    // below or a remote skip_next command, this honors repeat-track
    // (replays the same track) rather than always advancing - see
    // ConnectStateHandler::advanceToNextTrackLocked()'s own comment.
    TRACK_ENDED,
    // Posted by StreamPlayer when a track could never be loaded (CDN/audio
    // key failure) rather than reaching a natural end.
    TRACK_UNPLAYABLE,
    // Posted by ConnectStateHandler on a seek_to command - payload is the
    // already-resolved absolute target position in ms (relative/beginning/
    // value math happens in ConnectStateHandler; StreamPlayer just seeks
    // the open decoder to it).
    PLAYER_SEEK
  };

  // Define all possible event payload types
  using EventPayload =
      std::variant<std::monostate, bool, int64_t, CurrentTrackMetadata,
                   AudioKeyResponse, TrackQueueUpdate, ProvidedFile,
                   tao::json::value>;

  struct Event {
    EventType type;
    EventPayload payload;

    // Perfect forwarding constructor
    template <typename T>
    Event(EventType t, T&& p) : type(t), payload(std::forward<T>(p)) {}
  };

  using EventHandler = std::function<void(Event&&)>;

  // Post an event by moving it into the queue
  template <typename T>
  void post(EventType type, T&& payload) {
    std::scoped_lock lock(queueMutex);
    eventQueue.emplace(type, std::forward<T>(payload));
    eventSemaphore.give();
  }

  // Register a handler for a specific event type
  void registerHandler(EventType type, EventHandler handler);

  // Unregister all handlers for a specific event type
  void unregisterHandler(EventType type);

  // Processes the incoming events
  void processEvents(int timeoutMs = 1000);

 private:
  const char* LOG_TAG = "EventLoop";

  bell::Semaphore eventSemaphore;
  std::mutex queueMutex;
  std::queue<Event> eventQueue;
  std::unordered_map<EventType, EventHandler> handlers;
  std::mutex handlersMutex;

  // Bell task implementation
  void taskLoop() override;
};
}  // namespace cspot
