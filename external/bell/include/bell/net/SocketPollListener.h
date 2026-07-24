#pragma once

// Standard includes
#include <sys/poll.h>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "bell/Logger.h"
#include "bell/net/Socket.h"

namespace bell::net {
class SocketPollListener {
 public:
  // Default constructor
  SocketPollListener() = default;

  enum class Event {
    Readable = POLLIN,    // Readable
    Writeable = POLLOUT,  // Writable
    Error = POLLERR,      // Error
    Hangup = POLLHUP,     // Hangup
    Priority = POLLPRI,   // Priority
    All = 0xFFFF          // All events, used for unregistering
  };

  using EventCallback = std::function<void(Socket&)>;

  /**
   * @brief Registers a socket with the poll listener.
   *
   * @param socket Pointer to the socket to register
   * @param polledEvent Event to poll for
   * @param onEvent Callback function to be called when the event occurs
   */
  void registerSocket(const std::shared_ptr<Socket>& socket, Event polledEvent,
                      const EventCallback& onEvent = {});

  // Polls the registered sockets for events
  void poll(int timeoutMs = 100);

  // Unregisters a socket from the poll listener
  void unregisterSocket(const std::shared_ptr<Socket>& socket,
                        Event polledEvent = Event::All);

 private:
  struct SocketCallbacks {
    // Weak pointer to the registered socket
    std::weak_ptr<Socket> socketPtr;

    // Registered callbacks for events
    std::unordered_map<Event, EventCallback> callbacks;
  };

  // keeps reference to socket we're listening to events from
  std::unordered_map<int, SocketCallbacks> handlers;
  std::vector<pollfd> fds;
  std::recursive_mutex pollMutex;
  const char* LOG_TAG = "SocketPollListener";

  // Updates the file descriptor list for polling, based on the registered sockets
  void updateFdList();
};
}  // namespace bell::net

namespace bell {
using SocketPollListener = net::SocketPollListener;
using PollEvent = net::SocketPollListener::Event;
}  // namespace bell
