#pragma once

#include <atomic>              // for atomic
#include <condition_variable>  // for condition_variable
#include <cstdint>             // for uint8_t
#include <functional>          // for function
#include <memory>              // for shared_ptr
#include <mutex>               // for mutex
#include <string>              // for string
#include <utility>             // for pair
#include <vector>              // for vector

#include "BellTask.h"    // for Task
#include "ContextResolver.h"
#include "HTTPClient.h"  // for HTTPClient::Response
#include "PlaybackEvent.h"
#include "TrackQueue.h"
#include "protobuf/connectstate.pb.h"

typedef struct cJSON cJSON;

namespace cspot {
struct Context;
class Login5Client;
class TrackPlayer;
struct TrackReference;

// Publishes this device's connect-state to spclient (the outbound half of
// the modern protocol - docs/dealer_websocket_migration.md §5.3/§7), AND
// (Fase 6 "corte completo") owns the actual playback engine - TrackQueue/
// TrackPlayer, position tracking, player/command execution - replacing
// SpircHandler entirely rather than forwarding to it. Device registration
// (the first PutStateReason_NEW_DEVICE PUT that makes the device appear in
// the app) and player-state PUTs are bridged from this same engine's own
// events (see setEventHandler() below) instead of a separate class's.
//
// A bell::Task of its own (32KB, HTTPS-capable): updatePlayerState() is
// called from eventHandler() in cspot_connect.cpp, which per F62 can run
// on WHATEVER task triggered the SpircHandler event - including
// player_control_task, a 4KB worker sized only for cheap spirc calls, not
// for a blocking TLS PUT. Doing the PUT inline there overflowed that
// stack on real hardware (see the doc, "quinta vuelta"). So
// updatePlayerState() only records the latest pending state and returns
// immediately; this class's own task does the actual PUT, matching how
// DealerClient/Login5Client/TrackQueue already own their HTTPS calls
// instead of running them on a caller's stack.
class ConnectStateHandler : public bell::Task {
 public:
  ConnectStateHandler(std::shared_ptr<cspot::Context> ctx,
                      std::shared_ptr<cspot::Login5Client> login5);
  ~ConnectStateHandler();

  /**
  * @brief Stops the background publisher task.
  */
  void stop();

  /**
  * @brief Sets the x-spotify-connection-id that authorizes PUTs (from the
  * Dealer hello message, §6.4).
  */
  void setConnectionId(const std::string& connectionId);

  /**
  * @brief PUTs the current device state to spclient with the given reason.
  * Synchronous - only called from DealerClient's own (already
  * HTTPS-capable) task at registration time, never from an arbitrary
  * caller.
  * @param isActive Device.is_active - go-librespot only gets cluster
  * pushes once it's flipped this true (and keeps re-PUTting on every
  * player state change) - see docs/dealer_websocket_migration.md §5.3,
  * "primera vuelta" hardware finding. Defaults false (registration-only
  * PUTs, e.g. NEW_DEVICE).
  * @returns true on HTTP 200
  */
  bool putState(connectstate_PutStateReason reason, bool isActive = false);

  /**
  * @brief Tells spclient this device stopped being active (PUT
  * /connect-state/v1/devices/{id}/inactive, empty body, expects 204) -
  * the dedicated endpoint both reference implementations hit when
  * becoming inactive (go-librespot's PutConnectStateInactive() from
  * stopPlayback(), librespot-rust's became_inactive()), instead of a
  * regular state PUT. Without it the server keeps stale cluster state
  * for this device after another one takes over. Synchronous - only
  * called from DealerClient's task (cluster-update path).
  */
  bool putStateInactive();

  /**
  * @brief Records a real player-state snapshot and wakes the background
  * task to PUT it (is_active=true always - this IS the signal that the
  * device is genuinely playing something, per go-librespot's
  * State.setActive()). Non-blocking, safe to call from any task (see
  * class comment) - bridges SpircHandler's real playback events into
  * connect-state. Only the latest snapshot survives if called again
  * before the previous one is sent (same "only the latest state matters"
  * coalescing this project already uses elsewhere).
  * @param trackUri "spotify:track:..."/"spotify:episode:..." (see
  * TrackReference::encodeURI()), empty if unknown
  * @param reason defaults to PLAYER_STATE_CHANGED (every pre-existing
  * caller); handleSetVolume() passes VOLUME_CHANGED instead, matching
  * go-librespot's updateVolume()/volumeUpdated() - see docs/
  * dealer_websocket_migration_historial.md §55.
  */
  void updatePlayerState(
      bool isPlaying, const std::string& trackUri, uint32_t positionMs,
      uint32_t durationMs,
      connectstate_PutStateReason reason =
          connectstate_PutStateReason_PLAYER_STATE_CHANGED);

  /**
  * @brief Decodes a ClusterUpdate push (hm://connect-state/v1/cluster) and,
  * if we thought we were the active device and it now names a different
  * one, stops playback - the connect-state-native replacement for
  * SpircHandler's old "another player took control" reaction to a Mercury
  * Notify frame (Fase 6 "corte completo").
  */
  void handleClusterUpdate(const std::vector<uint8_t>& payload);

  /**
  * @brief Decodes a SetVolumeCommand push (hm://connect-state/v1/connect/
  * volume) and applies it - ctx->config.volume (read by buildDeviceInfo()
  * on the next PUT) plus an EventType::VOLUME engine event so whatever's
  * driving the audio sink (cspot_connect.cpp) picks it up the same way it
  * already does for SpircHandler's own VOLUME event. SetVolumeCommand.volume
  * is already in cspot's native 0..65535 scale - confirmed against
  * go-librespot's handling (player.MaxStateVolume), no conversion needed.
  */
  void handleSetVolume(const std::vector<uint8_t>& payload);

  /**
  * @brief Executes a real hm://connect-state/v1/player/command request
  * against this class's own engine (setPause/nextSong/previousSong/seekMs/
  * setRepeatContext/handleTransfer, below) - the "no coexistence" corte
  * completo replacement for the earlier CommandCallback-to-SpircHandler
  * forwarding (docs/dealer_websocket_migration.md, Fase 6). `command` is
  * the parsed "command" object (endpoint-specific fields like "position"/
  * "value"/"data"), borrowed - only valid for the duration of the call.
  * @returns whether it succeeded (drives the {"success":...} reply) -
  * false for any endpoint not yet implemented.
  */
  bool handlePlayerCommand(const std::string& endpoint, cJSON* command);

  /**
  * @brief Records which remote command was handled last (the request
  * envelope's message_id/sent_by_device_id) so every subsequent PUT can
  * carry PutStateRequest.last_command_message_id/
  * last_command_sent_by_device_id. This is how the requesting client
  * (e.g. the desktop app that sent a "transfer") correlates the cluster
  * state it gets back with the command IT sent - without these, the app
  * never sees its own transfer acknowledged and stays stuck on
  * "conectando..." forever. Both reference implementations set them for
  * EVERY handled command before executing it: go-librespot's
  * handlePlayerCommand() (`p.state.lastCommand = &req`, read back in
  * putConnectState()) and librespot-rust's handle_connect_state_request()
  * (`set_last_command()`, state.rs). Confirmed missing on real hardware
  * 2026-07-14 (docs/dealer_websocket_migration.md §10): two transfers
  * with distinct keys, both replied success=1 with the reply confirmed
  * sent, PUTs ok - and the app still never advanced past "conectando...".
  */
  void setLastCommand(uint32_t messageId, const std::string& sentByDeviceId);

  // --- Playback engine (docs/dealer_websocket_migration.md, Fase 6 "corte
  // completo"): ConnectStateHandler now owns its own TrackQueue/TrackPlayer
  // instead of forwarding to SpircHandler's. Position is tracked directly
  // here (positionMs/positionMeasuredAt/isPlayingState below) instead of
  // through a SPIRC Frame - same extrapolate-while-playing, freeze-while-
  // paused math PlaybackState used, just without the protobuf frame
  // wrapping it never needed for this. ---

  std::shared_ptr<TrackPlayer> getTrackPlayer() { return trackPlayer; }

  // Registers the callback for engine events (PLAY_PAUSE/TRACK_INFO/etc,
  // PlaybackEvent.h) - the ConnectStateHandler-driven replacement for
  // SpircHandler::setEventHandler(), same EventHandler type.
  void setEventHandler(EventHandler handler);

  // Loads a fresh track list (e.g. from a Transfer command's resolved
  // context) starting at `startIndex`/`positionMs`, replacing whatever was
  // queued before. Playing state becomes "paused" until trackLoadedCallback
  // (constructor, below) fires with the real one - same "Loading" gap
  // SpircHandler's Load-frame handling had, minus the SPIRC status enum.
  void loadTracks(const std::vector<TrackReference>& tracks, int startIndex,
                  uint32_t requestedPositionMs, bool startPaused);

  void setPause(bool pause);
  bool nextSong();
  bool previousSong();
  void seekMs(uint32_t position);
  void setRepeatContext(bool repeat);
  uint32_t getPositionMs();

  // Called from the audio pipeline (TrackPlayer's TrackReachedPlaybackCallback,
  // same as SpircHandler::notifyAudioReachedPlayback/Ended) to advance the
  // queue/report track info, and to reset to a clean stopped state once a
  // track finishes with nothing next.
  // @param trackId identifier of the QueuedTrack whose audio just started
  // producing PCM - used to catch
  // preloadedTracks' own head up to whatever's actually playing, however
  // many entries behind (a normal transition needs one catch-up skip; one
  // or more consecutive load failures ahead of this track, each already
  // skipped independently by TrackPlayer's own failure path, can leave more
  // than one stale/already-finished entry still sitting at the head - see
  // F89's own follow-up fix). Never sends a stale TRACK_INFO for the wrong
  // track: skips forward until the head matches trackId, or gives up
  // without sending anything if it's not found (queue genuinely exhausted).
  void notifyAudioReachedPlayback(const std::string& trackId);
  void notifyAudioEnded();

  void disconnect();

 protected:
  void runTask() override;

 private:
  // Decodes a "transfer" player/command's base64 TransferState (protobuf,
  // NOT JSON - unlike the context-resolve HTTP response ContextResolver
  // parses, see its own header comment), resolves the context it names via
  // contextResolver, and loads the result via loadTracks(). @returns
  // whether the transfer could be carried out at all (decode/resolve
  // failures only - "transferred to an empty context" isn't one, matching
  // go-librespot tolerating that case too).
  bool handleTransfer(cJSON* command);
  // Decodes a "play" player/command - unlike "transfer", this one carries
  // its context as plain JSON directly on the command object (command.
  // context.uri - protobuf's canonical JSON mapping, same shape
  // ContextResolver already parses from context-resolve HTTP responses),
  // not base64 protobuf. Fired when starting a NEW playback (e.g. tapping
  // a playlist/track in the app) as opposed to "transfer"'s "resume a
  // session already playing elsewhere". Confirmed on real hardware
  // (2026-07-14, docs/dealer_websocket_migration.md §10).
  bool handlePlay(cJSON* command);
  // Synchronous PUT (is_buffering=true, is_playing=true) sent from
  // handlePlay()/handleTransfer() BEFORE loadTracks() kicks off the async
  // key/CDN fetch, and therefore before DealerClient sends the {"success":
  // true} reply for the command. Matches go-librespot's loadCurrentTrack()
  // (daemon/controls.go), which calls updateState() with IsBuffering=true
  // as its first action, always ahead of the dealer reply (dealer/recv.go
  // only replies after the whole synchronous command handler returns). Our
  // own reply used to go out immediately while the real state PUT trailed
  // by ~2s behind the async TrackQueue pipeline - a real hardware capture
  // (2026-07-15, docs/dealer_websocket_migration.md §26) showed the Dealer
  // WebSocket closing 33ms after that early reply, well before any state
  // PUT had even been sent, on a connection that was NOT idle (ruling out
  // the idle-timeout theory of §22-§25). trackUri empty is tolerated
  // (matches putState()'s track-less shape).
  bool putBufferingState(const std::string& trackUri, uint32_t positionMs,
                         bool paused);
  bool skipTrack(TrackQueue::SkipDirection dir);
  // Folds elapsed time into positionMs when going from playing to paused
  // (so getPositionMs(), which only extrapolates while isPlayingState,
  // reflects "now" the instant it freezes) - mirrors
  // PlaybackState::setPlaybackState()'s Paused case. Caller holds no lock;
  // takes engineMutex itself.
  void setPlaybackPlaying(bool playing);
  void sendEngineEvent(EventType type);
  void sendEngineEvent(EventType type, EventData data);

  std::shared_ptr<cspot::Context> ctx;
  std::shared_ptr<cspot::Login5Client> login5;

  std::shared_ptr<cspot::TrackQueue> trackQueue;
  std::shared_ptr<cspot::TrackPlayer> trackPlayer;
  cspot::ContextResolver contextResolver;
  EventHandler engineEventHandler = nullptr;

  // Guards positionMs/positionMeasuredAt/isPlayingState only - not
  // TrackQueue/TrackPlayer's own internal state, which they protect
  // themselves.
  std::mutex engineMutex;
  uint32_t positionMs = 0;
  int64_t positionMeasuredAt = 0;
  bool isPlayingState = false;

  // Synced timestamp of the last real track load (set in the
  // trackLoadedCallback lambda, ConnectStateHandler.cpp's constructor) -
  // feeds PutStateRequest.has_been_playing_for_ms (sendPutStateRequest()).
  // Mirrors go-librespot's Player.startedPlaying (player/player.go, set on
  // every SetPrimaryStream call, paused or not) - a field this project's
  // trimmed connectstate.proto already has room for but never filled. 0
  // means "no track loaded yet this session", matching activeSinceMs's
  // same convention.
  int64_t currentTrackStartedAtMs = 0;

  // update_context's actual payload (§36) - restrictions/metadata for the
  // currently active context, applied only when the command's uri matches
  // (handlePlayerCommand()'s "update_context" case). Empty string means
  // "no restriction reported" (matches go-librespot's own "empty list"
  // convention, simplified to a single string - see the .proto comment).
  // Guarded by engineMutex, same as contextUri.
  std::string restrictionRepeatContext;
  std::string restrictionRepeatTrack;
  std::string restrictionShuffle;
  // Capped to what connectstate.options bounds
  // (PlayerState.context_metadata max_count:2) - extra entries from a real
  // command are simply dropped, not an error.
  std::vector<std::pair<std::string, std::string>> contextMetadata;

  // PlayerState.session_id/playback_id/context_uri - both reference
  // implementations always populate these (go's daemon/controls.go:
  // SessionId at transfer/activation, PlaybackId when a stream actually
  // opens, ContextUri from the resolved context); this class never did
  // until docs/dealer_websocket_migration.md §16. sessionId identifies the
  // current PLAYBACK session, not the device's lifetime: adopted from the
  // transfer's current_session.original_session_id, regenerated fresh on
  // every play-command context load (go-librespot's loadContext does
  // exactly this) - the constructor's initial random one only covers the
  // window before the first transfer/play (§24; §16's "generated once" was
  // a divergence from both references). playbackId is regenerated per real
  // track start (notifyAudioReachedPlayback()) - same granularity as go's
  // per-stream PlaybackId. contextUri is set whenever a transfer/play
  // names a resolvable context. All three guarded by engineMutex now that
  // sessionId is written after construction too.
  std::string sessionId;
  std::string playbackId;
  std::string contextUri;

  // sessionId rotation (§24): adopt the transferred session's id, or mint
  // a fresh random one when there is none (new context via play, or a
  // transfer that didn't carry one) - go-librespot's two behaviors.
  void adoptOrRegenerateSessionId(const char* transferredId);

  // Set whenever a PUT with is_active=true is sent (putState()/runTask()) -
  // read by handleClusterUpdate() to tell "someone else just took over"
  // apart from "we were never the active device to begin with".
  std::atomic<bool> isActiveDevice{false};
  // Epoch ms of when this device last became active - goes out as
  // PutStateRequest.started_playing_at on every PUT while active, same as
  // go-librespot's state.activeSince/librespot-rust's active_since. 0 =
  // not active.
  std::atomic<uint64_t> activeSinceMs{0};

  // See setLastCommand(). Guarded by lastCommandMutex: written from
  // DealerClient's task, read from both PUT paths (DealerClient's task
  // via putState(), this class's own task via runTask()).
  std::mutex lastCommandMutex;
  uint32_t lastCommandMessageId = 0;
  std::string lastCommandSentByDeviceId;

  // Shared PUT-sending machinery (connection-id/token checks, DeviceInfo,
  // encode, HTTP PUT, status handling) - putState()/the background
  // publisher only need to fill in put_state_reason/is_active/player_state
  // before calling this.
  bool sendPutStateRequest(connectstate_PutStateRequest& request);

  void buildDeviceInfo(connectstate_DeviceInfo& info);

  // Live PlayerState, mutated in place by putBufferingState()/runTask()'s
  // PUT builder instead of each rebuilding one from scratch - mirrors
  // go-librespot's single persistent State.player (player_state.go),
  // mutated field-by-field across calls and read whole at send time. Only
  // the mechanical playback fields live here (is_playing/is_paused/
  // is_buffering/playback_speed/timestamp/position_as_of_timestamp/
  // duration/track) - session_id/playback_id/context_uri/restrictions/
  // metadata stay their own members (as before), populated at send time,
  // since nothing here needs their persistence semantics changed.
  // Guarded by engineMutex. Reset to a fresh, minimal state (resetPlayerState())
  // on construction and whenever this device stops being active - go's own
  // two calls to State.reset() (initState(), stopPlayback()). Every writer
  // must set every field it owns unconditionally (no "only set if truthy"
  // - see has_track in putBufferingState()/runTask()): unlike the old
  // from-scratch builders, a field silently left untouched here now leaks
  // into every future PUT instead of just being absent from this one.
  connectstate_PlayerState playerState = connectstate_PlayerState_init_zero;

  // Resets playerState to go-librespot's State.reset() shape (is_system_
  // initiated/options/playback_speed set, everything else zero) - called
  // from the constructor and whenever this device stops being active.
  void resetPlayerState();

  std::mutex connectionIdMutex;
  std::string connectionId;

  uint32_t messageId = 0;

  // Cached spclient host + a kept-alive connection, reused across PUTs.
  // Without this, every single PUT (one per track/play/pause/command, not
  // just once per session like Login5) paid a fresh apresolve.spotify.com
  // lookup *and* a fresh TLS handshake - see
  // docs/dealer_websocket_migration.md, Fase 6 "eficiencia" finding.
  // putMutex serializes sendPutStateRequest() end-to-end (not just these
  // two fields): putState() is called synchronously from DealerClient's
  // own task (registration) while updatePlayerState()'s actual send runs
  // on this class's own task (runTask()) - two real tasks that can call
  // in here concurrently, and interleaving two requests on the same
  // shared Response object would corrupt the wire framing, not just race
  // the cached fields. Reset spclientHost/putConnection to force a fresh
  // resolve+reconnect only when a request genuinely fails (transport
  // exception), not on an ordinary non-200 (e.g. a 429) - same "don't
  // retry-storm a working connection over an application-level rejection"
  // principle as elsewhere in this project.
  std::mutex putMutex;
  std::string spclientHost;
  std::unique_ptr<bell::HTTPClient::Response> putConnection;

  // Set from a 429's Retry-After when sendPutStateRequest() hits one (see
  // HttpRetry.h's RateLimitedError) - runTask()'s PUT coalescing loop reads
  // this alongside PUT_MIN_INTERVAL_MS so a rate limit actually gets
  // respected instead of getting hammered again on the very next pending
  // update. std::atomic, not putMutex: runTask()'s coalescing wait happens
  // outside sendPutStateRequest()'s own lock, must not block on it.
  std::atomic<std::chrono::steady_clock::time_point> rateLimitedUntil{
      std::chrono::steady_clock::time_point{}};

  // Latest-wins pending player-state update, written by updatePlayerState()
  // (any task), consumed by runTask() (this class's own task).
  std::mutex pendingMutex;
  std::condition_variable pendingCv;
  bool hasPending = false;
  bool pendingIsPlaying = false;
  std::string pendingTrackUri;
  uint32_t pendingPositionMs = 0;
  uint32_t pendingDurationMs = 0;
  connectstate_PutStateReason pendingReason =
      connectstate_PutStateReason_PLAYER_STATE_CHANGED;

  // Held by runTask() for its whole lifetime; the destructor takes it
  // after stop(), so the object can't be freed under a still-running task
  // - same F93 pattern as MercurySession/DealerClient.
  std::mutex taskLifetimeMutex;

  std::atomic<bool> running{true};
};
}  // namespace cspot
