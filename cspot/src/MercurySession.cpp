#include "MercurySession.h"

#include <string.h>     // for memcpy
#include <memory>       // for shared_ptr
#include <mutex>        // for scoped_lock
#include <stdexcept>    // for runtime_error
#include <type_traits>  // for remove_extent_t, __underlying_type_impl<>:...
#include <utility>      // for pair
#ifndef _WIN32
#include <arpa/inet.h>  // for htons, ntohs, htonl, ntohl
#endif
#include "BellLogger.h"         // for AbstractLogger
#include "BellTask.h"           // for Task
#include "BellUtils.h"          // for BELL_SLEEP_MS
#include "Logger.h"             // for CSPOT_LOG
#include "NanoPBHelper.h"       // for pbPutString, pbDecode, pbEncode
#include "PlainConnection.h"    // for PlainConnection
#include "ShannonConnection.h"  // for ShannonConnection
#include "TimeProvider.h"       // for TimeProvider
#include "Utils.h"              // for extract, pack, hton64

using namespace cspot;

// 4KB (the original size here) is enough for the steady-state receive loop,
// but not for reconnect() (called on any read/write error - see
// MercurySession.cpp runTask()): it re-runs connectWithRandomAp()
// (ApResolve.cpp - a full HTTPS GET via bell::HTTPClient, i.e. a TLS
// handshake, plus a nlohmann::json::parse() of the response, both
// non-trivial stack users on their own) followed by authenticate()
// (Session.cpp), which stack-allocates an `APWelcome welcome;` nanopb
// struct (~1KB, thanks to its embedded reusable_auth_credentials/
// lfs_secret/canonical_username fields - see protobuf/authentication.options)
// - all layered on top of runTask()/reconnect()'s own frames. Found on real
// hardware: a transient read error mid-session triggered exactly this path
// and overflowed the 4KB stack (task "mercury_dispatch" in the FreeRTOS
// stack-overflow panic, truncated to 16 chars). Bumped well above what the
// reconnect path needs - this runs on PSRAM (bell::Task's default), which
// is plentiful (sibling tasks CSpotTrackQueue/cspot_player already use
// 32KB/48KB), so there's no reason to keep this one tight.
MercurySession::MercurySession(std::shared_ptr<TimeProvider> timeProvider)
    : bell::Task("mercury_dispatcher", 16 * 1024, 3, 1) {
  this->timeProvider = timeProvider;
}

MercurySession::~MercurySession() {
  std::scoped_lock lock(this->isRunningMutex);
}

void MercurySession::runTask() {
  isRunning = true;
  std::scoped_lock lock(this->isRunningMutex);

  while (isRunning) {
    cspot::Packet packet = {};
    try {
      // getShanConn(), not the bare member: reconnect() (this same task,
      // but only sequentially between loop iterations) and any other
      // task's execute()/requestAudioKey() must never see a torn read of
      // this pointer. See F93.
      auto conn = getShanConn();
      if (conn == nullptr) {
        throw std::runtime_error("not connected");
      }
      packet = conn->recvPacket();
      CSPOT_LOG(info, "Received packet, command: %d", packet.command);

      if (static_cast<RequestType>(packet.command) == RequestType::PING) {
        timeProvider->syncWithPingPacket(packet.data);

        this->lastPingTimestamp = timeProvider->getSyncedTimestamp();
        conn->sendPacket(0x49, packet.data);
      } else {
        this->packetQueue.push(packet);
      }
    } catch (const std::runtime_error& e) {
      CSPOT_LOG(error, "Error while receiving packet: %s", e.what());
      failAllPending();

      if (!isRunning)
        return;

      reconnect();
      continue;
    }
  }
}

void MercurySession::reconnect() {
  isReconnecting = true;

  // Was: `return reconnect();` inside the catch block below on every
  // failed attempt - self-recursive, not guaranteed tail-call-optimized
  // (it's inside a try/catch, which typically prevents that), so a long
  // enough network outage (retrying every 5s) could keep growing the
  // stack until it overflowed again, even after the stack size bump for
  // finding F19. A `while` loop does the same retry-forever-until-success
  // behavior with constant stack usage. See
  // docs/spotify_component_analysis.md, finding F32.
  while (true) {
    try {
      {
        // Briefly, not for the whole reconnect attempt: any other task
        // mid-way through getShanConn() must see either the old
        // connection or nullptr, never a half-destroyed one. See F93.
        std::scoped_lock lock(shanConnMutex);
        this->conn = nullptr;
        this->shanConn = nullptr;
      }

      this->connectWithRandomAp();
      this->authenticate(this->authBlob);

      CSPOT_LOG(info, "Reconnection successful");

      BELL_SLEEP_MS(100);

      lastPingTimestamp = timeProvider->getSyncedTimestamp();
      isReconnecting = false;
      return;
    } catch (...) {
      CSPOT_LOG(error, "Cannot reconnect, will retry in 5s");
      BELL_SLEEP_MS(5000);

      if (!isRunning) {
        return;
      }
    }
  }
}

bool MercurySession::triggerTimeout() {
  if (!isRunning)
    return true;
  auto currentTimestamp = timeProvider->getSyncedTimestamp();

  if (currentTimestamp - this->lastPingTimestamp > PING_TIMEOUT_MS) {
    CSPOT_LOG(debug, "Reconnection required, no ping received");
    return true;
  }

  return false;
}

void MercurySession::unregister(uint64_t sequenceId) {
  std::scoped_lock lock(sessionMutex);
  auto callback = this->callbacks.find(sequenceId);

  if (callback != this->callbacks.end()) {
    this->callbacks.erase(callback);
  }
}

void MercurySession::unregisterAudioKey(uint32_t sequenceId) {
  std::scoped_lock lock(sessionMutex);
  auto callback = this->audioKeyCallbacks.find(sequenceId);

  if (callback != this->audioKeyCallbacks.end()) {
    this->audioKeyCallbacks.erase(callback);
  }
}

void MercurySession::disconnect() {
  CSPOT_LOG(info, "Disconnecting mercury session");
  this->isRunning = false;
  {
    // conn can be null here if a reconnect() is torn down mid-attempt -
    // was an unguarded null deref before. See F93.
    std::scoped_lock lock(shanConnMutex);
    if (conn) conn->close();
  }
  std::scoped_lock lock(this->isRunningMutex);
}

std::string MercurySession::getCountryCode() {
  std::scoped_lock lock(sessionMutex);
  return this->countryCode;
}

void MercurySession::handlePacket() {
  Packet packet = {};

  this->packetQueue.wtpop(packet, 200);

  // Every branch below only locks sessionMutex long enough to read/erase
  // the map entry - the actual callback runs unlocked, so a callback that
  // takes a while (or, in principle, calls back into execute()) can never
  // block other tasks or deadlock against this same mutex. See F93.
  switch (static_cast<RequestType>(packet.command)) {
    case RequestType::COUNTRY_CODE_RESPONSE: {
      std::scoped_lock lock(sessionMutex);
      this->countryCode = std::string();
      this->countryCode.resize(2);
      memcpy(this->countryCode.data(), packet.data.data(), 2);
      CSPOT_LOG(debug, "Received country code %s", this->countryCode.c_str());
      break;
    }
    case RequestType::AUDIO_KEY_FAILURE_RESPONSE:
    case RequestType::AUDIO_KEY_SUCCESS_RESPONSE: {
      // First four bytes mark the sequence id
      auto seqId = ntohl(extract<uint32_t>(packet.data, 0));

      AudioKeyCallback callback;
      {
        std::scoped_lock lock(sessionMutex);
        auto it = this->audioKeyCallbacks.find(seqId);
        if (it != this->audioKeyCallbacks.end()) {
          callback = it->second;
        }
      }
      if (callback) {
        auto success = static_cast<RequestType>(packet.command) ==
                       RequestType::AUDIO_KEY_SUCCESS_RESPONSE;
        callback(success, packet.data);
      }

      break;
    }
    case RequestType::SEND: {
      CSPOT_LOG(debug, "Received mercury packet");

      auto response = this->decodeResponse(packet.data);
      ResponseCallback callback;
      {
        std::scoped_lock lock(sessionMutex);
        auto it = this->callbacks.find(response.sequenceId);
        if (it != this->callbacks.end()) {
          callback = it->second;
          this->callbacks.erase(it);
        }
      }
      if (callback) {
        callback(response);
      }
      break;
    }
    default:
      break;
  }
}

void MercurySession::failAllPending() {
  Response response = {};
  response.fail = true;

  // Move the map out under the lock (fast), then invoke the callbacks
  // unlocked below - execute()/requestAudioKey() insert into this same
  // map from any task, so iterating+clearing it in place without a lock
  // is a real concurrent-modification hazard, not just a theoretical one
  // (see F93 for the crash this whole file is hardening against).
  std::unordered_map<uint64_t, ResponseCallback> failedCallbacks;
  // audioKeyCallbacks used to be left untouched here - a requestAudioKey()
  // in flight when the AP connection drops (recvPacket() error, right
  // above this call) never got its callback invoked at all, success or
  // failure. The caller (TrackQueue.cpp's stepLoadAudioFile()) then just
  // sat on QueuedTrack::loadedSemaphore for the full 5s wait
  // (TrackPlayer.cpp) before giving up on its own timeout, instead of
  // failing immediately once the connection is known to be dead. See
  // docs/dealer_websocket_migration.md §30.
  std::unordered_map<uint32_t, AudioKeyCallback> failedAudioKeyCallbacks;
  {
    std::scoped_lock lock(sessionMutex);
    failedCallbacks = std::move(this->callbacks);
    this->callbacks.clear();
    failedAudioKeyCallbacks = std::move(this->audioKeyCallbacks);
    this->audioKeyCallbacks.clear();
  }

  for (auto& it : failedCallbacks) {
    it.second(response);
  }
  for (auto& it : failedAudioKeyCallbacks) {
    it.second(false, {});
  }
}

MercurySession::Response MercurySession::decodeResponse(
    const std::vector<uint8_t>& data) {
  Response response = {};
  response.parts = {};

  response.sequenceId = hton64(extract<uint64_t>(data, 2));

  auto headerSize = ntohs(extract<uint16_t>(data, 13));
  auto headerBytes =
      std::vector<uint8_t>(data.begin() + 15, data.begin() + 15 + headerSize);

  auto pos = 15 + headerSize;
  while (pos < data.size()) {
    auto partSize = ntohs(extract<uint16_t>(data, pos));

    response.parts.push_back(std::vector<uint8_t>(
        data.begin() + pos + 2, data.begin() + pos + 2 + partSize));
    pos += 2 + partSize;
  }

  pbDecode(response.mercuryHeader, Header_fields, headerBytes);
  response.fail = false;

  return response;
}

uint64_t MercurySession::execute(RequestType method, const std::string& uri,
                                 ResponseCallback callback,
                                 DataParts& payload) {
  CSPOT_LOG(debug, "Executing Mercury Request, type %s",
            RequestTypeMap[method].c_str());

  // Header's method field must carry the *original* text ("GET", not
  // "SEND") - the server reads this to decide whether to actually return
  // data. The GET->SEND aliasing below is only for the outer packet-type
  // byte (wire-identical per the comment on it), a completely separate
  // thing. Capturing this before the aliasing matters: doing it after
  // silently turned every metadata GET into a wire-identical-looking SEND,
  // and Spotify's servers responded with an empty body instead of the
  // real metadata - a regression from the F93 rewrite, only caught after
  // real hardware testing showed every track failing to load with
  // "res.parts.size() == 0". See F96.
  std::string headerMethodStr = RequestTypeMap[method];

  // GET and SEND are actually the same. Therefore the override
  // The difference between them is only in header's method
  if (method == RequestType::GET) {
    method = RequestType::SEND;
  }

  std::vector<uint8_t> sequenceIdBytes;
  uint64_t assignedSequenceId;
  {
    // Covers tempMercuryHeader/callbacks/sequenceId - all mutable state
    // shared with handlePacket() (this session's own task) and with every
    // other task that can call execute() concurrently. Released before
    // the actual send below - building these few hundred bytes is fast,
    // no need to hold the lock across network I/O. See F93.
    std::scoped_lock lock(sessionMutex);

    // Encode header
    pbPutString(uri, tempMercuryHeader.uri);
    pbPutString(headerMethodStr, tempMercuryHeader.method);

    tempMercuryHeader.has_method = true;
    tempMercuryHeader.has_uri = true;

    auto headerBytes = pbEncode(Header_fields, &tempMercuryHeader);

    assignedSequenceId = this->sequenceId;
    this->callbacks.insert({assignedSequenceId, callback});

    // Structure: [Sequence size] [SequenceId] [0x1] [Payloads number]
    // [Header size] [Header] [Payloads (size + data)]

    // Pack sequenceId
    sequenceIdBytes = pack<uint64_t>(hton64(assignedSequenceId));
    auto sequenceSizeBytes = pack<uint16_t>(htons(sequenceIdBytes.size()));

    sequenceIdBytes.insert(sequenceIdBytes.begin(), sequenceSizeBytes.begin(),
                           sequenceSizeBytes.end());
    sequenceIdBytes.push_back(0x01);

    auto payloadNum = pack<uint16_t>(htons(payload.size() + 1));
    sequenceIdBytes.insert(sequenceIdBytes.end(), payloadNum.begin(),
                           payloadNum.end());

    auto headerSizePayload = pack<uint16_t>(htons(headerBytes.size()));
    sequenceIdBytes.insert(sequenceIdBytes.end(), headerSizePayload.begin(),
                           headerSizePayload.end());
    sequenceIdBytes.insert(sequenceIdBytes.end(), headerBytes.begin(),
                           headerBytes.end());

    // Encode all the payload parts
    for (int x = 0; x < payload.size(); x++) {
      headerSizePayload = pack<uint16_t>(htons(payload[x].size()));
      sequenceIdBytes.insert(sequenceIdBytes.end(), headerSizePayload.begin(),
                             headerSizePayload.end());
      sequenceIdBytes.insert(sequenceIdBytes.end(), payload[x].begin(),
                             payload[x].end());
    }

    // Bump sequence id
    this->sequenceId += 1;
  }

  auto conn = getShanConn();
  if (conn == nullptr) {
    CSPOT_LOG(error, "Failed to send Mercury packet for %s: not connected",
              uri.c_str());
    return assignedSequenceId;
  }

  try {
    conn->sendPacket(
        static_cast<std::underlying_type<RequestType>::type>(method),
        sequenceIdBytes);
  } catch (const std::exception& e) {
    // Previously silent (see F40) - the registered callback/subscription
    // above is left pending regardless, since resolving it here would need
    // trackListMutex/tracksMutex, already held by callers like
    // TrackQueue::processTrack() -> deadlock. Relies on the caller's own
    // timeout (e.g. TrackPlayer's loadedSemaphore->twait) or a later
    // recvPacket() failure's failAllPending() to eventually recover.
    CSPOT_LOG(error, "Failed to send Mercury packet for %s: %s",
              uri.c_str(), e.what());
  }

  return assignedSequenceId;
}

uint32_t MercurySession::requestAudioKey(const std::vector<uint8_t>& trackId,
                                         const std::vector<uint8_t>& fileId,
                                         AudioKeyCallback audioCallback) {
  auto buffer = fileId;

  uint32_t assignedSequence;
  {
    // See execute() for why this is scoped tightly around just the
    // map/counter mutation, not the send. See F93.
    std::scoped_lock lock(sessionMutex);
    assignedSequence = this->audioKeySequence;
    this->audioKeyCallbacks.insert({assignedSequence, audioCallback});
    this->audioKeySequence += 1;
  }

  // Structure: [FILEID] [TRACKID] [4 BYTES SEQUENCE ID] [0x00, 0x00]
  buffer.insert(buffer.end(), trackId.begin(), trackId.end());
  auto audioKeySequenceBuffer = pack<uint32_t>(htonl(assignedSequence));
  buffer.insert(buffer.end(), audioKeySequenceBuffer.begin(),
                audioKeySequenceBuffer.end());
  auto suffix = std::vector<uint8_t>({0x00, 0x00});
  buffer.insert(buffer.end(), suffix.begin(), suffix.end());

  auto conn = getShanConn();
  if (conn == nullptr) {
    CSPOT_LOG(error, "Failed to send audio key request: not connected");
    return assignedSequence;
  }

  try {
    conn->sendPacket(
        static_cast<uint8_t>(RequestType::AUDIO_KEY_REQUEST_COMMAND), buffer);
  } catch (const std::exception& e) {
    // Previously silent (see F40) - same reasoning as execute()
    // above: not resolved here to avoid a tracksMutex deadlock. The waiting
    // track already times out via TrackPlayer's loadedSemaphore->twait(5000).
    CSPOT_LOG(error, "Failed to send audio key request: %s", e.what());
  }
  return assignedSequence;
}
