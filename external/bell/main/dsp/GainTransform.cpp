#include "bell/dsp/GainTransform.h"

// Standard includes
#include <cmath>
#include <mutex>

// IQmathLib
#include "IQmathLib.h"

using namespace bell::dsp;

void GainTransform::configure(float gainDb) {
  std::scoped_lock lock(accessMutex);
  this->gainDb = gainDb;
  this->gainFactor = _IQ30(powf(10.0F, gainDb / 20.0F));
}

float GainTransform::calculateHeadroom() {
  return gainDb;
}

void GainTransform::process(DataSlots& sampleSlots) {
  std::scoped_lock lock(accessMutex);

  for (uint32_t i = 0; i < sampleSlots.numSamples; i++) {
    // Apply gain to all channels
    for (auto& channel : channels) {
      sampleSlots.primarySlot->at(channel)[i] =
          _IQ30mpy(sampleSlots.primarySlot->at(channel)[i], this->gainFactor);
    }
  }
}