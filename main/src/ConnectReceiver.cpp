#include "ConnectReceiver.h"

#include <atomic>
#include <utility>

#include "Authenticator.h"
#include "ZeroconfServer.h"
#include "bell/Logger.h"
#include "bell/http/Server.h"
#include "bell/utils/Semaphore.h"

using namespace cspot;

namespace {
const char* LOG_TAG = "ConnectReceiver";
}

ConnectReceiver::ConnectReceiver(std::shared_ptr<AuthInfo> authInfo,
                                 std::string sessionFilePath,
                                 ConnectReceiverConfig config)
    : authInfo(std::move(authInfo)),
      sessionStore(std::move(sessionFilePath)),
      config(std::move(config)) {}

void ConnectReceiver::run() {
  sessionStore.load(*authInfo);
  authInfo->logDeviceIdOrigin();

  auto httpServer = std::make_shared<bell::http::Server>();
  bell::Semaphore authSemaphore;
  std::atomic<bool> needsSessionRestart{false};

  // Wakes the loop below to (re)build the Session with these credentials,
  // replacing whatever pairing (if any) is already active. give() only
  // fires on success so the loop never wakes to rebuild against
  // stale/unchanged credentials.
  ZeroconfServer zeroconfServer(authInfo, httpServer, [&]() {
    needsSessionRestart.store(true);
    authSemaphore.give();
  });
  auto mdnsService = zeroconfServer.start();

  sessionStore.save(*authInfo);

  // haveCredentialsToTry: skips the initial wait when a persisted session
  // was loaded above, so it's tried once before falling back to waiting
  // for a pairing.
  std::shared_ptr<cspot::Session> session;
  bool haveCredentialsToTry = authInfo->loginCredentials.has_value();
  while (true) {
    if (!haveCredentialsToTry) {
      authSemaphore.take();
      needsSessionRestart.store(false);

      if (!authInfo->loginCredentials.has_value() ||
          authInfo->loginCredentials->authData.empty()) {
        continue;
      }
    }
    haveCredentialsToTry = false;

    BELL_LOG(info, LOG_TAG,
             session ? "Rebuilding session after a fresh pairing"
                     : "Starting session");
    if (session) {
      auto oldInactiveRes = session->putInactive();
      if (!oldInactiveRes) {
        BELL_LOG(warn, LOG_TAG, "putInactive on old session failed: {}",
                 oldInactiveRes.error());
      }
    }
    // Explicit reset before reassigning: make_shared below would otherwise
    // fully construct the new Session (starting its own tasks) before this
    // pointer's old value is released, leaving two Sessions briefly alive
    // at once - both driving the same shared config.audioSink.
    session.reset();
    session = std::make_shared<cspot::Session>(
        authInfo, config.audioSink, config.playbackNotificationCallback,
        config.audioConfig);
    {
      std::scoped_lock lock(activeSessionMutex);
      activeSession = session;
    }
    auto startRes = session->start();
    if (!startRes) {
      BELL_LOG(error, LOG_TAG,
               "Failed to start session: {} - waiting for another pairing "
               "attempt",
               startRes.error());
      continue;
    }
    sessionStore.save(*authInfo);

    // Returns on a fresh pairing (needsSessionRestart) or a rejected login
    // (Session::credentialsRejected()). Transient transport failures are
    // retried internally and never surface here.
    session->runPoller(needsSessionRestart);

    if (session->credentialsRejected()) {
      BELL_LOG(error, LOG_TAG,
               "Credentials rejected by server - clearing and waiting for "
               "a new pairing");
      authInfo->loginCredentials.reset();
      sessionStore.save(*authInfo);
    }
  }
}

std::shared_ptr<cspot::Session> ConnectReceiver::lockActiveSession() {
  std::scoped_lock lock(activeSessionMutex);
  return activeSession.lock();
}

bool ConnectReceiver::requestPlayPause(bool play) {
  auto session = lockActiveSession();
  return session && session->requestPlayPause(play);
}

bool ConnectReceiver::requestNext() {
  auto session = lockActiveSession();
  return session && session->requestNext();
}

bool ConnectReceiver::requestPrevious() {
  auto session = lockActiveSession();
  return session && session->requestPrevious();
}

bool ConnectReceiver::requestSeek(uint32_t positionMs) {
  auto session = lockActiveSession();
  return session && session->requestSeek(positionMs);
}

bool ConnectReceiver::requestSetRepeatContext(bool enabled) {
  auto session = lockActiveSession();
  return session && session->requestSetRepeatContext(enabled);
}

uint32_t ConnectReceiver::getPositionMs() {
  auto session = lockActiveSession();
  return session ? session->getPositionMs() : 0;
}
