#pragma once

#include <bell/utils/Semaphore.h>
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
  ~EventLoop() {};

  enum class EventType {
    DEALER_REQUEST,
    DEALER_MESSAGE,
    FILE_PROVIDED,
    CURRENT_TRACK_METADATA,
    AUDIO_KEY,
    QUEUE_UPDATED,
  };

  // Define all possible event payload types
  using EventPayload =
      std::variant<tao::json::value, std::monostate, CurrentTrackMetadata,
                   AudioKeyResponse, TrackQueueUpdate, ProvidedFile>;

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
