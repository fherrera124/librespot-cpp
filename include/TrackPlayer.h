#pragma once

#include <atomic>       // for atomic
#include <cstdint>      // for uint8_t, int64_t
#include <ctime>        // for size_t, time
#include <functional>   // for function
#include <memory>       // for shared_ptr, unique_ptr
#include <mutex>        // for mutex
#include <string_view>  // for string_view

#include "AudioSink.h"  // for AudioSink
#include "BellTask.h"  // for Task
#include "CDNAudioFile.h"
#include "TrackQueue.h"

namespace bell {
class WrappedSemaphore;
}  // namespace bell

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
  // (see PlayerEngine::notifyAudioReachedPlayback(), the callback's
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
  // recreated every session (see PlayerEngine's own lifetime). This
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

  // Real decoder position, from whatever Decoder is currently playing
  // (see Decoder::getPositionMs()) - freezes naturally while paused
  // (feedPCMFrames() isn't called while `paused`, see setPaused()).
  // Doesn't correct for BufferedAudioSink's downstream ring buffer, so it
  // can run ahead of what's actually audible by however full that buffer
  // is - see docs/aprendizaje.md 2026-07-18. Returns false (nothing
  // written to outMs) for codecs with no position concept (MP3 - F60) or
  // between tracks.
  bool getDecoderPositionMs(uint32_t& outMs) const;

  void stop();
  void start();

 protected:
  // Matches stop()'s existing effect: pendingReset/currentSongPlaying so
  // any in-progress wait/decode loop notices and unwinds instead of an
  // explicit semaphore wake - runTask()'s own poll intervals (<=300ms)
  // already tolerate this latency today.
  void onStopRequested() override { resetState(); }

 private:
  std::shared_ptr<cspot::Context> ctx;
  std::shared_ptr<cspot::TrackQueue> trackQueue;
  std::shared_ptr<cspot::CDNAudioFile> currentTrackStream;
  // Kept alive across tracks (TrackPlayer processes them strictly one at a
  // time, so there's never a concurrency conflict over it) - see
  // CDNConnection's own comment, CDNAudioFile.h.
  cspot::CDNConnection cdnConnection;
  // Wired to cdnConnection.activeFd in the constructor - see
  // CDNConnection's own comment on activeFd for why this exists
  // (resetState() shutdown()s it to interrupt an in-flight CDN read).
  std::atomic<int> activeCdnFd{-1};

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

  bool autoStart = false;

  // Guards start() against being called more than once - unrelated to
  // the base class's own stop-tracking (shouldStop()/stopAndWait()).
  bool started = false;
  std::atomic<bool> pendingReset = false;
  std::atomic<bool> inFuture = false;
  std::atomic<size_t> pendingSeekPositionMs = 0;
  std::atomic<bool> startPaused = false;

  // See getDecoderPositionMs().
  std::atomic<bool> hasDecoderPosition = false;
  std::atomic<uint32_t> decoderPositionMs = 0;

  // Recomputed once per track (runTask(), right after openStream()) from
  // its own CDN-embedded gain/peak - see LoudnessNormalisation.h. Only ever
  // touched from this task's own thread, same as currentTrackStream/decoder.
  float normalizationGain = 1.0f;

  // Fires reachedPlaybackCallback once (via notifiedThisTrack), waits out
  // a pause without discarding `data`, then feeds it to audioSink. No-op
  // (data dropped) if a reset/stop landed while waiting. Codec-agnostic -
  // called with whatever the current Decoder::readChunk() produced.
  // Non-const: applies normalizationGain to `data` in place before it's
  // handed onward - safe, `data` points into the decoder's own scratch
  // buffer, about to be overwritten by the next readChunk() anyway.
  void feedChunk(uint8_t* data, size_t bytes, std::string_view trackId,
                bool& notifiedThisTrack);

  enum class LoadWaitOutcome { READY, FAILED, RESET, TIMED_OUT };
  // Waits for `track` to leave its preload pipeline (READY/FAILED), for
  // pendingReset to be raised (RESET - some other thread told us to
  // abandon whatever we're doing, via resetState()), or for timeoutMs to
  // elapse (TIMED_OUT). Polls track.getState()/pendingReset every
  // kLoadWaitPollMs instead of one long twait(), so a reset lands within
  // one poll interval instead of only being noticed once the track's own
  // loadedSemaphore fires or the full timeout elapses - see
  // docs/aprendizaje.md 2026-07-21 for the reasoning (why this doesn't
  // need a mutex/condition_variable shared with QueuedTrack/TrackQueue:
  // every cross-thread caller that changes the current track already
  // pairs it with resetState(), verified against every PlaybackController
  // call site - the two that don't are self-reentrant from this task's
  // own thread and don't need the signal).
  LoadWaitOutcome waitForTrackReady(QueuedTrack& track, uint32_t timeoutMs);

  void runTask() override;
};
}  // namespace cspot
