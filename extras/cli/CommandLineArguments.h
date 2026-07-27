#ifndef COMMANDLINEARGUMENTS_H
#define COMMANDLINEARGUMENTS_H
#include <memory>  // for shared_ptr
#include <string>  // for string, basic_string

// Parses cspotcli's command line arguments. SpotifyConnectReceiver only
// supports ZeroConf pairing - the resulting user-session token (Login5)
// is also what CDN storage-resolve uses (see TrackQueue::TrackQueue()'s
// own comment), so there's no separate app credential to supply here.
class CommandLineArguments {
 public:
  std::string deviceName = "CSpot CLI";
  int bitrate = 160;  // 96, 160 or 320 (kbps) - matches SpotifyConnectReceiverConfig::bitrate
  float normalisationPregainDb = 0.0f;
  bool shouldShowHelp = false;

  CommandLineArguments() = default;

  static std::shared_ptr<CommandLineArguments> parse(int argc, char** argv);
};

#endif
