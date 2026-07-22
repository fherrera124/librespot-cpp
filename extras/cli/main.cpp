// Thin platform adapter: the Spotify Connect device lifecycle itself
// (ZeroConf pairing, session connect/auth/retry, engine event handling)
// lives in cspot::SpotifyConnectReceiver (librespot-cpp) - this file only
// supplies what's actually platform-specific (the audio sink, argument
// parsing) and prints engine events to stdout. See cspot_connect.cpp
// (cspot, the ESP32 app) for the same pattern applied to a real device.

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <memory>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#endif

#include "BellLogger.h"  // for setDefaultLogger/enableTimestampLogging
#include "PlaybackEvent.h"
#include "SpotifyConnectReceiver.h"
#include "TrackQueue.h"  // for TrackInfo (cspot::Event's TRACK_INFO payload)

#include "CommandLineArguments.h"

#if defined(CSPOT_ENABLE_ALSA_SINK)
#include "ALSAAudioSink.h"
#elif defined(CSPOT_ENABLE_PORTAUDIO_SINK)
#include "PortAudioSink.h"
#else
#include "NamedPipeAudioSink.h"  // for NamedPipeAudioSink
#endif

namespace {

std::atomic<bool> keepRunning{true};

void handleSigint(int) {
  keepRunning = false;
}

void handleEngineEvent(std::unique_ptr<cspot::Event> event) {
  using EventType = cspot::EventType;
  switch (event->eventType) {
    case EventType::PLAY_PAUSE:
      std::cout << (std::get<bool>(event->data) ? "Paused" : "Playing")
                << std::endl;
      break;
    case EventType::TRACK_INFO: {
      auto info = std::get<cspot::TrackInfo>(event->data);
      std::cout << "Now playing: " << info.name << " - " << info.artist
                << std::endl;
      break;
    }
    default:
      // VOLUME/DISC/NEXT/PREV/SEEK/DEPLETED/FLUSH/REPEAT_CONTEXT: no
      // stdout-worthy counterpart, SpotifyConnectReceiver already did
      // whatever engine-side reaction they need.
      break;
  }
}

void handleConnectionStateChanged(bool connected) {
  std::cout << (connected ? "Connected to Spotify" : "Disconnected")
            << std::endl;
}

void printUsage() {
  std::cout << "Usage: cspotcli [OPTION]...\n";
  std::cout << "Emulate a Spotify Connect speaker.\n";
  std::cout << "\n";
  std::cout << "Pairing happens via mDNS/ZeroConf on the local network - "
               "open the Spotify app and this device should appear as a "
               "Connect target.\n";
  std::cout << "\n";
  std::cout << "--client-id <id>          Spotify Developer Dashboard app "
               "client id (required)\n";
  std::cout << "--client-secret <secret>  Spotify Developer Dashboard app "
               "client secret (required)\n";
  std::cout << "--device-name <name>      Name advertised to Spotify "
               "clients (default: \"CSpot CLI\")\n";
  std::cout << "-b, --bitrate <kbps>      Streaming bitrate: 96, 160 or "
               "320\n";
  std::cout << "--normalisation-pregain-db <db>  Added to Spotify's own "
               "per-track gain (default: 0 = standard -14 LUFS)\n";
  std::cout << "-h, --help                Show this help\n";
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
  WSADATA wsaData;
  WORD wVersionRequested = MAKEWORD(2, 2);
  if (WSAStartup(wVersionRequested, &wsaData) != 0)
    exit(1);
#endif

  bell::setDefaultLogger();
  bell::enableTimestampLogging();

  std::shared_ptr<CommandLineArguments> args;
  try {
    args = CommandLineArguments::parse(argc, argv);
  } catch (const std::invalid_argument& e) {
    std::cout << "Invalid options passed: " << e.what() << "\n";
    std::cout << "Pass --help for more information.\n";
    return 1;
  }

  if (args->shouldShowHelp) {
    printUsage();
    return 0;
  }

#if defined(CSPOT_ENABLE_ALSA_SINK)
  auto audioSink = std::make_unique<ALSAAudioSink>();
#elif defined(CSPOT_ENABLE_PORTAUDIO_SINK)
  std::unique_ptr<AudioSink> audioSink = std::make_unique<PortAudioSink>();
#else
  auto audioSink = std::make_unique<NamedPipeAudioSink>();
#endif

  cspot::SpotifyConnectReceiverConfig receiverConfig;
  receiverConfig.deviceName = args->deviceName;
  receiverConfig.bitrate = args->bitrate;
  receiverConfig.clientId = args->clientId;
  receiverConfig.clientSecret = args->clientSecret;
  receiverConfig.normalisationPregainDb = args->normalisationPregainDb;

  auto receiver = std::make_unique<cspot::SpotifyConnectReceiver>(
      std::move(audioSink), receiverConfig, handleEngineEvent,
      handleConnectionStateChanged);

  std::signal(SIGINT, handleSigint);

  if (!receiver->startTask()) {
    std::cout << "Failed to start Spotify Connect receiver task\n";
    return 1;
  }

  std::cout << "Waiting for Spotify app to connect (device name: \""
            << args->deviceName << "\")... Press Ctrl+C to stop.\n";
  while (keepRunning) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  receiver->requestStop();
  receiver.reset();

  return 0;
}
