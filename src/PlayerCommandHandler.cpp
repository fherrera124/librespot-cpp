#include "PlayerCommandHandler.h"

#include <mutex>    // for lock_guard
#include <string>   // for string
#include <utility>  // for pair, move
#include <vector>   // for vector

// Always the real cJSON.h, not just CSpotContext.h's conditional
// (BELL_ONLY_CJSON-only) include - this file calls the raw cJSON C API
// unconditionally regardless of that flag, and bell always compiles
// cJSON.c in either way (nlohmann is only ever additive, see
// external/bell/CMakeLists.txt).
#include "cJSON.h"
#include "CSpotContext.h"
#include "Crypto.h"          // for Crypto::base64Decode
#include "PlayerStateModel.h"
#include "ContextResolver.h"
#include "Logger.h"          // for CSPOT_LOG
#include "NanoPBHelper.h"    // for pbDecode, pbArrayToVector
#include "PlaybackController.h"
#include "TrackReference.h"  // for TrackReference
#include "pb_decode.h"       // for pb_release

using namespace cspot;

namespace {
// pbPutString() has no bounds check of its own - server-controlled strings
// (update_context's restrictions/metadata, §36) must be truncated to the
// destination field's max_size-1 first, or they overflow into the next
// struct field (nanopb). maxLen = field's max_size - 1.
std::string truncateForPb(const std::string& s, size_t maxLen) {
  return s.size() > maxLen ? s.substr(0, maxLen) : s;
}

// Decodes a TransferState ContextTrack (uri + raw gid) into a TrackReference
// - the wire-protobuf twin of ContextResolver's JSON-shaped equivalent.
bool contextTrackToRef(const connectstate_ContextTrack& t,
                       cspot::TrackReference& out) {
  if (t.uri != nullptr) {
    out.uri = t.uri;
  }
  if (t.gid != nullptr) {
    out.gid = pbArrayToVector(t.gid);
  }
  if (out.gid.empty() && !out.uri.empty()) {
    out.decodeURI();
  }
  if (out.uri.find("episode:") != std::string::npos) {
    out.type = cspot::TrackReference::Type::EPISODE;
  }
  return !out.gid.empty();
}
}  // namespace

PlayerCommandHandler::PlayerCommandHandler(
    PlaybackController& playbackController, PlayerStateModel& stateModel,
    cspot::ContextResolver& contextResolver,
    PutBufferingStateCallback putBufferingState,
    UpdatePlayerStateCallback updatePlayerState,
    SendEngineEventCallback sendEngineEvent,
    SendEngineEventDataCallback sendEngineEventData)
    : playbackController(playbackController),
      stateModel(stateModel),
      contextResolver(contextResolver),
      putBufferingState(std::move(putBufferingState)),
      updatePlayerState(std::move(updatePlayerState)),
      sendEngineEvent(std::move(sendEngineEvent)),
      sendEngineEventData(std::move(sendEngineEventData)) {}

bool PlayerCommandHandler::handlePlayerCommand(const std::string& endpoint,
                                               cJSON* command) {
  if (endpoint == "transfer") {
    return handleTransfer(command);
  } else if (endpoint == "play") {
    return handlePlay(command);
  } else if (endpoint == "pause") {
    setPause(true);
    return true;
  } else if (endpoint == "resume") {
    setPause(false);
    return true;
  } else if (endpoint == "skip_next") {
    return nextSong();
  } else if (endpoint == "skip_prev") {
    // The client decides this, not us - a swipe gesture and a "hold
    // previous" press don't mean the same thing. Absent -> true (today's
    // restart-if-recent behavior); only an explicit false always goes to
    // the real previous track regardless of position. Matches
    // go-librespot's skipPrev(ctx, req.Command.Options.AllowSeeking).
    cJSON* optionsItem =
        command != nullptr ? cJSON_GetObjectItem(command, "options") : nullptr;
    cJSON* allowSeekingItem =
        optionsItem != nullptr
            ? cJSON_GetObjectItem(optionsItem, "allow_seeking")
            : nullptr;
    bool allowSeeking =
        allowSeekingItem == nullptr || cJSON_IsTrue(allowSeekingItem);
    return previousSong(allowSeeking);
  } else if (endpoint == "seek_to") {
    // "relative" is misleadingly named - only "current" is relative-to-now;
    // "beginning" and absent are both absolute, just different fields
    // ("position" vs "value"). See §10.
    cJSON* relativeItem =
        command != nullptr ? cJSON_GetObjectItem(command, "relative")
                           : nullptr;
    std::string relative = (relativeItem != nullptr &&
                            relativeItem->valuestring != nullptr)
                               ? relativeItem->valuestring
                               : "";

    cJSON* positionItem =
        command != nullptr ? cJSON_GetObjectItem(command, "position")
                           : nullptr;
    cJSON* valueItem =
        command != nullptr ? cJSON_GetObjectItem(command, "value") : nullptr;

    uint32_t position = 0;
    bool ok = false;
    if (relative == "current" && positionItem != nullptr &&
        cJSON_IsNumber(positionItem)) {
      position = playbackController.getPositionMs() +
                (uint32_t)positionItem->valuedouble;
      ok = true;
    } else if (relative == "beginning" && positionItem != nullptr &&
              cJSON_IsNumber(positionItem)) {
      position = (uint32_t)positionItem->valuedouble;
      ok = true;
    } else if (relative.empty() && valueItem != nullptr &&
              cJSON_IsNumber(valueItem)) {
      position = (uint32_t)valueItem->valuedouble;
      ok = true;
    }

    if (!ok) {
      CSPOT_LOG(error,
               "player/command seek_to: unsupported shape (relative='%s', "
               "has position=%d, has value=%d)",
               relative.c_str(), positionItem != nullptr,
               valueItem != nullptr);
      return false;
    }
    seekMs(position);
    return true;
  } else if (endpoint == "set_repeating_context") {
    cJSON* valueItem =
        command != nullptr ? cJSON_GetObjectItem(command, "value") : nullptr;
    if (valueItem == nullptr) {
      return false;
    }
    setRepeatContext(cJSON_IsTrue(valueItem));
    return true;
  } else if (endpoint == "update_context") {
    // Context metadata/restrictions sync, not a new queue/context (that's
    // transfer/play). Always acks, even on a uri mismatch (skip applying),
    // matching both reference clients. See §32/§36.
    cJSON* contextItem =
        command != nullptr ? cJSON_GetObjectItem(command, "context") : nullptr;
    cJSON* uriItem =
        contextItem != nullptr ? cJSON_GetObjectItem(contextItem, "uri")
                               : nullptr;
    std::string incomingUri = (uriItem != nullptr && uriItem->valuestring != nullptr)
                                  ? uriItem->valuestring
                                  : "";

    std::string currentUri = stateModel.contextUri();
    auto snapshot = currentPlaybackSnapshot();

    if (incomingUri.empty() || incomingUri != currentUri) {
      CSPOT_LOG(info,
               "update_context: ignoring context update for wrong uri: %s",
               incomingUri.c_str());
      return true;
    }

    // Same trimming as connectstate.proto's Restrictions (§36): only the 3
    // reasons go-librespot reads, one string each, truncated to fit.
    cJSON* restrictionsItem =
        cJSON_GetObjectItem(contextItem, "restrictions");
    std::string repeatContextReason, repeatTrackReason, shuffleReason;
    if (restrictionsItem != nullptr) {
      auto firstReasonOf = [&](const char* key) -> std::string {
        cJSON* arr = cJSON_GetObjectItem(restrictionsItem, key);
        cJSON* first = cJSON_GetArrayItem(arr, 0);
        return (first != nullptr && first->valuestring != nullptr)
                   ? truncateForPb(first->valuestring, 47)
                   : "";
      };
      repeatContextReason =
          firstReasonOf("disallow_toggling_repeat_context_reasons");
      repeatTrackReason =
          firstReasonOf("disallow_toggling_repeat_track_reasons");
      shuffleReason = firstReasonOf("disallow_toggling_shuffle_reasons");
    }

    cJSON* metadataItem = cJSON_GetObjectItem(contextItem, "metadata");
    std::vector<std::pair<std::string, std::string>> metadata;
    if (metadataItem != nullptr) {
      cJSON* entry = nullptr;
      cJSON_ArrayForEach(entry, metadataItem) {
        // connectstate.options caps context_metadata at 2 entries -
        // extras from a real command are simply dropped, not an error.
        if (metadata.size() >= 2) {
          break;
        }
        if (entry->string != nullptr && entry->valuestring != nullptr) {
          // key/value max_size is 32 each - truncate both (see
          // truncateForPb()'s comment above).
          metadata.emplace_back(truncateForPb(entry->string, 31),
                                truncateForPb(entry->valuestring, 31));
        }
      }
    }

    stateModel.setRestrictions(repeatContextReason, repeatTrackReason,
                               shuffleReason);
    stateModel.setContextMetadata(std::move(metadata));

    updatePlayerState(snapshot.isPlaying, snapshot.trackUri,
                      snapshot.positionMs, snapshot.durationMs,
                      connectstate_PutStateReason_PLAYER_STATE_CHANGED,
                      /*isBuffering=*/false);
    return true;
  } else if (endpoint == "add_to_queue") {
    // command.track is one ContextTrack, same canonical-JSON shape
    // ContextResolver parses from context-resolve responses.
    cJSON* trackItem =
        command != nullptr ? cJSON_GetObjectItem(command, "track") : nullptr;
    TrackReference ref;
    if (trackItem == nullptr || !ContextResolver::trackFromJson(trackItem, ref)) {
      CSPOT_LOG(error, "add_to_queue: no usable track in command");
      return false;
    }
    if (!playbackController.getTrackQueue()->insertNext(ref)) {
      // Nothing playing to queue behind - go-librespot warns and still
      // replies success here (nil-tracks branch of its addToQueue), so
      // mirror that instead of making the app error out.
      CSPOT_LOG(info, "add_to_queue: nothing playing, ignored");
    }
    return true;
  } else if (endpoint == "set_queue") {
    // command.prev_tracks/next_tracks: the app's full picture of what
    // surrounds the current track (its queue edits included) - replace
    // ours with it, keeping the playing track untouched.
    std::vector<TrackReference> prevTracks, nextTracks;
    cJSON* item = nullptr;
    cJSON* prevArray =
        command != nullptr ? cJSON_GetObjectItem(command, "prev_tracks")
                           : nullptr;
    cJSON_ArrayForEach(item, prevArray) {
      TrackReference ref;
      if (ContextResolver::trackFromJson(item, ref)) {
        prevTracks.push_back(std::move(ref));
      }
    }
    cJSON* nextArray =
        command != nullptr ? cJSON_GetObjectItem(command, "next_tracks")
                           : nullptr;
    cJSON_ArrayForEach(item, nextArray) {
      TrackReference ref;
      if (ContextResolver::trackFromJson(item, ref)) {
        nextTracks.push_back(std::move(ref));
      }
    }
    if (!playbackController.getTrackQueue()->replaceUpcoming(prevTracks,
                                                              nextTracks)) {
      CSPOT_LOG(info, "set_queue: nothing playing, ignored");
    }
    return true;
  }

  CSPOT_LOG(info,
           "player/command endpoint '%s' not implemented (needs a new "
           "queue/context - deferred, see Fase 6 MVP scope note)",
           endpoint.c_str());
  return false;
}

void PlayerCommandHandler::loadTracks(const std::vector<TrackReference>& tracks,
                                      int startIndex,
                                      uint32_t requestedPositionMs,
                                      bool startPaused) {
  playbackController.loadTracks(tracks, startIndex, requestedPositionMs,
                                startPaused);
}

PlayerCommandHandler::PlaybackSnapshot
PlayerCommandHandler::currentPlaybackSnapshot() const {
  return {stateModel.trackUri(), stateModel.duration(),
          playbackController.isPlaying(), playbackController.getPositionMs()};
}

bool PlayerCommandHandler::handleTransfer(cJSON* command) {
  cJSON* dataItem =
      command != nullptr ? cJSON_GetObjectItem(command, "data") : nullptr;
  std::vector<uint8_t> raw;
  if (dataItem != nullptr && dataItem->valuestring != nullptr) {
    raw = Crypto::base64Decode(dataItem->valuestring);
  }
  // Check decoded byte length, not just JSON field presence - a "data":""
  // slips past a nullptr-only check into pb_decode() on a zero-byte buffer,
  // which trivially "succeeds" with defaults. See §10.
  if (raw.empty()) {
    CSPOT_LOG(info, "transfer: no data, becoming active with an empty "
                    "queue");
    // is_active only reaches spclient via runTask()'s PUT, which needs a
    // real playback event to fire - force one here. See §10.
    updatePlayerState(false, "", 0, 0,
                      connectstate_PutStateReason_PLAYER_STATE_CHANGED,
                      /*isBuffering=*/false);
    return true;
  }

  // Dump raw wire bytes: TransferState shapes this schema doesn't parse
  // cleanly are best debugged from the actual varint tags. See §10.
  {
    std::string hex;
    hex.reserve(raw.size() * 2);
    static const char* hexDigits = "0123456789abcdef";
    for (uint8_t b : raw) {
      hex += hexDigits[b >> 4];
      hex += hexDigits[b & 0x0f];
    }
    CSPOT_LOG(info, "transfer: raw TransferState bytes (%d): %s",
             (int)raw.size(), hex.c_str());
  }

  connectstate_TransferState transferState =
      connectstate_TransferState_init_zero;
  pbDecode(transferState, connectstate_TransferState_fields, raw);

  CSPOT_LOG(debug,
           "transfer: decoded %d bytes, has_session=%d has_context=%d "
           "context_uri='%s' has_queue=%d queue_tracks=%d",
           (int)raw.size(), (int)transferState.has_current_session,
           (int)(transferState.has_current_session &&
                 transferState.current_session.has_context),
           (transferState.has_current_session &&
            transferState.current_session.has_context &&
            transferState.current_session.context.uri != nullptr)
               ? transferState.current_session.context.uri
               : "?",
           (int)transferState.has_queue,
           transferState.has_queue ? (int)transferState.queue.tracks_count : 0);

  // Adopt the transferred session id (or mint a new one), before any
  // branch below and before pb_release (the string is freed with the
  // message). See §24.
  stateModel.adoptOrRegenerateSessionId(
      transferState.has_current_session
          ? transferState.current_session.original_session_id
          : nullptr);

  // context.uri can be a real pointer to an EMPTY string, not just absent -
  // that shape means "ad-hoc queue, no context" (search results, radio):
  // the track list is in queue.tracks instead, already resolved. See §10.
  bool haveContext = transferState.has_current_session &&
                     transferState.current_session.has_context &&
                     transferState.current_session.context.uri != nullptr &&
                     transferState.current_session.context.uri[0] != '\0';

  std::vector<TrackReference> tracks;
  if (haveContext) {
    std::string resolvedContextUri = transferState.current_session.context.uri;
    if (!contextResolver.resolve(resolvedContextUri, tracks)) {
      CSPOT_LOG(error, "transfer: failed resolving context %s",
               resolvedContextUri.c_str());
      pb_release(connectstate_TransferState_fields, &transferState);
      return false;
    }
    // PlayerState.context_uri.
    stateModel.setContextUri(resolvedContextUri);
  } else if (transferState.has_queue && transferState.queue.tracks_count > 0) {
    for (pb_size_t i = 0; i < transferState.queue.tracks_count; i++) {
      TrackReference ref;
      if (contextTrackToRef(transferState.queue.tracks[i], ref)) {
        tracks.push_back(std::move(ref));
      }
    }
    if (tracks.empty()) {
      CSPOT_LOG(error, "transfer: queue had no usable tracks");
      pb_release(connectstate_TransferState_fields, &transferState);
      return false;
    }
  } else if (transferState.has_playback &&
             transferState.playback.has_current_track) {
    // Single-track transfer: no context/queue, but playback.current_track
    // is itself a real track - load just that. No context to resume
    // "next" from. See §15.
    TrackReference ref;
    if (!contextTrackToRef(transferState.playback.current_track, ref)) {
      CSPOT_LOG(error, "transfer: current_track has no usable gid/uri");
      pb_release(connectstate_TransferState_fields, &transferState);
      return false;
    }
    tracks.push_back(std::move(ref));
  } else {
    // Not an error: a genuinely empty TransferState means "nothing to hand
    // off yet" (device selected while nothing plays anywhere) - same
    // outcome as the raw.empty() case above. See §10.
    CSPOT_LOG(info, "transfer: no context/queue tracks in TransferState, "
                    "becoming active with an empty queue");
    pb_release(connectstate_TransferState_fields, &transferState);
    updatePlayerState(false, "", 0, 0,
                      connectstate_PutStateReason_PLAYER_STATE_CHANGED,
                      /*isBuffering=*/false);
    return true;
  }

  int startIndex = 0;
  bool isPaused = transferState.has_playback && transferState.playback.is_paused;
  uint32_t requestedPositionMs =
      transferState.has_playback
          ? (uint32_t)transferState.playback.position_as_of_timestamp
          : 0;

  // Find where to start in the resolved list, matching by uri.
  if (transferState.has_playback && transferState.playback.has_current_track &&
      transferState.playback.current_track.uri != nullptr) {
    std::string currentTrackUri = transferState.playback.current_track.uri;
    for (size_t i = 0; i < tracks.size(); i++) {
      if (tracks[i].uri == currentTrackUri) {
        startIndex = (int)i;
        break;
      }
    }
  }

  pb_release(connectstate_TransferState_fields, &transferState);

  // Synchronous, before loadTracks() kicks off the async key/CDN fetch and
  // before DealerClient sends this request's WS reply - see
  // putBufferingState()'s header comment / §26.
  std::string firstTrackUri = (startIndex >= 0 && startIndex < (int)tracks.size())
                                  ? tracks[startIndex].uri
                                  : "";
  putBufferingState(firstTrackUri, requestedPositionMs, isPaused);

  loadTracks(tracks, startIndex, requestedPositionMs, isPaused);
  return true;
}

bool PlayerCommandHandler::handlePlay(cJSON* command) {
  cJSON* contextItem =
      command != nullptr ? cJSON_GetObjectItem(command, "context") : nullptr;
  cJSON* uriItem =
      contextItem != nullptr ? cJSON_GetObjectItem(contextItem, "uri")
                             : nullptr;
  if (uriItem == nullptr || uriItem->valuestring == nullptr ||
      uriItem->valuestring[0] == '\0') {
    CSPOT_LOG(error, "play: command has no context uri");
    return false;
  }

  std::string resolvedContextUri = uriItem->valuestring;
  std::vector<TrackReference> tracks;
  if (!contextResolver.resolve(resolvedContextUri, tracks)) {
    CSPOT_LOG(error, "play: failed resolving context %s",
             resolvedContextUri.c_str());
    return false;
  }
  // PlayerState.context_uri.
  stateModel.setContextUri(resolvedContextUri);

  // command.options.skip_to.{track_uri,track_index} - track_uid not
  // matched (TrackReference has no uid field, only uri/gid).
  int startIndex = 0;
  bool startPaused = false;
  cJSON* optionsItem =
      command != nullptr ? cJSON_GetObjectItem(command, "options") : nullptr;
  if (optionsItem != nullptr) {
    cJSON* initiallyPausedItem =
        cJSON_GetObjectItem(optionsItem, "initially_paused");
    startPaused = cJSON_IsTrue(initiallyPausedItem);

    cJSON* skipToItem = cJSON_GetObjectItem(optionsItem, "skip_to");
    if (skipToItem != nullptr) {
      cJSON* trackUriItem = cJSON_GetObjectItem(skipToItem, "track_uri");
      if (trackUriItem != nullptr && trackUriItem->valuestring != nullptr &&
          trackUriItem->valuestring[0] != '\0') {
        std::string skipToUri = trackUriItem->valuestring;
        for (size_t i = 0; i < tracks.size(); i++) {
          if (tracks[i].uri == skipToUri) {
            startIndex = (int)i;
            break;
          }
        }
      } else {
        cJSON* trackIndexItem = cJSON_GetObjectItem(skipToItem, "track_index");
        if (trackIndexItem != nullptr && cJSON_IsNumber(trackIndexItem) &&
            trackIndexItem->valueint > 0 &&
            trackIndexItem->valueint < (int)tracks.size()) {
          startIndex = trackIndexItem->valueint;
        }
      }
    }
  }

  // A new context starts a new playback session - regenerate the session
  // id, like go-librespot. See §24.
  stateModel.adoptOrRegenerateSessionId(nullptr);

  // Synchronous, before loadTracks() kicks off the async key/CDN fetch and
  // before DealerClient sends this request's WS reply - see
  // putBufferingState()'s header comment / §26.
  std::string firstTrackUri = (startIndex >= 0 && startIndex < (int)tracks.size())
                                  ? tracks[startIndex].uri
                                  : "";
  putBufferingState(firstTrackUri, 0, startPaused);

  loadTracks(tracks, startIndex, 0, startPaused);
  return true;
}

void PlayerCommandHandler::setPause(bool pause) {
  playbackController.setPlaybackPlaying(!pause);
  sendEngineEventData(EventType::PLAY_PAUSE, pause);

  // Sent here (F105, docs/spotify_component_analysis.md), not left to the
  // app layer: this is the single entry point for both a remote
  // player/command and a local hardware button, so it's the one place
  // that always has the real, current trackUri/duration - no separate
  // cache to go stale.
  auto snapshot = currentPlaybackSnapshot();
  if (!snapshot.trackUri.empty()) {
    updatePlayerState(!pause, snapshot.trackUri, snapshot.positionMs,
                      snapshot.durationMs,
                      connectstate_PutStateReason_PLAYER_STATE_CHANGED,
                      /*isBuffering=*/false);
  }
}

bool PlayerCommandHandler::nextSong() {
  return playbackController.nextSong();
}

bool PlayerCommandHandler::previousSong(bool allowSeeking) {
  return playbackController.previousSong(allowSeeking);
}

void PlayerCommandHandler::seekMs(uint32_t position) {
  playbackController.seekMs(position);

  auto snapshot = currentPlaybackSnapshot();
  sendEngineEventData(EventType::SEEK, (int)position);

  // A seek while playing (not paused) otherwise never reaches the app at
  // all - EventType::SEEK isn't handled in cspot_connect.cpp's
  // eventHandler(), and pause/resume are the only other callers of
  // updatePlayerState(). The device applies the seek and keeps playing
  // fine locally, but the app's own clock/position bar was left frozen at
  // wherever it last heard from us, since it never got told. Matches
  // go-librespot's seek() (controls.go), which always calls updateState()
  // right after SeekMs() too.
  updatePlayerState(snapshot.isPlaying, snapshot.trackUri, position,
                    snapshot.durationMs,
                    connectstate_PutStateReason_PLAYER_STATE_CHANGED,
                    /*isBuffering=*/false);
}

void PlayerCommandHandler::setRepeatContext(bool repeat) {
  playbackController.setRepeatContext(repeat);
  sendEngineEventData(EventType::REPEAT_CONTEXT, repeat);
}
