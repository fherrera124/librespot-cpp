#pragma once

// Standard includes
#include <array>
#include <optional>

// bell includes
#include "BiquadParameters.h"
#include "TransformPipeline.h"
#include "bell/audio/Common.h"

namespace bell::dsp {
class BiquadTransform : public Transform {
 public:
  BiquadTransform() = default;

  /**
   * @brief Add a biquad stage to the transform
   *
   * Multiple stages can be added to the transform, which will be processed in a cascade
   *
   * @param params Biquad filter parameters
   */
  void addStage(const BiquadParameters& params);

  // Transform implementation, see Transform.h for details
  void process(DataSlots& sampleSlots) override;
  float calculateHeadroom() override;
  void sampleRateUpdated(const audio::SampleRate sampleRate) override;
  Type getType() const override { return Type::BIQUAD; }

 private:
  const char* LOG_TAG = "BiquadTransform";

  // Biquad stage type
  struct BiquadStage {
    BiquadParameters params;
    // Coefficients
    int32_t a1 = 0;
    int32_t a2 = 0;
    int32_t b0 = 0;
    int32_t b1 = 0;
    int32_t b2 = 0;

    // Input state
    int32_t x1 = 0;
    int32_t x2 = 0;

    // Output state
    int32_t y1 = 0;
    int32_t y2 = 0;

    // Saved fractional part of the accumulator
    int32_t savedFractional = 0;
  };

  std::vector<BiquadStage> stages;

  /**
   * @brief Recalculate the coefficients for all stages
   */
  void recalculateCoefficients();
};
}  // namespace bell::dsp

namespace bell {
using BiquadTransform = dsp::BiquadTransform;
}
