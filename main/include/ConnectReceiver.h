#pragma once

#include <memory>
#include <mutex>
#include <string>

#include "AudioSink.h"
#include "AuthInfo.h"
#include "Session.h"
#include "SessionStore.h"

namespace cspot {

// Runs the full Spotify Connect receiver lifecycle: loads any persisted
// session, pairs via ZeroconfServer when needed, and builds/rebuilds a
// Session for each pairing. Shared by every target - the only per-target
// inputs are audioSink and where to persist the session file.
class ConnectReceiver {
 public:
  ConnectReceiver(std::shared_ptr<AuthInfo> authInfo,
                  std::string sessionFilePath,
                  std::shared_ptr<AudioSink> audioSink =
                      std::make_shared<NullAudioSink>(),
                  cspot::PlaybackNotificationCallback
                      playbackNotificationCallback =
                          [](const PlaybackNotificationEvent&) {});

  // Blocks forever running the pairing/session lifecycle - callers may
  // invoke it from any thread/task context.
  void run();

  // --- Local control ---
  // ConnectReceiver, not Session, is the stable handle here since run()
  // rebuilds Session on every pairing (see the class comment). Returns
  // false (0 for getPositionMs()) if there's no active session yet - safe
  // to call from any thread at any time.
  bool requestPlayPause(bool play);
  bool requestNext();
  bool requestPrevious();
  bool requestSeek(uint32_t positionMs);
  bool requestSetRepeatContext(bool enabled);
  uint32_t getPositionMs();

 private:
  std::shared_ptr<AuthInfo> authInfo;
  SessionStore sessionStore;
  std::shared_ptr<AudioSink> audioSink;
  cspot::PlaybackNotificationCallback playbackNotificationCallback;

  // Set by run() on every (re)build. weak_ptr, not shared_ptr: this must
  // never be what keeps a Session alive past when run() decides to
  // replace it.
  std::mutex activeSessionMutex;
  std::weak_ptr<Session> activeSession;

  // Takes activeSessionMutex, locks the weak_ptr, and returns the result
  // (nullptr if expired/never set) - shared by every local-control method
  // above.
  std::shared_ptr<Session> lockActiveSession();
};

}  // namespace cspot
