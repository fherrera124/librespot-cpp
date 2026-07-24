#include "bell/dsp/Engine.h"
#include "bell/audio/Common.h"

using namespace bell::dsp;

void Engine::applyPipeline(const std::shared_ptr<TransformPipeline>& pipeline) {
  // Replace the active pipeline with the new one
  std::scoped_lock lock(accessMutex);
  activePipeline = pipeline;
}

DataSlots* Engine::process(const std::byte* inputBuffer, size_t inputBufferLen,
                           std::byte* outputBuffer, size_t outputBufferLen,
                           const audio::Format& format) {
  std::scoped_lock lock(accessMutex);

  // Check if a pipeline is set
  if (!activePipeline) {
    return nullptr;
  }

  // Check if all the channels are set
  for (int x = 0; x < format.getNumChannels(); x++) {
    innerDataSlots.ensureChannel(x);
  }

  if (innerDataSlots.sampleFormat != format) {
    // Update the format of the inner data slots, and clear the slots
    innerDataSlots.sampleFormat = format;
  }

  innerDataSlots.numSamples = format.bytesToSamples(inputBufferLen);

  const auto* inputAsInt16 = reinterpret_cast<const int16_t*>(inputBuffer);
  const auto* inputAsInt32 = reinterpret_cast<const int32_t*>(inputBuffer);

  uint8_t numChannels = format.getNumChannels();

  switch (format.getSampleFormat()) {
    case audio::SampleFormat::S16: {
      for (size_t frameIdx = 0; frameIdx < innerDataSlots.numSamples;
           frameIdx++) {
        for (auto chan = 0; chan < numChannels; chan++) {
          innerDataSlots.primarySlot->at(chan)[frameIdx] =
              inputAsInt16[(frameIdx * numChannels) + chan]
              << 15U;  // Shift left by 15 bits to convert to IQ30 format (1.31)
        }
      }
      break;
    }

    case audio::SampleFormat::S32: {
      for (size_t frameIdx = 0; frameIdx < innerDataSlots.numSamples;
           frameIdx++) {
        for (auto chan = 0; chan < numChannels; chan++) {
          innerDataSlots.primarySlot->at(chan)[frameIdx] =
              inputAsInt32[(frameIdx * numChannels) + chan];
        }
      }
      break;
    }

    default:
      // Unsupported bit width
      throw std::runtime_error("Unsupported bit width");
      break;
  }

  // Process the samples
  activePipeline->process(innerDataSlots);

  // Check if the output buffer is large enough
  if (format.samplesToBytes(innerDataSlots.numSamples) > outputBufferLen) {
    throw std::runtime_error("Output buffer is too small");
    return nullptr;
  }

  auto* outputData16 = reinterpret_cast<int16_t*>(outputBuffer);
  auto* outputData32 = reinterpret_cast<int32_t*>(outputBuffer);

  switch (format.getSampleFormat()) {
    case audio::SampleFormat::S16: {
      for (size_t frameIdx = 0; frameIdx < innerDataSlots.numSamples;
           frameIdx++) {
        for (auto chan = 0; chan < numChannels; chan++) {
          // Shift right by 15 bits to convert back to 16bit PCM
          outputData16[(frameIdx * numChannels) + chan] =
              innerDataSlots.primarySlot->at(chan)[frameIdx] >> 15U;
        }
      }
      break;
    }

    case audio::SampleFormat::S32: {
      for (size_t frameIdx = 0; frameIdx < innerDataSlots.numSamples;
           frameIdx++) {
        for (auto chan = 0; chan < numChannels; chan++) {
          outputData32[(frameIdx * numChannels) + chan] =
              innerDataSlots.primarySlot->at(chan)[frameIdx];
        }
      }
      break;
    }

    default:
      // Unsupported bit width
      throw std::runtime_error("Unsupported bit width");
      break;
  }

  return &innerDataSlots;
}
