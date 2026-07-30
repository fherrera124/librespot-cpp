#pragma once

#include <memory>
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
                  cspot::AudioFlushCallback audioFlushCallback = []() {});

  // Blocks forever running the pairing/session lifecycle. Callers decide
  // what thread/task context to invoke this from - it has no threading
  // requirements of its own.
  void run();

 private:
  std::shared_ptr<AuthInfo> authInfo;
  SessionStore sessionStore;
  cspot::AudioOutputCallback audioCallback;
  cspot::VolumeChangedCallback volumeCallback;
  cspot::AudioFlushCallback audioFlushCallback;
};

}  // namespace cspot
