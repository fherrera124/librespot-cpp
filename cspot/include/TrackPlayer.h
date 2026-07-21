#pragma once

#include <atomic>       // for atomic
#include <cstdint>      // for uint8_t, int64_t
#include <ctime>        // for size_t, time
#include <functional>   // for function
#include <memory>       // for shared_ptr, unique_ptr
#include <mutex>        // for mutex
#include <string_view>  // for string_view
#include <vector>       // for vector

#include "AudioSink.h"  // for AudioSink
#include "BellTask.h"  // for Task
#include "CDNAudioFile.h"
#include "TrackQueue.h"

namespace bell {
class WrappedSemaphore;
}  // namespace bell

#ifdef BELL_VORBIS_FLOAT
#include "vorbis/vorbisfile.h"
#else
#include "ivorbisfile.h"  // for OggVorbis_File, ov_callbacks
#endif

#include "MP3Decoder.h"  // for bell::MP3Decoder - podcast episodes (F60)

namespace cspot {
class TrackProvider;
class TrackQueue;
struct Context;
struct TrackReference;

class TrackPlayer : bell::Task {
 public:
  // Callback types
  typedef std::function<void(std::shared_ptr<QueuedTrack>, bool)>
      TrackLoadedCallback;
  // Fired once per track (re)start, right before the first PCM for it
  // reaches audioSink - not on every chunk. Replaces the old cross-thread
  // trackId-diffing that used to live in cspot_connect.cpp's pcmWrite()
  // (see ConnectStateHandler::notifyAudioReachedPlayback(), the callback's
  // real consumer).
  typedef std::function<void(std::string_view)> TrackReachedPlaybackCallback;
  typedef std::function<void()> EOFCallback;

  TrackPlayer(std::shared_ptr<cspot::Context> ctx,
              std::shared_ptr<cspot::TrackQueue> trackQueue,
              EOFCallback eofCallback, TrackLoadedCallback loadedCallback,
              TrackReachedPlaybackCallback reachedPlaybackCallback);
  ~TrackPlayer();

  void loadTrackFromRef(TrackReference& ref, size_t playbackMs,
                        bool startAutomatically);

  // Non-owning: cspot_connect.cpp constructs the real AudioSink once
  // (it's a long-lived hardware resource - I2S channel, ring buffer) and
  // keeps it alive across reconnects, while TrackPlayer itself is
  // recreated every session (see ConnectStateHandler's own lifetime). This
  // just points TrackPlayer at it so its own decode loop can call
  // feedPCMFrames() directly, instead of routing through a callback into
  // cspot_connect.cpp. Set once, before start().
  void setAudioSink(AudioSink* sink);
  // Pass-throughs to the sink - see the class comment on why TrackPlayer
  // drives it directly instead of cspot_connect.cpp.
  void setPaused(bool paused);
  bool isPaused() const { return paused; }
  void setVolume(uint16_t volume);
  bool setAudioParams(uint32_t sampleRate, uint8_t channelCount,
                      uint8_t bitDepth);

  // CDNTrackStream::TrackInfo getCurrentTrackInfo();
  void seekMs(size_t ms);
  void resetState(bool paused = false);

  // Real decoder position (Vorbis only, via ov_time_tell()) - freezes
  // naturally while paused (feedPCMFrames() isn't called while `paused`,
  // see setPaused()). Doesn't correct for BufferedAudioSink's downstream
  // ring buffer, so it can run ahead of what's actually audible by however
  // full that buffer is - see docs/aprendizaje.md 2026-07-18. Returns false
  // (nothing written to outMs) for MP3 (no tell() equivalent - F60) or
  // between tracks.
  bool getDecoderPositionMs(uint32_t& outMs) const;

  // Vorbis codec callbacks
  size_t _vorbisRead(void* ptr, size_t size, size_t nmemb);
  size_t _vorbisClose();
  int _vorbisSeek(int64_t offset, int whence);
  long _vorbisTell();

  void stop();
  void start();

 private:
  std::shared_ptr<cspot::Context> ctx;
  std::shared_ptr<cspot::TrackQueue> trackQueue;
  std::shared_ptr<cspot::CDNAudioFile> currentTrackStream;
  // Kept alive across tracks (TrackPlayer processes them strictly one at a
  // time, so there's never a concurrency conflict over it) - see
  // CDNConnection's own comment, CDNAudioFile.h.
  cspot::CDNConnection cdnConnection;

  std::unique_ptr<bell::WrappedSemaphore> playbackSemaphore;

  TrackLoadedCallback trackLoaded;
  TrackReachedPlaybackCallback reachedPlaybackCallback;
  EOFCallback eofCallback;

  // Non-owning - see setAudioSink(). Real output, fed directly from the
  // decode loop below.
  AudioSink* audioSink = nullptr;
  // Set via setPaused(); the decode loop stalls retrying the same chunk
  // instead of calling feedPCMFrames() while this is true - same "heard
  // immediately, never discards" contract as before (F52/F77), just
  // decided here instead of via a callback's return value.
  std::atomic<bool> paused = false;

  // Playback control
  std::atomic<bool> currentSongPlaying;
  std::mutex dataOutMutex;

  // Vorbis related
  OggVorbis_File vorbisFile;
  ov_callbacks vorbisCallbacks;
  int currentSection;

  // MP3 related (podcast episodes, see finding F60) - bell::MP3Decoder
  // (bell/main/audio-codec/MP3Decoder.cpp) wraps libhelix-mp3 directly
  // with a correctly-sized output buffer (unlike bell::EncodedAudioStream,
  // whose own output buffer is undersized for a real MP3 frame - not used
  // here for that reason). mp3InputBuffer/mp3BytesInBuffer hold
  // compressed bytes read from currentTrackStream until a sync word is
  // found, mirroring the pattern in EncodedAudioStream::decodeFrameMp3()
  // but with real bounds checking on the resync path.
  bell::MP3Decoder mp3Decoder;
  static const size_t MP3_INPUT_BUFFER_SIZE = 2 * 1024;
  std::vector<uint8_t> mp3InputBuffer =
      std::vector<uint8_t>(MP3_INPUT_BUFFER_SIZE);
  size_t mp3BytesInBuffer = 0;

  std::vector<uint8_t> pcmBuffer = std::vector<uint8_t>(1024);

  bool autoStart = false;

  std::atomic<bool> isRunning = false;
  std::atomic<bool> pendingReset = false;
  std::atomic<bool> inFuture = false;
  std::atomic<size_t> pendingSeekPositionMs = 0;
  std::atomic<bool> startPaused = false;

  // See getDecoderPositionMs().
  std::atomic<bool> hasDecoderPosition = false;
  std::atomic<uint32_t> decoderPositionMs = 0;

  std::mutex runningMutex;

  // Reads/decodes the next MP3 frame from currentTrackStream via
  // mp3Decoder, resyncing on corrupt/misaligned data. Mirrors
  // VORBIS_READ's return contract exactly: >0 is decoded PCM byte count
  // (*pcmOut points into mp3Decoder's own buffer, valid until the next
  // call - not owned by the caller), 0 is clean EOF, <0 is an
  // unrecoverable error (failed to resync within a bounded number of
  // attempts). See finding F60.
  long _mp3DecodeFrame(uint8_t** pcmOut);

  // Shared by the Vorbis/MP3 branches of runTask(): fires
  // reachedPlaybackCallback once (via notifiedThisTrack), waits out a
  // pause without discarding `data`, then feeds it to audioSink. No-op
  // (data dropped) if a reset/stop landed while waiting.
  void feedChunk(const uint8_t* data, size_t bytes, std::string_view trackId,
                bool& notifiedThisTrack);

  void runTask() override;
};
}  // namespace cspot
