#pragma once

#include <fstream>
#include <trompeloeil.hpp>

#include "api/SpClient.h"
#include "bell/http/Writer.h"
#include "fmt/color.h"

// Mock for SpClient class
class MockSpClient : public cspot::SpClient {
 public:
  MAKE_MOCK3(putConnectState,
             bell::Result<>(cspot_proto::PutStateRequest&, const std::string&, const std::string&),
             override);

  MAKE_MOCK1(contextResolve,
             bell::Result<bell::HTTPResponse>(const std::string&), override);

  MAKE_MOCK1(
      contextAutoplayResolve,
      bell::Result<bell::HTTPResponse>(cspot_proto::AutoplayContextRequest&),
      override);

  MAKE_MOCK1(rawRequest, bell::Result<bell::HTTPResponse>(const std::string&),
             override);

  MAKE_MOCK1(trackMetadata,
             bell::Result<cspot_proto::Track>(const cspot::SpotifyId&),
             override);

  MAKE_MOCK1(episodeMetadata,
             bell::Result<cspot_proto::Episode>(const cspot::SpotifyId&),
             override);

  MAKE_MOCK2(resolveStorageInteractive,
             bell::Result<std::string>(const std::vector<std::byte>&, bool),
             override);

  MAKE_MOCK2(playPlayLicense,
             bell::Result<cspot::PlayPlayLicense>(
                 const std::vector<std::byte>&, bool),
             override);
};

// Helper function to create a valid bell::http::Response for tests.
// testFilePath is relative to TEST_DATA_PATH
inline bell::http::Response createMockResponseTestFile(
    int statusCode, const std::string& testFilePath,
    const std::string& contentType) {
  auto ss = std::make_shared<std::stringstream>();

  // Open the test data file
  std::ifstream testDataFile(fmt::format("{}/{}", TEST_DATA_PATH, testFilePath),
                             std::ios::binary);

  // Read the entire file content into a string
  std::string bodyStr((std::istreambuf_iterator<char>(testDataFile)),
                      std::istreambuf_iterator<char>());

  // Write the mocked response body to the stringstream.
  bell::http::Writer writer(bell::http::Direction::Response, ss);
  (void)writer.writeResponseWithBody(statusCode,
                                     {{"Content-Type", contentType}}, bodyStr);

  // Construct reader object from same stringstream
  bell::http::Reader reader(bell::http::Direction::Response, ss);

  // Read headers to ensure they are parsed correctly.
  (void)reader.readHeaders();

  return {std::move(reader)};
}
