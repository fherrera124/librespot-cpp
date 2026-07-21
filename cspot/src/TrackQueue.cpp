#include "TrackQueue.h"
#include <pb_decode.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <mutex>

#include "AccessKeyFetcher.h"
#include "BellTask.h"
#include "CDNAudioFile.h"
#include "CSpotContext.h"
#include "HTTPClient.h"
#include "Logger.h"
#include "NanoPBHelper.h"  // for pbDecode, pbArrayToVector
#include "Utils.h"
#include "WrappedSemaphore.h"
#ifdef BELL_ONLY_CJSON
#include "cJSON.h"
#else
#include "nlohmann/json.hpp"      // for basic_json<>::object_t, basic_json
#include "nlohmann/json_fwd.hpp"  // for json
#endif
#include "protobuf/metadata.pb.h"

using namespace cspot;

namespace {
// Bounded retry for the network-dependent preload steps (Mercury metadata,
// audio key, CDN URL resolve). Before this, a single transient failure at
// any of the three permanently skipped the track - observed for real on an
// unstable mobile uplink (2026-07-17 hardware log): two Dealer WS
// reconnects and three straight "Track failed to load" within ~15s, all
// inside the same network blip that also caused a context-resolve failure.
// 3 attempts, 1s apart, is deliberately modest - it covers a fast-failing
// transient error (connection refused, empty/garbled body, HTTP error
// status - exactly what the log showed), not a link that's actually down
// for many seconds. TrackPlayer's own wait for a track to become READY
// (TrackPlayer.cpp) was widened to fit this budget.
constexpr int MAX_LOAD_ATTEMPTS = 3;  // 1 initial + 2 retries
constexpr auto LOAD_RETRY_BACKOFF = std::chrono::milliseconds(1000);
}  // namespace

namespace TrackDataUtils {
bool countryListContains(char* countryList, const char* country) {
  uint16_t countryList_length = strlen(countryList);
  for (int x = 0; x < countryList_length; x += 2) {
    if (countryList[x] == country[0] && countryList[x + 1] == country[1]) {
      return true;
    }
  }
  return false;
}

bool doRestrictionsApply(Restriction* restrictions, int count,
                         const char* country) {
  for (int x = 0; x < count; x++) {
    if (restrictions[x].countries_allowed != nullptr) {
      return !countryListContains(restrictions[x].countries_allowed, country);
    }

    if (restrictions[x].countries_forbidden != nullptr) {
      return countryListContains(restrictions[x].countries_forbidden, country);
    }
  }

  return false;
}

bool canPlayTrack(Track& trackInfo, int altIndex, const char* country) {
  if (altIndex < 0) {

  } else {
    for (int x = 0; x < trackInfo.alternative[altIndex].restriction_count;
         x++) {
      if (trackInfo.alternative[altIndex].restriction[x].countries_allowed !=
          nullptr) {
        return countryListContains(
            trackInfo.alternative[altIndex].restriction[x].countries_allowed,
            country);
      }

      if (trackInfo.alternative[altIndex].restriction[x].countries_forbidden !=
          nullptr) {
        return !countryListContains(
            trackInfo.alternative[altIndex].restriction[x].countries_forbidden,
            country);
      }
    }
  }
  return true;
}
}  // namespace TrackDataUtils

void TrackInfo::loadPbTrack(Track* pbTrack, const std::vector<uint8_t>& gid) {
  // Generate ID based on GID
  trackId = bytesToHexString(gid);

  name = std::string(pbTrack->name);

  if (pbTrack->artist_count > 0) {
    // Handle artist data
    artist = std::string(pbTrack->artist[0].name);
  }

  if (pbTrack->has_album) {
    // Handle album data
    album = std::string(pbTrack->album.name);

    if (pbTrack->album.has_cover_group &&
        pbTrack->album.cover_group.image_count > 0) {
      auto imageId =
          pbArrayToVector(pbTrack->album.cover_group.image[0].file_id);
      imageUrl = "https://i.scdn.co/image/" + bytesToHexString(imageId);
    }
  }

  number = pbTrack->has_number ? pbTrack->number : 0;
  discNumber = pbTrack->has_disc_number ? pbTrack->disc_number : 0;
  duration = pbTrack->duration;
}

void TrackInfo::loadPbEpisode(Episode* pbEpisode,
                              const std::vector<uint8_t>& gid) {
  // Generate ID based on GID
  trackId = bytesToHexString(gid);

  name = std::string(pbEpisode->name);

  if (pbEpisode->covers->image_count > 0) {
    // Handle episode info
    auto imageId = pbArrayToVector(pbEpisode->covers->image[0].file_id);
    imageUrl = "https://i.scdn.co/image/" + bytesToHexString(imageId);
  }

  number = pbEpisode->has_number ? pbEpisode->number : 0;
  discNumber = 0;
  duration = pbEpisode->duration;
}

QueuedTrack::QueuedTrack(TrackReference& ref,
                         std::shared_ptr<cspot::Context> ctx,
                         uint32_t requestedPosition)
    : requestedPosition(requestedPosition), ctx(ctx) {
  this->ref = ref;

  loadedSemaphore = std::make_shared<bell::WrappedSemaphore>();
}

QueuedTrack::~QueuedTrack() {
  setState(State::FAILED);
  loadedSemaphore->give();

  if (pendingMercuryRequest != 0) {
    ctx->session->unregister(pendingMercuryRequest);
  }

  if (pendingAudioKeyRequest != 0) {
    ctx->session->unregisterAudioKey(pendingAudioKeyRequest);
  }
}

// Once failed, stay failed: a late-firing retry/success callback from an
// attempt this object has already given up on (see the catch-up-skip
// comment in reachedPlaybackCallback, PlaybackController.cpp, for the
// same class of out-of-order-callback risk) must not resurrect a track
// past this point.
void QueuedTrack::setState(State newState) {
  if (state.load() == State::FAILED) {
    return;
  }
  state = newState;
}

// CDNAudioFile sizes its CDN range-request buffer off this (see F84) -
// keeps the same ~2.4s-per-request margin regardless of quality setting.
// Unknown/unmapped formats fall back to 160, same as CSPOT_BITRATE's own
// Kconfig help text ("any other value falls back to 160 at runtime").
static int audioFormatBitrateKbps(AudioFormat format) {
  switch (format) {
    case AudioFormat_OGG_VORBIS_96:
    case AudioFormat_MP3_96:
      return 96;
    case AudioFormat_OGG_VORBIS_320:
    case AudioFormat_MP3_320:
      return 320;
    case AudioFormat_MP3_256:
      return 256;
    default:
      return 160;
  }
}

std::shared_ptr<cspot::CDNAudioFile> QueuedTrack::getAudioFile(
    CDNConnection& connection) {
  if (getState() != State::READY) {
    return nullptr;
  }

  return std::make_shared<cspot::CDNAudioFile>(
      cdnUrl, audioKey, audioFormatBitrateKbps(selectedFormat), connection);
}

void QueuedTrack::stepParseMetadata(Track* pbTrack, Episode* pbEpisode) {
  int filesCount = 0;
  AudioFile* selectedFiles = nullptr;

  const char* countryCode = ctx->config.countryCode.c_str();

  if (ref.type == TrackReference::Type::TRACK) {
    CSPOT_LOG(info, "Track name: %s", pbTrack->name);
    CSPOT_LOG(info, "Track duration: %d", pbTrack->duration);

    CSPOT_LOG(debug, "trackInfo.restriction.size() = %d",
              pbTrack->restriction_count);

    // Check if we can play the track, if not, try alternatives
    if (TrackDataUtils::doRestrictionsApply(
            pbTrack->restriction, pbTrack->restriction_count, countryCode)) {
      // Go through alternatives
      for (int x = 0; x < pbTrack->alternative_count; x++) {
        if (!TrackDataUtils::doRestrictionsApply(
                pbTrack->alternative[x].restriction,
                pbTrack->alternative[x].restriction_count, countryCode)) {
          selectedFiles = pbTrack->alternative[x].file;
          filesCount = pbTrack->alternative[x].file_count;
          trackId = pbArrayToVector(pbTrack->alternative[x].gid);
          break;
        }
      }
    } else {
      // We can play the track
      selectedFiles = pbTrack->file;
      filesCount = pbTrack->file_count;
      trackId = pbArrayToVector(pbTrack->gid);
    }

    if (trackId.size() > 0) {
      // Load track information
      trackInfo.loadPbTrack(pbTrack, trackId);
    }
  } else {
    // Handle episodes
    CSPOT_LOG(info, "Episode name: %s", pbEpisode->name);
    CSPOT_LOG(info, "Episode duration: %d", pbEpisode->duration);

    CSPOT_LOG(debug, "episodeInfo.restriction.size() = %d",
              pbEpisode->restriction_count);

    // Check if we can play the episode
    if (!TrackDataUtils::doRestrictionsApply(pbEpisode->restriction,
                                             pbEpisode->restriction_count,
                                             countryCode)) {
      selectedFiles = pbEpisode->file;
      filesCount = pbEpisode->file_count;
      trackId = pbArrayToVector(pbEpisode->gid);

      // Load track information
      trackInfo.loadPbEpisode(pbEpisode, trackId);
    }
  }

  std::vector<uint8_t> mp3FallbackFileId;
  AudioFormat mp3FallbackFormat = AudioFormat_MP3_160;

  // Find playable file
  for (int x = 0; x < filesCount; x++) {
    CSPOT_LOG(debug, "File format: %d", selectedFiles[x].format);
    if (selectedFiles[x].format == ctx->config.audioFormat) {
      fileId = pbArrayToVector(selectedFiles[x].file_id);
      selectedFormat = selectedFiles[x].format;
      break;  // If file found stop searching
    }

    // Fallback to OGG Vorbis 96kbps
    if (fileId.size() == 0 &&
        selectedFiles[x].format == AudioFormat_OGG_VORBIS_96) {
      fileId = pbArrayToVector(selectedFiles[x].file_id);
      selectedFormat = selectedFiles[x].format;
    }

    // Second-tier fallback: MP3, only actually used below if nothing Ogg
    // Vorbis matched at all in this file list. ctx->config.audioFormat is
    // always one of the OGG_VORBIS_* values (see CSpotContext.h), so it -
    // and the 96kbps fallback above - can never match an MP3-only file
    // list, which is the typical case for a podcast episode. Prefer
    // 160kbps, else whichever MP3 bitrate is available. See
    // docs/spotify_component_analysis.md, finding F60.
    bool isMp3 = selectedFiles[x].format == AudioFormat_MP3_256 ||
                selectedFiles[x].format == AudioFormat_MP3_320 ||
                selectedFiles[x].format == AudioFormat_MP3_160 ||
                selectedFiles[x].format == AudioFormat_MP3_96 ||
                selectedFiles[x].format == AudioFormat_MP3_160_ENC;
    if (isMp3 && (mp3FallbackFileId.size() == 0 ||
                 selectedFiles[x].format == AudioFormat_MP3_160)) {
      mp3FallbackFileId = pbArrayToVector(selectedFiles[x].file_id);
      mp3FallbackFormat = selectedFiles[x].format;
    }
  }

  if (fileId.size() == 0 && mp3FallbackFileId.size() > 0) {
    fileId = mp3FallbackFileId;
    selectedFormat = mp3FallbackFormat;
  }

  // No viable files found for playback
  if (fileId.size() == 0) {
    CSPOT_LOG(info, "File not available for playback");

    // no alternatives for song
    setState(State::FAILED);
    loadedSemaphore->give();
    return;
  }

  // Assign track identifier
  identifier = bytesToHexString(fileId);

  setState(State::KEY_REQUIRED);
}

void QueuedTrack::stepLoadAudioFile(
    std::mutex& trackListMutex,
    std::shared_ptr<bell::WrappedSemaphore> updateSemaphore) {
  if (std::chrono::steady_clock::now() < retryNotBeforeTime) {
    return;  // still backing off from a previous failed attempt
  }

  // Request audio key
  this->pendingAudioKeyRequest = ctx->session->requestAudioKey(
      trackId, fileId,
      [this, &trackListMutex, updateSemaphore](
          bool success, const std::vector<uint8_t>& audioKey) {
        std::scoped_lock lock(trackListMutex);

        if (success) {
          CSPOT_LOG(info, "Got audio key");
          this->audioKey =
              std::vector<uint8_t>(audioKey.begin() + 4, audioKey.end());

          loadAttempts = 0;  // fresh budget for the CDN URL step
          setState(State::CDN_REQUIRED);
        } else if (++loadAttempts < MAX_LOAD_ATTEMPTS) {
          CSPOT_LOG(error, "Failed to get audio key, retrying (%d/%d)",
                   loadAttempts, MAX_LOAD_ATTEMPTS - 1);
          retryNotBeforeTime =
              std::chrono::steady_clock::now() + LOAD_RETRY_BACKOFF;
          setState(State::KEY_REQUIRED);  // processTrack() calls this step again
        } else {
          CSPOT_LOG(error, "Failed to get audio key, giving up after %d attempts",
                   MAX_LOAD_ATTEMPTS);
          setState(State::FAILED);
          loadedSemaphore->give();
        }
        updateSemaphore->give();
      });

  setState(State::PENDING_KEY);
}

void QueuedTrack::stepLoadCDNUrl(const std::string& accessKey) {
  if (accessKey.size() == 0) {
    // Wait for access key
    return;
  }

  if (std::chrono::steady_clock::now() < retryNotBeforeTime) {
    return;  // still backing off from a previous failed attempt
  }

  // Request CDN URL
  CSPOT_LOG(info, "Received access key, fetching CDN URL...");

  auto retryOrFail = [this](const char* reason) {
    if (++loadAttempts < MAX_LOAD_ATTEMPTS) {
      CSPOT_LOG(error, "Cannot fetch CDN URL: %s, retrying (%d/%d)", reason,
               loadAttempts, MAX_LOAD_ATTEMPTS - 1);
      retryNotBeforeTime =
          std::chrono::steady_clock::now() + LOAD_RETRY_BACKOFF;
      setState(State::CDN_REQUIRED);  // processTrack() calls this step again
    } else {
      CSPOT_LOG(error, "Cannot fetch CDN URL: %s, giving up after %d attempts",
               reason, MAX_LOAD_ATTEMPTS);
      setState(State::FAILED);
      loadedSemaphore->give();
    }
  };

  try {

    std::string requestUrl = string_format(
        "https://api.spotify.com/v1/storage-resolve/files/audio/interactive/"
        "%s?alt=json&product=9",
        bytesToHexString(fileId).c_str());

    auto req = bell::HTTPClient::get(
        requestUrl, {bell::HTTPClient::ValueHeader(
                        {"Authorization", "Bearer " + accessKey})});

    // Wait for response
    std::string_view result = req->body();

    // A non-2xx or empty body (rate limiting, backend hiccup) used to go
    // straight into the JSON parser with zero context on why it failed -
    // now checked and logged before parsing. See F80.
    if (req->statusCode() < 200 || req->statusCode() >= 300) {
      CSPOT_LOG(error, "storage-resolve HTTP %d, body=%.*s", req->statusCode(),
                (int)std::min<size_t>(result.size(), 240), result.data());
      throw std::runtime_error("storage-resolve HTTP error");
    }
    if (result.empty()) {
      throw std::runtime_error("storage-resolve returned an empty body");
    }

#ifdef BELL_ONLY_CJSON
    // cJSON_Parse()/cJSON_GetObjectItem() both return null on failure - the
    // original code dereferenced the result unconditionally, a null deref
    // waiting for a malformed/unexpected response. See F80.
    cJSON* jsonResult = cJSON_Parse(result.data());
    if (jsonResult == nullptr) {
      throw std::runtime_error("storage-resolve returned invalid JSON");
    }
    cJSON* cdnUrlItem =
        cJSON_GetArrayItem(cJSON_GetObjectItem(jsonResult, "cdnurl"), 0);
    if (cdnUrlItem == nullptr || cdnUrlItem->valuestring == nullptr) {
      cJSON_Delete(jsonResult);
      throw std::runtime_error("storage-resolve JSON missing cdnurl");
    }
    cdnUrl = cdnUrlItem->valuestring;
    cJSON_Delete(jsonResult);
#else
    auto jsonResult = nlohmann::json::parse(result);
    cdnUrl = jsonResult["cdnurl"][0];
#endif

    CSPOT_LOG(info, "Received CDN URL, %s", cdnUrl.c_str());
    setState(State::READY);
    loadedSemaphore->give();
  } catch (const std::exception& e) {
    retryOrFail(e.what());
  } catch (...) {
    retryOrFail("unknown error");
  }
}

void QueuedTrack::expire() {
  if (getState() != State::QUEUED) {
    setState(State::FAILED);
    loadedSemaphore->give();
  }
}

void QueuedTrack::stepLoadMetadata(
    Track* pbTrack, Episode* pbEpisode, std::mutex& trackListMutex,
    std::shared_ptr<bell::WrappedSemaphore> updateSemaphore) {
  if (std::chrono::steady_clock::now() < retryNotBeforeTime) {
    return;  // still backing off from a previous failed attempt
  }

  // Prepare request ID
  std::string requestUrl = string_format(
      "hm://metadata/3/%s/%s",
      ref.type == TrackReference::Type::TRACK ? "track" : "episode",
      bytesToHexString(ref.gid).c_str());

  auto responseHandler = [this, pbTrack, pbEpisode, &trackListMutex,
                          updateSemaphore](MercurySession::Response& res) {
    std::scoped_lock lock(trackListMutex);

    if (res.parts.size() == 0) {
      if (++loadAttempts < MAX_LOAD_ATTEMPTS) {
        CSPOT_LOG(error, "Empty metadata response, retrying (%d/%d)",
                 loadAttempts, MAX_LOAD_ATTEMPTS - 1);
        retryNotBeforeTime =
            std::chrono::steady_clock::now() + LOAD_RETRY_BACKOFF;
        setState(State::QUEUED);  // processTrack() calls this step again
      } else {
        CSPOT_LOG(error,
                 "Empty metadata response, giving up after %d attempts",
                 MAX_LOAD_ATTEMPTS);
        setState(State::FAILED);
        loadedSemaphore->give();
      }
      updateSemaphore->give();
      return;
    }

    // Parse the metadata
    if (ref.type == TrackReference::Type::TRACK) {
      pb_release(Track_fields, pbTrack);
      pbDecode(*pbTrack, Track_fields, res.parts[0]);
    } else {
      pb_release(Episode_fields, pbEpisode);
      pbDecode(*pbEpisode, Episode_fields, res.parts[0]);
    }

    loadAttempts = 0;  // fresh budget for the next step

    // Parse received metadata
    stepParseMetadata(pbTrack, pbEpisode);

    updateSemaphore->give();
  };
  // Execute the request
  pendingMercuryRequest = ctx->session->execute(
      MercurySession::RequestType::GET, requestUrl, responseHandler);

  // Set the state to pending
  setState(State::PENDING_META);
}

TrackQueue::TrackQueue(std::shared_ptr<cspot::Context> ctx,
                      std::shared_ptr<cspot::AccessKeyFetcher> accessKeyFetcher)
    : bell::Task("CSpotTrackQueue", 1024 * 32, 2, 1), ctx(ctx),
      accessKeyFetcher(accessKeyFetcher) {
  processSemaphore = std::make_shared<bell::WrappedSemaphore>();
  playableSemaphore = std::make_shared<bell::WrappedSemaphore>();

  pbTrack = Track_init_zero;
  pbEpisode = Episode_init_zero;

  // Start the task
  startTask();
};

TrackQueue::~TrackQueue() {
  stopTask();

  std::scoped_lock lock(tracksMutex);

  pb_release(Track_fields, &pbTrack);
  pb_release(Episode_fields, &pbEpisode);
}

TrackInfo TrackQueue::getTrackInfo(std::string_view identifier) {
  for (auto& track : preloadedTracks) {
    if (track->identifier == identifier)
      return track->trackInfo;
  }
  return TrackInfo{};
}

void TrackQueue::runTask() {
  isRunning = true;

  std::scoped_lock lock(runningMutex);

  std::deque<std::shared_ptr<QueuedTrack>> trackQueue;

  while (isRunning) {
    processSemaphore->twait(100);

    // Make sure we have the newest access key
    accessKey = accessKeyFetcher->getAccessKey();

    int loadedIndex = currentTracksIndex;

    // No tracks loaded yet
    if (loadedIndex < 0) {
      continue;
    } else {
      std::scoped_lock lock(tracksMutex);

      trackQueue = preloadedTracks;
    }

    for (auto& track : trackQueue) {
      if (track) {
        this->processTrack(track);
      }
    }
  }
}

void TrackQueue::stopTask() {
  if (isRunning) {
    isRunning = false;
    processSemaphore->give();
    std::scoped_lock lock(runningMutex);
  }
}

void TrackQueue::setRepeatContext(bool repeat) {
  std::scoped_lock lock(tracksMutex);
  shouldRepeatContext = repeat;
}

bool TrackQueue::isRepeatingContext() {
  std::scoped_lock lock(tracksMutex);
  return shouldRepeatContext;
}

// Repeat-context (F92): when the last track finishes and repeat is on,
// loop back to the first track instead of ending. Neither classic SPIRC
// nor this project's connect-state Fase 6 command handling distinguish
// repeating_context from repeating_track (see the "no per-track repeat"
// note where updateTracks() is declared) - one flag, whole-queue repeat.
// Mirrors updateTracks()'s "initial" branch (reset index, clear+requeue
// from 0) but reuses the already-known currentTracks directly instead of
// taking a tracks list parameter - the list itself hasn't changed.
void TrackQueue::restartFromBeginning() {
  std::scoped_lock lock(tracksMutex);
  if (currentTracks.empty()) return;

  currentTracksIndex = 0;
  pendingQueuedCount = 0;
  preloadedTracks.clear();
  queueNextTrack(0);

  // Same as updateTracks(initial): notifyAudioReachedPlayback() should
  // treat the next track-start as "position already known (0)", not
  // walk the queue forward with skipTrack(NEXT).
  notifyPending = true;
  playableSemaphore->give();
}

std::shared_ptr<QueuedTrack> TrackQueue::consumeTrack(
    std::shared_ptr<QueuedTrack> prevTrack, int& offset) {
  std::scoped_lock lock(tracksMutex);

  if (currentTracksIndex == -1 || currentTracksIndex >= currentTracks.size()) {
    return nullptr;
  }

  // No previous track, return head
  if (prevTrack == nullptr) {
    offset = 0;

    if (preloadedTracks.empty()) {
      return nullptr;
    }

    // A permanently failed head (metadata/audio-key/CDN all exhausted
    // their own error paths into State::FAILED) never gets popped by
    // skipTrack(NEXT) when it's also the last track in whatever window
    // the client last sent (skipTrack(NEXT) can't advance past the end).
    // Returning it here anyway sent TrackPlayer's failure-retry loop
    // (see F89) into hammering the same broken track forever - a fresh
    // "Track failed to load" + Mercury notify + full state reset every
    // ~5s, indefinitely. Nothing to play until the client sends a new
    // Load with more tracks. See F94.
    if (preloadedTracks[0]->getState() == QueuedTrack::State::FAILED) {
      return nullptr;
    }

    return preloadedTracks[0];
  }

  // if (currentTracksIndex + preloadedTracks.size() >= currentTracks.size()) {
  //   offset = -1;

  //   // Last track in queue
  //   return nullptr;
  // }

  auto prevTrackIter =
      std::find(preloadedTracks.begin(), preloadedTracks.end(), prevTrack);

  if (prevTrackIter != preloadedTracks.end()) {
    // Get offset of next track
    offset = prevTrackIter - preloadedTracks.begin() + 1;
  } else {
    offset = 0;
  }

  if (offset >= preloadedTracks.size()) {
    // Last track in preloaded queue
    return nullptr;
  }

  // Return the current track
  return preloadedTracks[offset];
}

void TrackQueue::processTrack(std::shared_ptr<QueuedTrack> track) {
  switch (track->getState()) {
    case QueuedTrack::State::QUEUED:
      track->stepLoadMetadata(&pbTrack, &pbEpisode, tracksMutex,
                              processSemaphore);
      break;
    case QueuedTrack::State::KEY_REQUIRED:
      track->stepLoadAudioFile(tracksMutex, processSemaphore);
      break;
    case QueuedTrack::State::CDN_REQUIRED:
      track->stepLoadCDNUrl(accessKey);

      if (track->getState() == QueuedTrack::State::READY) {
        // runTask() calls processTrack() outside tracksMutex (it only
        // locks to copy preloadedTracks before iterating) - queueNextTrack()
        // mutates the real preloadedTracks, so it needs its own lock here,
        // same as every other caller (consumeTrack/skipTrack/updateTracks).
        // Scoped to just this, not the whole function, since
        // stepLoadCDNUrl() above is a blocking network call. See F78.
        std::scoped_lock lock(tracksMutex);
        if (preloadedTracks.size() < MAX_TRACKS_PRELOAD) {
          // Queue a new track to preload
          queueNextTrack(preloadedTracks.size());
        }
      }
      break;
    default:
      // Do not perform any action
      break;
  }
}

bool TrackQueue::queueNextTrack(int offset, uint32_t positionMs) {
  const int requestedRefIndex = offset + currentTracksIndex;

  if (requestedRefIndex < 0 || requestedRefIndex >= currentTracks.size()) {
    return false;
  }

  // in case we re-queue current track, make sure position is updated (0)
  if (offset == 0 && preloadedTracks.size() &&
      preloadedTracks[0]->ref == currentTracks[currentTracksIndex]) {
    preloadedTracks.pop_front();
  }

  if (offset <= 0) {
    preloadedTracks.push_front(std::make_shared<QueuedTrack>(
        currentTracks[requestedRefIndex], ctx, positionMs));
  } else {
    preloadedTracks.push_back(std::make_shared<QueuedTrack>(
        currentTracks[requestedRefIndex], ctx, positionMs));
  }

  return true;
}

bool TrackQueue::skipTrack(SkipDirection dir, uint32_t currentPositionMs,
                           bool expectNotify, bool allowSeeking) {
  bool skipped = true;
  std::scoped_lock lock(tracksMutex);

  if (dir == SkipDirection::PREV) {
    if (currentTracksIndex > 0 && (!allowSeeking || currentPositionMs < 3000)) {
      currentTracksIndex--;
      // Every preloaded entry (current + lookahead) was fetched relative
      // to the OLD current index - none of it is valid once we move
      // backward (what used to be "next"/"next-next" no longer follows
      // the new current track, and the old front is a stale, already-
      // played QueuedTrack). Same pattern updateTracks(initial=true)
      // uses for a fresh load: clear and push just the new current
      // track, let the background prefetch in processTrack()/runTask()
      // repopulate the lookahead window naturally from here, instead of
      // trying to hand-patch stale entries into the right positions.
      preloadedTracks.clear();
      queueNextTrack(0);
    } else {
      queueNextTrack(0);
    }
  } else {
    if (currentTracks.size() > currentTracksIndex + 1) {
      preloadedTracks.pop_front();

      if (!queueNextTrack(preloadedTracks.size() + 1)) {
        CSPOT_LOG(info, "Failed to queue next track");
      }

      currentTracksIndex++;
      // Playback advanced past one pending "add to queue" track (if any) -
      // see insertNext().
      if (pendingQueuedCount > 0) {
        pendingQueuedCount--;
      }
    } else {
      skipped = false;
    }
  }

  if (skipped) {
    if (expectNotify) {
      // Reset position to zero
      notifyPending = true;
    }
  }

  return skipped;
}

bool TrackQueue::hasTracks() {
  std::scoped_lock lock(tracksMutex);

  return currentTracks.size() > 0;
}

bool TrackQueue::isFinished() {
  std::scoped_lock lock(tracksMutex);
  return currentTracksIndex >= currentTracks.size() - 1;
}

std::vector<TrackReference> TrackQueue::getPrevTracks(size_t maxCount) {
  std::scoped_lock lock(tracksMutex);
  std::vector<TrackReference> result;
  if (currentTracksIndex <= 0) {
    return result;
  }
  // Closest maxCount tracks immediately before the current one, in
  // playback order (oldest of the window first) - same convention as
  // go-librespot's PrevTracks() (tracks/tracks.go).
  size_t from = (size_t)currentTracksIndex > maxCount
                    ? (size_t)currentTracksIndex - maxCount
                    : 0;
  for (size_t i = from; i < (size_t)currentTracksIndex; i++) {
    result.push_back(currentTracks[i]);
  }
  return result;
}

std::vector<TrackReference> TrackQueue::getNextTracks(size_t maxCount) {
  std::scoped_lock lock(tracksMutex);
  std::vector<TrackReference> result;
  if (currentTracksIndex < 0 ||
      (size_t)currentTracksIndex + 1 >= currentTracks.size()) {
    return result;
  }
  size_t from = (size_t)currentTracksIndex + 1;
  size_t to = std::min(from + maxCount, currentTracks.size());
  for (size_t i = from; i < to; i++) {
    result.push_back(currentTracks[i]);
  }
  return result;
}

bool TrackQueue::updateTracks(const std::vector<TrackReference>& tracks,
                              int startIndex, uint32_t requestedPosition,
                              bool initial) {
  std::scoped_lock lock(tracksMutex);
  bool cleared = true;

  // Copy requested track list
  currentTracks = tracks;
  currentTracksIndex = startIndex;
  pendingQueuedCount = 0;

  if (initial) {
    // Clear preloaded tracks
    preloadedTracks.clear();

    if (currentTracksIndex < currentTracks.size()) {
      // Push a song on the preloaded queue
      queueNextTrack(0, requestedPosition);
    }

    // We already updated track meta, mark it
    notifyPending = true;

    playableSemaphore->give();
    // FIX: preloadedTracks can be empty here (e.g. a REPLACE frame -
    // SpircHandler::handleFrame() -> updateTracks(false) - arriving before
    // any LOAD ever populated it) - indexing [0] without the emptiness
    // check first was an out-of-bounds deque access driven directly by a
    // remote client's SPIRC frame. See
    // docs/spotify_component_analysis.md, finding F25.
  } else if (!preloadedTracks.empty() &&
           preloadedTracks[0]->getState() != QueuedTrack::State::READY &&
           preloadedTracks[0]->getState() != QueuedTrack::State::FAILED) {
    // try to not re-load track if we are still loading it

    // remove everything except first track
    preloadedTracks.erase(preloadedTracks.begin() + 1, preloadedTracks.end());

    // Push a song on the preloaded queue
    CSPOT_LOG(info, "Keeping current track %d", currentTracksIndex);
    queueNextTrack(1);

    cleared = false;
  } else {
    // Clear preloaded tracks
    preloadedTracks.clear();

    // Push a song on the preloaded queue
    CSPOT_LOG(info, "Re-loading current track");
    queueNextTrack(0, requestedPosition);
  }

  return cleared;
}

bool TrackQueue::insertNext(const TrackReference& track) {
  std::scoped_lock lock(tracksMutex);
  if (currentTracksIndex < 0 || currentTracksIndex >= currentTracks.size() ||
      preloadedTracks.empty()) {
    return false;
  }

  // After the current track AND after any still-pending earlier inserts -
  // FIFO, matching the real apps' queue semantics (inserting always at
  // index+1 would play consecutive adds in reverse).
  int insertPos = currentTracksIndex + 1 + pendingQueuedCount;
  if (insertPos > (int)currentTracks.size()) {
    insertPos = currentTracks.size();
  }
  currentTracks.insert(currentTracks.begin() + insertPos, track);
  pendingQueuedCount++;

  // Preloads from the insertion point on now point at the wrong tracks -
  // drop and requeue them. If the insert landed beyond the preload window,
  // nothing is stale and the preloader picks it up naturally.
  int offset = insertPos - currentTracksIndex;
  if (offset < (int)preloadedTracks.size()) {
    preloadedTracks.erase(preloadedTracks.begin() + offset,
                          preloadedTracks.end());
    queueNextTrack(offset);
    processSemaphore->give();
  }

  CSPOT_LOG(info, "add_to_queue: inserted at %d (%d pending queued)",
            insertPos, (int)pendingQueuedCount);
  return true;
}

bool TrackQueue::replaceUpcoming(
    const std::vector<TrackReference>& prevTracks,
    const std::vector<TrackReference>& nextTracks) {
  std::scoped_lock lock(tracksMutex);
  if (currentTracksIndex < 0 || currentTracksIndex >= currentTracks.size() ||
      preloadedTracks.empty()) {
    return false;
  }

  TrackReference current = currentTracks[currentTracksIndex];
  std::vector<TrackReference> newTracks;
  newTracks.reserve(prevTracks.size() + 1 + nextTracks.size());
  newTracks.insert(newTracks.end(), prevTracks.begin(), prevTracks.end());
  newTracks.push_back(current);
  newTracks.insert(newTracks.end(), nextTracks.begin(), nextTracks.end());

  currentTracks = std::move(newTracks);
  currentTracksIndex = (int16_t)prevTracks.size();
  // The app's set_queue list already reflects whatever queue-view it has -
  // pending insert bookkeeping restarts from it.
  pendingQueuedCount = 0;

  // Keep the playing head, requeue everything after it from the new list
  // (same keep-the-head pattern as updateTracks()'s non-initial branch).
  preloadedTracks.erase(preloadedTracks.begin() + 1, preloadedTracks.end());
  queueNextTrack(1);
  processSemaphore->give();

  CSPOT_LOG(info, "set_queue: %d prev + current + %d next",
            (int)prevTracks.size(), (int)nextTracks.size());
  return true;
}
