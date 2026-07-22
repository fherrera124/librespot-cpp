#ifndef COMMANDLINEARGUMENTS_H
#define COMMANDLINEARGUMENTS_H
#include <memory>  // for shared_ptr
#include <string>  // for string, basic_string

// Parses cspotcli's command line arguments. SpotifyConnectReceiver only
// supports ZeroConf pairing (no direct username/password login), so
// clientId/clientSecret (a Spotify Developer Dashboard app's credentials)
// are the only required inputs - there's no Kconfig here to supply them
// like the ESP32 build has.
class CommandLineArguments {
 public:
  std::string clientId;
  std::string clientSecret;
  std::string deviceName = "CSpot CLI";
  int bitrate = 160;  // 96, 160 or 320 (kbps) - matches SpotifyConnectReceiverConfig::bitrate
  float normalisationPregainDb = 0.0f;
  bool shouldShowHelp = false;

  CommandLineArguments() = default;

  static std::shared_ptr<CommandLineArguments> parse(int argc, char** argv);
};

#endif
