#pragma once

#include <stdint.h>  // for uint8_t
#include <memory>    // for shared_ptr, unique_ptr
#include <mutex>     // for mutex
#include <string>    // for string
#include <vector>    // for vector

namespace cspot {
class AuthChallenges;
class LoginBlob;
class PlainConnection;
class ShannonConnection;
}  // namespace cspot

#define LOGIN_REQUEST_COMMAND 0xAB
#define AUTH_SUCCESSFUL_COMMAND 0xAC
#define AUTH_DECLINED_COMMAND 0xAD

namespace cspot {
class Session {
 protected:
  std::unique_ptr<cspot::AuthChallenges> challenges;
  std::shared_ptr<cspot::PlainConnection> conn;
  std::shared_ptr<LoginBlob> authBlob;

  std::string deviceId = "142137fd329622137a14901634264e6f332e2411";

  // Guards conn/shanConn themselves (the shared_ptr members, not what they
  // point to - ShannonConnection already has its own read/write mutexes for
  // that). reconnect() (MercurySession.cpp, runs on its own task) resets
  // both mid-session on any read/write error; without this, any other task
  // calling execute()/requestAudioKey() concurrently (e.g. TrackPlayer's
  // notifyAudioReachedPlayback()) can dereference a ShannonConnection being
  // destroyed at that exact moment - confirmed on real hardware (heap
  // poisoning caught a LoadProhibited crash inside shanConn's own destroyed
  // writeMutex). See docs/spotify_component_analysis.md, finding F93.
  std::mutex shanConnMutex;

 public:
  Session();
  ~Session();

  std::shared_ptr<cspot::ShannonConnection> shanConn;

  // Thread-safe accessor - takes shanConnMutex just long enough to copy the
  // shared_ptr, so the returned connection stays alive (via refcounting)
  // even if reconnect() swaps `shanConn` out immediately after. Prefer this
  // over reading `shanConn` directly from any task other than the one that
  // owns the reconnect loop (MercurySession's own task). See F93.
  std::shared_ptr<cspot::ShannonConnection> getShanConn();

  void connect(std::unique_ptr<cspot::PlainConnection> connection);
  void connectWithRandomAp();
  void close();
  virtual bool triggerTimeout() = 0;
  std::vector<uint8_t> authenticate(std::shared_ptr<LoginBlob> blob);
};
}  // namespace cspot
