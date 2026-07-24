#include "bell/net/SocketPollListener.h"

// Standar includes
#include <sys/poll.h>
#include <cstring>

#include "bell/utils/Utils.h"
#include "fmt/format.h"

using namespace bell::net;

void SocketPollListener::registerSocket(const std::shared_ptr<Socket>& socket,
                                        Event polledEvent,
                                        const EventCallback& onEvent) {
  std::scoped_lock lock(pollMutex);

  if (!socket || !socket->isValid()) {
    throw std::invalid_argument("Invalid socket");
  }

  if (handlers.find(socket->getFd()) == handlers.end()) {
    // Create a new handler for the socket
    handlers[socket->getFd()] = {.socketPtr = socket, .callbacks = {}};
  }

  handlers[socket->getFd()].callbacks[polledEvent] = onEvent;

  updateFdList();
}

void SocketPollListener::updateFdList() {
  std::scoped_lock lock(pollMutex);

  fds.clear();

  for (const auto& handler : handlers) {
    pollfd pfd{};
    pfd.fd = handler.first;

    pfd.events = 0;

    for (const auto& callback : handler.second.callbacks) {
      switch (callback.first) {
        case Event::Readable:
          pfd.events |= POLLIN;
          break;
        case Event::Writeable:
          pfd.events |= POLLOUT;
          break;
        case Event::Error:
          pfd.events |= POLLERR;
          break;
        case Event::Hangup:
          pfd.events |= POLLHUP;
          break;
        case Event::Priority:
          pfd.events |= POLLPRI;
          break;
        case Event::All:
          pfd.events |= (POLLIN | POLLOUT | POLLERR | POLLHUP | POLLPRI);
          break;
      }
    }

    // Add the file descriptor to the list
    fds.push_back(pfd);
  }
}

void SocketPollListener::unregisterSocket(const std::shared_ptr<Socket>& socket,
                                          Event polledEvent) {
  std::scoped_lock lock(pollMutex);
  if (socket->isValid() && handlers.contains(socket->getFd())) {
    if (polledEvent == Event::All) {
      // Remove all events for the socket
      handlers.erase(socket->getFd());
    } else {
      // Remove the specific event for the socket
      handlers[socket->getFd()].callbacks.erase(polledEvent);
    }
  }

  updateFdList();
}

void SocketPollListener::poll(int timeoutMs) {
  std::vector<pollfd> fdsCopy{};

  {
    std::scoped_lock lock(pollMutex);

    bool rebuildFdList = false;

    // Erase all FDS with expired weakptr
    for (auto it = handlers.begin(); it != handlers.end();) {
      if (it->second.socketPtr.expired() ||
          !it->second.socketPtr.lock()->isValid()) {
        rebuildFdList = true;
        it = handlers.erase(it);
      } else {
        ++it;
      }
    }

    if (rebuildFdList) {
      updateFdList();
    }

    fdsCopy.reserve(fds.size());
    std::copy(fds.begin(), fds.end(), std::back_inserter(fdsCopy));
  }

  if (fdsCopy.empty()) {
    bell::utils::sleepMs(timeoutMs);
    return;
  }

  int pollResult = ::poll(fdsCopy.data(), fdsCopy.size(), timeoutMs);

  if (pollResult < 0) {  // Handle polling error
    throw std::runtime_error(
        fmt::format("poll failed err={}", strerror(errno)));
    return;
  }

  for (auto& pfd : fdsCopy) {
    if (pfd.revents != 0) {  // If there are any events
      auto it = handlers.find(pfd.fd);
      if (it != handlers.end()) {
        auto socketPtr = it->second.socketPtr.lock();

        if (!socketPtr) {
          continue;
        }

        if ((pfd.revents & POLLIN) &&
            it->second.callbacks.contains(Event::Readable)) {
          // Call the readable callback
          it->second.callbacks[Event::Readable](*socketPtr);
        }

        if ((pfd.revents & POLLOUT) &&
            it->second.callbacks.contains(Event::Writeable)) {
          // Call the writeable callback
          it->second.callbacks[Event::Writeable](*socketPtr);
        }

        if ((pfd.revents & POLLPRI) &&
            it->second.callbacks.contains(Event::Priority)) {
          // Call the priority callback
          it->second.callbacks[Event::Priority](*socketPtr);
        }

        if ((pfd.revents & POLLERR) &&
            it->second.callbacks.contains(Event::Error)) {
          // Call the writeable callback
          it->second.callbacks[Event::Error](*socketPtr);
        }

        if (pfd.revents & POLLHUP) {
          if (it->second.callbacks.contains(Event::Hangup)) {
            // Call the hangup callback
            it->second.callbacks[Event::Hangup](*socketPtr);
          }

          // Remove the handler
          handlers.erase(it->first);

          // Rebuild the file descriptor list
          updateFdList();
        }
      }
    }
  }
}
