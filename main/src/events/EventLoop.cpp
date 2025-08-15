#include "events/EventLoop.h"
#include <iostream>

#include "bell/Logger.h"

using namespace cspot;

cspot::EventLoop::EventLoop() : bell::Task("cspot_event_loop", 8 * 1024) {
  // Run the event loop in a separate thread
  startTask();
}

void EventLoop::taskLoop() {
  // Process events with a timeout of 1000ms
  processEvents(1000);
}

void EventLoop::processEvents(int timeoutMs) {
  // Wait for events to be posted
  if (!eventSemaphore.take(timeoutMs)) {
    return;  // Timeout, no events to process
  }

  std::queue<Event> processingQueue;

  {
    std::scoped_lock lock(queueMutex);

    if (eventQueue.empty()) {
      return;  // No events to process
    }

    // Swap with the main queue under lock
    processingQueue.swap(eventQueue);
  }

  // Process each event
  while (!processingQueue.empty()) {
    Event event = std::move(processingQueue.front());
    processingQueue.pop();

    std::cout << "Got event of type: " << static_cast<int>(event.type)
              << std::endl;
    // Call the appropriate handler for the event type
    std::scoped_lock lock(handlersMutex);
    auto it = handlers.find(event.type);
    if (it != handlers.end()) {
      try {
        it->second(std::move(event));
      } catch (const std::exception& e) {
        BELL_LOG(error, LOG_TAG, "Error in event handler: {}", e.what());
      }
    } else {
      BELL_LOG(warn, LOG_TAG, "No handler registered for event type: {}",
               static_cast<int>(event.type));
    }
  }
}

void EventLoop::registerHandler(EventType type, EventHandler handler) {
  std::scoped_lock lock(handlersMutex);
  handlers.insert({type, std::move(handler)});
}

void EventLoop::unregisterHandler(EventType type) {
  std::scoped_lock lock(handlersMutex);
  handlers.erase(type);
}
