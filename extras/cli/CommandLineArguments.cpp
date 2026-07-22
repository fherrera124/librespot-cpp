#include "CommandLineArguments.h"

#include <stdexcept>  // for invalid_argument

std::shared_ptr<CommandLineArguments> CommandLineArguments::parse(int argc,
                                                                   char** argv) {
  auto result = std::make_shared<CommandLineArguments>();

  for (int i = 1; i < argc; i++) {
    auto stringVal = std::string(argv[i]);

    if (stringVal == "-h" || stringVal == "--help") {
      result->shouldShowHelp = true;
      return result;
    } else if (stringVal == "--client-id") {
      if (i >= argc - 1) {
        throw std::invalid_argument("expected value after --client-id");
      }
      result->clientId = std::string(argv[++i]);
    } else if (stringVal == "--client-secret") {
      if (i >= argc - 1) {
        throw std::invalid_argument("expected value after --client-secret");
      }
      result->clientSecret = std::string(argv[++i]);
    } else if (stringVal == "--device-name") {
      if (i >= argc - 1) {
        throw std::invalid_argument("expected value after --device-name");
      }
      result->deviceName = std::string(argv[++i]);
    } else if (stringVal == "-b" || stringVal == "--bitrate") {
      if (i >= argc - 1) {
        throw std::invalid_argument("expected value after the bitrate flag");
      }
      auto bitrateStr = std::string(argv[++i]);
      if (bitrateStr != "96" && bitrateStr != "160" && bitrateStr != "320") {
        throw std::invalid_argument("invalid bitrate argument");
      }
      result->bitrate = std::stoi(bitrateStr);
    } else if (stringVal == "--normalisation-pregain-db") {
      if (i >= argc - 1) {
        throw std::invalid_argument(
            "expected value after --normalisation-pregain-db");
      }
      result->normalisationPregainDb = std::stof(std::string(argv[++i]));
    } else {
      throw std::invalid_argument(("unknown flag '" + stringVal + "'").c_str());
    }
  }

  if (result->clientId.empty() || result->clientSecret.empty()) {
    throw std::invalid_argument(
        "--client-id and --client-secret are required (create an app at "
        "the Spotify Developer Dashboard to get them)");
  }

  return result;
}
