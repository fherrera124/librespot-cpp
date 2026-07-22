#pragma once

#include <atomic>
#include <memory>
#include <string>

#include "BellTask.h"
#include "PlaybackEvent.h"
#include "WrappedSemaphore.h"

class AudioSink;

namespace bell {
class MDNSService;
class SimpleHTTPServer;
}  // namespace bell

namespace cspot {
class LoginBlob;
class DealerClient;

struct SpotifyConnectReceiverConfig {
  std::string deviceName;
  uint16_t initialVolume = 0;   // 0..UINT16_MAX
  int bitrate = 160;            // 96, 160 or 320 (kbps); anything else -> 160
  std::string clientId;
  std::string clientSecret;
  uint16_t zeroconfHttpPort = 8090;
};

// Runs a Spotify Connect device end-to-end: ZeroConf pairing (HTTP server +
// mDNS advertisement), session connect/auth/retry, and the actual playback
// engine (via DealerClient/PlayerEngine) - everything a consumer on
// any platform needs, given only an AudioSink and config. Platform-specific
// code (GPIO pins, a concrete AudioSink implementation) stays with the
// caller; this class never touches hardware directly.
class SpotifyConnectReceiver : public bell::Task {
 public:
  // Fired for every engine event (PLAY_PAUSE/TRACK_INFO/VOLUME/etc, see
  // PlaybackEvent.h) - after this class's own mandatory internal reactions
  // (audio params/pause/volume on the track player, ending the queue on
  // DEPLETED) already ran. Passthrough, not curated - the caller decides
  // what it cares about.
  //
  // Fired when a session starts (true) or ends (false) - a receiver-level
  // lifecycle notification, not a single cspot::EventType: there's no one
  // engine event for "the whole session just ended, retrying or waiting
  // for a new pairing attempt".
  using ConnectionStateCallback = std::function<void(bool connected)>;

  SpotifyConnectReceiver(std::unique_ptr<AudioSink> audioSink,
                         SpotifyConnectReceiverConfig config,
                         EventHandler eventHandler,
                         ConnectionStateCallback onConnectionStateChanged);
  // Defined in the .cpp, not inlined here: MDNSService/SimpleHTTPServer/
  // DealerClient are only forward-declared in this header, and the
  // implicit unique_ptr deleter needs their complete types to call
  // delete - an inline definition would break for any translation unit
  // that includes this header without also including theirs.
  ~SpotifyConnectReceiver() override;

  // Signals the task to stop and blocks until it actually has (inherited
  // stopAndWait(), via onStopRequested() below) - safe to call from any
  // task.
  void requestStop();

  // Local playback control (e.g. on-device buttons) - safe to call from
  // any task. Returns false if there's no active/linked session yet.
  bool requestPlayPause(bool play);
  bool requestNext();
  bool requestPrevious();
  bool requestSetRepeatContext(bool enabled);
  bool requestSeek(uint32_t positionMs);
  uint32_t getPositionMs();

 protected:
  void runTask() override;
  // Wakes clientConnected.wait() so runTask()'s outer loop notices
  // shouldStop() instead of staying parked forever - the same semaphore
  // a completed ZeroConf pairing gives() to start a session.
  void onStopRequested() override { clientConnected.give(); }

 private:
  std::string deviceName;
  int volume;
  int bitrate;
  uint16_t zeroconfHttpPort;
  std::string clientId;
  std::string clientSecret;
  EventHandler eventHandler;
  ConnectionStateCallback connectionStateCallback;

  std::unique_ptr<AudioSink> audioSink;
  std::unique_ptr<bell::MDNSService> mdns;

  bell::WrappedSemaphore clientConnected{1};
  std::atomic<bool> linked{false};

  std::shared_ptr<cspot::LoginBlob> blob;
  // Declared after blob/clientConnected so it's destroyed (and its accept
  // task joined) BEFORE either - its handlers run on that task and touch
  // both. Members destroy in reverse declaration order.
  std::unique_ptr<bell::SimpleHTTPServer> httpServer;
  std::unique_ptr<cspot::DealerClient> dealer;

  void startHttpServerAndMdns();

  static constexpr int SESSION_RETRY_BASE_MS = 2000;
  static constexpr int SESSION_RETRY_MAX_MS = 30000;
  static constexpr int SESSION_MAX_ATTEMPTS = 5;

  void runSession();
  void runSessionInner();
  void handleEngineEvent(std::unique_ptr<cspot::Event> event);
};

}  // namespace cspot
