#include "bell/dsp/MixerTransform.h"

#include "IQmathLib.h"

#include <cstring>

using namespace bell;

void dsp::MixerTransform::configure(
    const std::vector<std::vector<int>>& mixerMapping) {
  std::scoped_lock lock(accessMutex);
  this->mixerMapping = mixerMapping;
}

float dsp::MixerTransform::calculateHeadroom() {
  return 0.0F;  // No headroom required for mixer
}

void dsp::MixerTransform::process(dsp::DataSlots& sampleSlots) {
  std::scoped_lock lock(accessMutex);

  for (size_t outputChannelIdx = 0;
       outputChannelIdx < this->mixerMapping.size(); outputChannelIdx++) {
    int accum = 0;  // accumulator for downmixing

    auto& outputChanData = (*sampleSlots.secondarySlot)[outputChannelIdx];
    memset(outputChanData.data(), 0, sizeof(int32_t) * sampleSlots.numSamples);

    for (auto chanIter = mixerMapping[outputChannelIdx].begin();
         chanIter != mixerMapping[outputChannelIdx].end(); chanIter++) {
      if (sampleSlots.primarySlot->find(*chanIter) ==
          sampleSlots.primarySlot->end()) {
        // Invalid channel
        continue;
      }

      // Increment accumulator for downmixing
      accum++;

      bool downmixByTwo =
          (accum == 2) &&
          (std::next(chanIter) == mixerMapping[outputChannelIdx].end());
      bool downmixByMore =
          (accum > 2) &&
          (std::next(chanIter) == mixerMapping[outputChannelIdx].end());

      for (size_t i = 0; i < sampleSlots.numSamples; i++) {
        outputChanData[i] += (*sampleSlots.primarySlot)[*chanIter][i];

        // Downmix when at last channel
        if (downmixByTwo) {
          outputChanData[i] = _IQdiv2(outputChanData[i]);
        } else if (downmixByMore) {
          outputChanData[i] = _IQ30div(outputChanData[i], accum);
        }
      }
    }
  }

  sampleSlots.swapSlots();  // Swap the DSP buffers
}