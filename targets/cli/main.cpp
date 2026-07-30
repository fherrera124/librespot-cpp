#include <memory>

#include "AudioSinkALSA.h"
#include "AuthInfo.h"
#include "ConnectReceiver.h"
#include "bell/Logger.h"

namespace {
const char* sessionFilePath = "session.json";
}

int main(int argc, char** argv) {
  bell::registerDefaultLogger();

  auto authInfo = std::make_shared<cspot::AuthInfo>("Cspot player");

  auto audioSink = std::make_shared<cspot::AudioSinkALSA>();
  cspot::AudioOutputCallback audioCallback =
      [audioSink](tcb::span<const std::byte> pcm, const cspot::SpotifyId&) {
        audioSink->feedPCMFrames(reinterpret_cast<const uint8_t*>(pcm.data()),
                                 pcm.size());
      };
  cspot::AudioFlushCallback audioFlushCallback = [audioSink]() {
    audioSink->flush();
  };

  cspot::ConnectReceiver(authInfo, sessionFilePath, audioCallback,
                         [](uint16_t) {}, audioFlushCallback)
      .run();
  return 0;
}
