#include <cstdint>
#include <fstream>
#include <iostream>
#include <tao/json.hpp>

#include "bell/Logger.h"
#include "bell/dsp/Engine.h"
#include "bell/dsp/TaoJSONParser.h"

int main(int argc, char* argv[]) {
  bell::registerDefaultLogger();

  // DSP engine
  bell::dsp::Engine dspEngine;

  if (argc != 4) {
    std::cerr << "Usage: " << argv[0]
              << " <pipeline.json> <input.pcm> <output.pcm>\n";
    return 1;
  }

  // Load the pipeline from the JSON file
  const tao::json::value pipelineJSON = tao::json::from_file(argv[1]);

  // Open the input and output PCM files
  std::ifstream inputPCM(argv[2], std::ios::binary);
  std::ofstream outputPCM(argv[3], std::ios::binary);

  // Apply the pipeline to the DSP engine
  dspEngine.applyPipeline(bell::dsp::parseTaoJsonPipeline(pipelineJSON));

  // Pre-defined audio format
  auto format = bell::audio::Format(2, bell::audio::BitWidth::BW_16,
                                    bell::audio::SampleRate::SR_44100HZ);

  // Process the input PCM data in 1024 byte chunks
  constexpr size_t chunkSize = 1024;
  std::vector<uint8_t> inputBuffer(chunkSize);
  std::vector<uint8_t> outputBuffer(chunkSize);

  while (inputPCM) {
    inputPCM.read(reinterpret_cast<char*>(inputBuffer.data()), chunkSize);
    const auto* res = dspEngine.process(inputBuffer.data(), inputPCM.gcount(),
                                        outputBuffer.data(), chunkSize, format);

    if (res == nullptr) {
      BELL_LOG(error, "dsp_engine", "Error processing audio");
      return 1;
    }

    outputPCM.write(reinterpret_cast<const char*>(outputBuffer.data()),
                    res->sampleFormat.samplesToBytes(res->numSamples));
  }
  return 0;
}
