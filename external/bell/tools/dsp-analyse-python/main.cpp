#include "bell/Logger.h"
#include "bell/audio/Types.h"
#include "bell/dsp/Engine.h"
#include "bell/dsp/TaoJSONParser.h"
#include <tao/json.hpp>

// DSP engine
bell::dsp::Engine dspEngine;

extern "C" {
void init() {
  bell::registerDefaultLogger();
}

bool parseDspPipeline(const char* jsonStr, size_t jsonStrLen) {
  try {
    const auto pipelineJson =
        tao::json::from_string(std::string(jsonStr, jsonStrLen));
    const auto pipeline = bell::dsp::parseTaoJsonPipeline(pipelineJson);
    dspEngine.applyPipeline(pipeline);
    return true;
  } catch (const std::exception& e) {
    BELL_LOG(error, "dsp_engine", "Error parsing pipeline: {}", e.what());
    return false;
  }
}

bool runDspPipeline(const uint8_t* inputBuffer, size_t inputBufferLen,
                    uint8_t* outputBuffer, size_t outputBufferLen,
                    uint8_t numChannels, uint8_t bitWidth,
                    uint32_t sampleRate) {
  const auto format = bell::audio::Format(numChannels, bitWidth, sampleRate);
  try {
    const auto* res = dspEngine.process(inputBuffer, inputBufferLen,
                                        outputBuffer, outputBufferLen, format);
    return res != nullptr;
  } catch (const std::exception& e) {
    BELL_LOG(error, "dsp_engine", "Error processing audio: {}", e.what());
    return false;
  }
}
}