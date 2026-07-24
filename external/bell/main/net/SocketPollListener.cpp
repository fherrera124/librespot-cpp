#include "bell/net/SocketPollListener.h"

// Standar includes
#include <sys/poll.h>
#include <cstring>

#include "bell/Logger.h"
#include "bell/utils/Utils.h"

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
    BELL_LOG(error, LOG_TAG, "poll failed err={}", strerror(errno));
    return;
  }

  for (auto& pfd : fdsCopy) {
    if (pfd.revents == 0) {
      continue;
    }

    // Fresh handlers.find() per revents bit, not one cached iterator: an
    // earlier callback for this fd (eg. Readable finding a dead socket)
    // can unregisterSocket() and erase the handler mid-dispatch, which
    // would leave a cached iterator dangling for the next bit's check -
    // e.g. POLLIN+POLLHUP arriving together on a peer-closed socket.
    auto dispatchEvent = [&](int pollFlag, Event eventType) {
      if (!(pfd.revents & pollFlag)) {
        return;
      }

      EventCallback cb;
      std::shared_ptr<Socket> socketPtr;

      {
        std::scoped_lock lock(pollMutex);
        auto it = handlers.find(pfd.fd);
        if (it == handlers.end()) {
          return;
        }

        socketPtr = it->second.socketPtr.lock();
        if (!socketPtr) {
          return;
        }

        auto cbIt = it->second.callbacks.find(eventType);
        if (cbIt == it->second.callbacks.end()) {
          return;
        }

        cb = cbIt->second;
      }

      // Run the callback with the lock released, so it can freely call
      // back into registerSocket()/unregisterSocket() without blocking
      // (or racing) other threads doing the same.
      cb(*socketPtr);
    };

    dispatchEvent(POLLIN, Event::Readable);
    dispatchEvent(POLLOUT, Event::Writeable);
    dispatchEvent(POLLPRI, Event::Priority);
    dispatchEvent(POLLERR, Event::Error);

    if (pfd.revents & POLLHUP) {
      dispatchEvent(POLLHUP, Event::Hangup);

      std::scoped_lock lock(pollMutex);
      if (handlers.erase(pfd.fd) > 0) {
        updateFdList();
      }
    }
  }
}
