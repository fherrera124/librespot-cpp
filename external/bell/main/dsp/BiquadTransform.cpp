#include "bell/dsp/BiquadTransform.h"

// Standard includes
#include <mutex>

// IQmathLib
#include <IQmathLib.h>

using namespace bell::dsp;

void BiquadTransform::addStage(const BiquadParameters& params) {
  std::scoped_lock lock(accessMutex);

  // Add the stage to the list
  stages.push_back({
      .params = params,
  });

  recalculateCoefficients();
}

void BiquadTransform::sampleRateUpdated(const audio::SampleRate sampleRate) {
  std::scoped_lock lock(accessMutex);

  if (this->sampleRate != sampleRate) {
    this->sampleRate = sampleRate;

    // Coefficients are dependent on the sample rate
    recalculateCoefficients();
  }
}

void BiquadTransform::recalculateCoefficients() {
  for (auto& stage : stages) {
    auto coeffs = stage.params.calculateCoefficients(this->sampleRate);

    // Assign the coefficients to the stage
    stage.a1 = coeffs[0];
    stage.a2 = coeffs[1];
    stage.b0 = coeffs[2];
    stage.b1 = coeffs[3];
    stage.b2 = coeffs[4];

    // Reset the state variables
    stage.x1 = 0;
    stage.x2 = 0;
    stage.y1 = 0;
    stage.y2 = 0;
    stage.savedFractional = 0;
  }
}

float BiquadTransform::calculateHeadroom() {
  return 0.0F;
}

void BiquadTransform::process(DataSlots& sampleSlots) {
  std::scoped_lock lock(accessMutex);

  if (stages.empty()) {
    return;  // No stages to process
  }

  auto& input = sampleSlots.primarySlot->at(this->channels[0]);

  // Direct form 1 biquad filter, with basic noise shaping
  // Based on robert bristow-johnson code from https://dsp.stackexchange.com/questions/21792/best-implementation-of-a-real-time-fixed-point-iir-filter-with-constant-coeffic
  for (auto stageItr = stages.begin(); stageItr != stages.end(); ++stageItr) {
    if (stageItr != stages.begin()) {
      // For inputs, use the output of the previous stage
      stageItr->x1 = std::prev(stageItr)->y1;
      stageItr->x2 = std::prev(stageItr)->y2;
    }

    int64_t accumulator = stageItr->savedFractional;
    for (size_t i = 0; i < sampleSlots.numSamples; i++) {
      // IQ30 * IQ28 = IQ58
      accumulator += (int64_t)stageItr->b0 * input[i];
      accumulator += (int64_t)stageItr->b1 * stageItr->x1;
      accumulator += (int64_t)stageItr->b2 * stageItr->x2;
      accumulator += (int64_t)stageItr->a1 * stageItr->y1;
      accumulator += (int64_t)stageItr->a2 * stageItr->y2;

      // Clip values
      if (accumulator > 0x07FFFFFFFFFFFFFFLL) {
        accumulator = 0x07FFFFFFFFFFFFFFLL;
      } else if (accumulator < -0x0800000000000000LL) {
        accumulator = -0x0800000000000000LL;
      }

      // point of quantization, always rounding down
      int32_t y = (int32_t)(accumulator >> 28);

      // bump the states over
      stageItr->x2 = stageItr->x1;
      stageItr->x1 = input[i];
      stageItr->y2 = stageItr->y1;
      stageItr->y1 = y;

      // keep the fractional bits that were dropped for
      accumulator &= 0x000000000FFFFFFFLL;

      input[i] = y;
    }

    // Narrowing is fine, as we've previously masked the fractional bits
    stageItr->savedFractional = static_cast<int32_t>(accumulator);
  }
}
