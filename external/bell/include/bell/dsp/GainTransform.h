#pragma once

#include "TransformPipeline.h"

namespace bell::dsp {
/**
  * @brief Gain transform that multiplies the audio samples by a constant gain factor.
  */
class GainTransform : public Transform {
 public:
  GainTransform() = default;

  /**
   * @brief Set the gain of the transform in dB.
   *
   * @param gainDb Gain in dB
   */
  void configure(float gainDb);

  // Transform implementation, see Transform.h for details
  void process(DataSlots& sampleSlots) override;
  float calculateHeadroom() override;
  Type getType() const override { return Type::GAIN; }

 private:
  // 1.0 in IQ30
  int32_t gainFactor = (1 << 30U);
  float gainDb = 0.0F;
};
}  // namespace bell::dsp

namespace bell {
using GainTransform = dsp::GainTransform;
}
