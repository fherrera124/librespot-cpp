#pragma once

#include "BiquadTransform.h"
#include "TransformPipeline.h"

namespace bell::dsp {
/**
  * @brief BiquadCombo contains multiple biquad filters, composing a higher order filter.
  */
class BiquadComboTransform : public Transform {
 public:
  BiquadComboTransform() = default;

  enum class Type {
    LR_Lowpass,   // Linkwitz-Riley Lowpass
    LR_Highpass,  // Linkwitz-Riley Highpass
    BW_Lowpass,   // Butterworth Lowpass
    BW_Highpass   // Butterworth Highpass
  };

  static Type stringToType(const std::string& type) {
    const std::unordered_map<std::string, Type> typeMap = {
        {"lr_lowpass", Type::LR_Lowpass},
        {"lr_highpass", Type::LR_Highpass},
        {"bw_lowpass", Type::BW_Lowpass},
        {"bw_highpass", Type::BW_Highpass},
    };

    if (typeMap.find(type) == typeMap.end()) {
      throw std::invalid_argument("Invalid filter type");
    }

    return typeMap.at(type);
  }

  // Configure the filter with the given type, frequency and order
  void configure(Type filterType, float freq, int order);

  // Transform implementation, see Transform.h for details
  void process(DataSlots& sampleSlots) override;
  float calculateHeadroom() override;

 private:
  std::unique_ptr<BiquadTransform> biquad;

  // Calculates Q values for Nth order Butterworth
  static std::vector<float> calculateBWQ(int order);

  // Calculates Q values for Nth order Linkwitz-Riley
  static std::vector<float> calculateLRQ(int order);

  // Configures a Linkwitz-Riley filter, with the given frequency and order
  void configureLinkwitzRiley(float freq, int order, bool isLowpass);

  // Configures a Butterworth filter, with the given frequency and order
  void configureButterworth(float freq, int order, bool isLowpass);
};
}  // namespace bell::dsp

namespace bell {
using BiquadComboTransform = dsp::BiquadComboTransform;
}
