#pragma once

#include <memory>
#include <mutex>
#include <string>

#include "AuthInfo.h"
#include "Session.h"
#include "SessionStore.h"
#include "tracks/AudioDecoder.h"

namespace cspot {

// Runs the full Spotify Connect receiver lifecycle: loads any persisted
// session, pairs via ZeroconfServer when needed, and builds/rebuilds a
// Session for each pairing until the process is killed. Shared by every
// target - the only per-target inputs are the audio callbacks and where to
// persist the session file.
class ConnectReceiver {
 public:
  ConnectReceiver(std::shared_ptr<AuthInfo> authInfo,
                  std::string sessionFilePath,
                  cspot::AudioOutputCallback audioCallback,
                  cspot::VolumeChangedCallback volumeCallback =
                      [](uint16_t) {},
                  cspot::AudioFlushCallback audioFlushCallback = []() {},
                  cspot::PlaybackNotificationCallback
                      playbackNotificationCallback =
                          [](const PlaybackNotificationEvent&) {});

  // Blocks forever running the pairing/session lifecycle. Callers decide
  // what thread/task context to invoke this from - it has no threading
  // requirements of its own.
  void run();

  // --- Local control ---
  // The stable handle an embedding app actually holds onto across
  // reconnects/re-pairings - run()'s own Session is rebuilt from scratch on
  // each of those (see run()'s own comment), so this is the one object
  // whose identity survives long enough to be useful from outside.
  // Returns false (0 for getPositionMs()) if there's no active/linked
  // session yet - safe to call from any thread at any time, including
  // before run() has paired with anything.
  bool requestPlayPause(bool play);
  bool requestNext();
  bool requestPrevious();
  bool requestSeek(uint32_t positionMs);
  bool requestSetRepeatContext(bool enabled);
  uint32_t getPositionMs();

 private:
  std::shared_ptr<AuthInfo> authInfo;
  SessionStore sessionStore;
  cspot::AudioOutputCallback audioCallback;
  cspot::VolumeChangedCallback volumeCallback;
  cspot::AudioFlushCallback audioFlushCallback;
  cspot::PlaybackNotificationCallback playbackNotificationCallback;

  // Published by run() every time it (re)builds a Session, read by the
  // local-control methods above from whatever thread the embedding app
  // calls them on. weak_ptr, not shared_ptr: this must never be what keeps
  // a Session alive past when run()'s own loop decides to replace it.
  std::mutex activeSessionMutex;
  std::weak_ptr<Session> activeSession;

  // Takes activeSessionMutex, locks the weak_ptr, and returns the result
  // (nullptr if expired/never set) - shared by every local-control method
  // above.
  std::shared_ptr<Session> lockActiveSession();
};

}  // namespace cspot
