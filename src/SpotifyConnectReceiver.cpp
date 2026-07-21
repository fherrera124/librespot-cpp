#include "SpotifyConnectReceiver.h"

#include <algorithm>
#include <map>

#include "AudioSink.h"
#include "BellUtils.h"  // for BELL_SLEEP_MS
#include "CSpotContext.h"
#include "PlayerEngine.h"
#include "DealerClient.h"
#include "LoginBlob.h"
#include "Logger.h"
#include "MDNSService.h"
#include "SimpleHTTPServer.h"
#include "TrackPlayer.h"
#include "URLParser.h"

using namespace cspot;

namespace {
const char* TAG = "SpotifyConnectReceiver";
}  // namespace

SpotifyConnectReceiver::SpotifyConnectReceiver(
    std::unique_ptr<AudioSink> audioSink, SpotifyConnectReceiverConfig config,
    EventHandler eventHandler, ConnectionStateCallback onConnectionStateChanged)
    : bell::Task("spotifyConnectReceiver", 32 * 1024, 0, 0),
      deviceName(config.deviceName.empty() ? "cspot" : config.deviceName),
      volume(config.initialVolume),
      bitrate(config.bitrate == 96 || config.bitrate == 320 ? config.bitrate
                                                             : 160),
      zeroconfHttpPort(config.zeroconfHttpPort),
      clientId(config.clientId),
      clientSecret(config.clientSecret),
      eventHandler(std::move(eventHandler)),
      connectionStateCallback(std::move(onConnectionStateChanged)),
      audioSink(std::move(audioSink)) {}

// Out-of-line (not defaulted in the header) - see the header's comment on
// the declaration.
SpotifyConnectReceiver::~SpotifyConnectReceiver() = default;

void SpotifyConnectReceiver::requestStop() {
  running = false;
  clientConnected.give();
}

bool SpotifyConnectReceiver::requestPlayPause(bool play) {
  if (!linked || !dealer) return false;
  dealer->getConnectState()->setPause(!play);
  return true;
}

bool SpotifyConnectReceiver::requestNext() {
  if (!linked || !dealer) return false;
  return dealer->getConnectState()->nextSong();
}

bool SpotifyConnectReceiver::requestPrevious() {
  if (!linked || !dealer) return false;
  return dealer->getConnectState()->previousSong();
}

bool SpotifyConnectReceiver::requestSetRepeatContext(bool enabled) {
  if (!linked || !dealer) return false;
  dealer->getConnectState()->setRepeatContext(enabled);
  return true;
}

bool SpotifyConnectReceiver::requestSeek(uint32_t positionMs) {
  if (!linked || !dealer) return false;
  dealer->getConnectState()->seekMs(positionMs);
  return true;
}

uint32_t SpotifyConnectReceiver::getPositionMs() {
  if (!linked || !dealer) return 0;
  return dealer->getConnectState()->getPositionMs();
}

void SpotifyConnectReceiver::runTask() {
  startHttpServerAndMdns();

  while (running) {
    clientConnected.wait();
    if (!running) break;

    CSPOT_LOG(info, "Spotify client connecting for %s", deviceName.c_str());
    runSession();
  }

  if (mdns) mdns->unregisterService();
}

void SpotifyConnectReceiver::startHttpServerAndMdns() {
  httpServer = std::make_unique<bell::SimpleHTTPServer>(zeroconfHttpPort);
  httpServer->registerGet(
      "/spotify_info",
      [this](const std::string& /*body*/,
            const bell::SimpleHTTPServer::Params&) {
        return bell::SimpleHTTPServer::HTTPResponse{
            blob->buildZeroconfInfo(), "application/json", 200};
      });
  httpServer->registerPost(
      "/spotify_info",
      [this](const std::string& body,
            const bell::SimpleHTTPServer::Params&) {
        // ZeroConf pairing: https://developer.spotify.com/documentation/commercial-hardware/implementation/guides/zeroconf
        if (body.empty()) {
          return bell::SimpleHTTPServer::HTTPResponse{"", "text/plain", 400};
        }

        std::map<std::string, std::string> queryMap;
        size_t pos = 0;
        while (pos < body.size()) {
          size_t amp = body.find('&', pos);
          std::string pair = body.substr(
              pos, amp == std::string::npos ? std::string::npos : amp - pos);
          size_t eq = pair.find('=');
          if (eq != std::string::npos) {
            queryMap[bell::URLParser::urlDecode(pair.substr(0, eq))] =
                bell::URLParser::urlDecode(pair.substr(eq + 1));
          }
          if (amp == std::string::npos) break;
          pos = amp + 1;
        }

        // loadZeroconfQuery() can throw on a PSA failure; uncaught here it
        // would crash the caller's task instead of just failing this
        // pairing. See F65.
        try {
          blob->loadZeroconfQuery(queryMap);
        } catch (const std::exception& e) {
          CSPOT_LOG(error, "loadZeroconfQuery failed: %s", e.what());
          return bell::SimpleHTTPServer::HTTPResponse{"", "text/plain", 500};
        }
        clientConnected.give();

        return bell::SimpleHTTPServer::HTTPResponse{
            R"({"status":101,"statusString":"OK","spotifyError":0})",
            "application/json", 200};
      });

  // bell::MDNSService::registerService() starts the mDNS responder itself
  // on first call, on every platform backend.
  blob = std::make_unique<cspot::LoginBlob>(deviceName);
  mdns = bell::MDNSService::registerService(
      blob->getDeviceName(), "_spotify-connect", "_tcp", "", zeroconfHttpPort,
      {{"VERSION", "1.0"}, {"CPath", "/spotify_info"}, {"Stack", "SP"}});

  CSPOT_LOG(info,
           "Spotify Connect device '%s' advertised, waiting to be selected "
           "from the Spotify app",
           deviceName.c_str());
}

void SpotifyConnectReceiver::runSession() {
  // cspot throws on unrecoverable network/protocol errors; uncaught, that
  // would escape this task. `blob` (the cached credentials this session
  // already has) stays valid across attempts, so a transient failure here
  // (AP unreachable, etc.) is worth retrying with backoff instead of
  // sitting idle until the app itself happens to retry pairing - which
  // could take much longer. A clean return (session ran and ended
  // normally, or authenticate() failed - already logged inside
  // runSessionInner()) is NOT retried here: only genuine exceptions are,
  // since those are the transient-network case.
  int backoffMs = SESSION_RETRY_BASE_MS;
  for (int attempt = 1; running && attempt <= SESSION_MAX_ATTEMPTS;
       attempt++) {
    try {
      runSessionInner();
      return;
    } catch (const std::exception& e) {
      CSPOT_LOG(error, "session failed (attempt %d/%d): %s", attempt,
               SESSION_MAX_ATTEMPTS, e.what());
      if (connectionStateCallback) connectionStateCallback(false);
    }

    if (attempt == SESSION_MAX_ATTEMPTS) {
      break;
    }
    CSPOT_LOG(info, "retrying session in %dms", backoffMs);
    for (int slept = 0; running && slept < backoffMs; slept += 250) {
      BELL_SLEEP_MS(250);
    }
    backoffMs = std::min(backoffMs * 2, SESSION_RETRY_MAX_MS);
  }
}

void SpotifyConnectReceiver::runSessionInner() {
  auto ctx = cspot::Context::createFromBlob(blob);

  if (bitrate == 320)
    ctx->config.audioFormat = AudioFormat_OGG_VORBIS_320;
  else if (bitrate == 96)
    ctx->config.audioFormat = AudioFormat_OGG_VORBIS_96;
  else
    ctx->config.audioFormat = AudioFormat_OGG_VORBIS_160;

  ctx->session->connectWithRandomAp();
  ctx->config.authData = ctx->session->authenticate(blob);
  ctx->config.clientId = clientId;
  ctx->config.clientSecret = clientSecret;

  if (ctx->config.authData.empty()) {
    CSPOT_LOG(error,
             "authentication failed, waiting for a new pairing attempt");
    return;
  }

  // Reported in every PUT's DeviceInfo.volume (PlayerEngine::
  // buildDeviceInfo()) from here on.
  ctx->config.volume = volume;
  linked = true;

  // Dealer connection, publishing state, and (via its PlayerEngine)
  // the actual playback engine - real player/command requests execute
  // directly against it. Connects on its own task, nothing here blocks on
  // it.
  dealer = std::make_unique<cspot::DealerClient>(ctx);
  auto connectState = dealer->getConnectState();

  // audioSink outlives every session (real hardware/ring buffer) -
  // TrackPlayer itself is recreated per session, so it just gets pointed
  // at the same sink each time.
  connectState->getTrackPlayer()->setAudioSink(audioSink.get());

  connectState->setEventHandler(
      [this](std::unique_ptr<cspot::Event> event) {
        handleEngineEvent(std::move(event));
      });

  ctx->session->startTask();

  if (connectionStateCallback) connectionStateCallback(true);

  while (linked && running) {
    ctx->session->handlePacket();
  }

  connectState->disconnect();
  dealer->stop();
  dealer.reset();
  ctx->session->disconnect();
  ctx.reset();
  if (connectionStateCallback) connectionStateCallback(false);
  CSPOT_LOG(info, "disconnected from Spotify");
}

void SpotifyConnectReceiver::handleEngineEvent(
    std::unique_ptr<cspot::Event> event) {
  using EventType = cspot::EventType;
  switch (event->eventType) {
    case EventType::PLAYBACK_START:
      if (dealer) {
        dealer->getConnectState()->getTrackPlayer()->setAudioParams(44100, 2,
                                                                     16);
      }
      break;
    case EventType::PLAY_PAUSE: {
      bool isPaused = std::get<bool>(event->data);
      // Discards whatever's already buffered downstream so pause is heard
      // immediately instead of after it drains - handled inside
      // TrackPlayer::setPaused().
      if (dealer) {
        dealer->getConnectState()->getTrackPlayer()->setPaused(isPaused);
      }
      break;
    }
    case EventType::VOLUME:
      volume = std::get<int>(event->data);
      if (dealer) {
        dealer->getConnectState()->getTrackPlayer()->setVolume(
            static_cast<uint16_t>(volume));
      }
      break;
    case EventType::DISC:
      linked = false;
      break;
    case EventType::DEPLETED:
      dealer->getConnectState()->notifyAudioEnded();
      break;
    default:
      break;
  }

  if (eventHandler) eventHandler(std::move(event));
}
