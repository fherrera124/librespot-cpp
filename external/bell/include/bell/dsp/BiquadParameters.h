#pragma once

// Standard includes
#include <array>
#include <optional>
#include <string>
#include <vector>

// Bell includes
#include "bell/audio/Common.h"

namespace bell::dsp {
// Structure responsible for storage and calculation of biquad filter coefficients
class BiquadParameters {
 public:
  /**
   * @brief Biquad filter type enumeration
   */
  enum class Type : uint8_t {
    Free,
    Highpass,
    Lowpass,
    HighpassFO,
    LowpassFO,
    Peaking,
    Highshelf,
    HighshelfFO,
    Lowshelf,
    LowshelfFO,
    Notch,
    Bandpass,
    Allpass,
    AllpassFO
  };

  /**
   * @brief Construct and calculate coefficients for a given Biquad filter
   *
   * @remark Required parameters depend on the selected filter type
   *
   * @param type filter type
   * @param f optional filter frequency
   * @param q optional filter q-value
   * @param gain optional filter gain
   * @param slope optional filter slope
   * @param bandwidth optional filter bandwidth
   */
  BiquadParameters(Type type, std::optional<float> f, std::optional<float> q,
                   std::optional<float> gain, std::optional<float> slope,
                   std::optional<float> bandwidth);

  /**
   * @brief Construct a Biquad filter of "Free" type, with pre-computed coefficients
   *
   * @remark Remember that these coefficients wont be adjusted with sample rate changes
   */
  BiquadParameters(float a1, float a2, float b0, float b1, float b2);

  // Returns inner filter type
  Type getType() const { return filterType; }

  // Calculates coefficients for given filter type and parameters
  // Coefficients are in IQ28 format, aka mathematical value scaled by 2^28
  std::array<int32_t, 5> calculateCoefficients(
      const audio::SampleRate sampleRate);

  /**
   * @brief Parse a string to a biquad filter type
   *
   * @param type String representation of the filter type
   * @return Type Parsed filter type
   */
  static Type stringToType(const std::string& type);

  /**
   * @brief Calculates a vector of biquad parameters required to construct a higher-order linkwitz-riley filter
   *
   * @param type Either Lowpass or Highpass
   * @param frequency cutoff frequency
   * @param order filters order
   * @return std::vector<BiquadParameters>
   */
  static std::vector<BiquadParameters> linkwitzRiley(Type type, float frequency,
                                                     int order);

  /**
   * @brief Calculates a vector of biquad parameters required to construct a higher-order butterworth filter
   *
   * @param type Either Lowpass or Highpass
   * @param frequency cutoff frequency
   * @param order filters order
   * @return std::vector<BiquadParameters>
   */
  static std::vector<BiquadParameters> butterworth(Type type, float frequency,
                                                   int order);

 private:
  const char* LOG_TAG = "BiquadParameters";

  // Filter type
  Type filterType = Type::Free;

  // Possible configuration parameters
  std::optional<float> fValue;
  std::optional<float> qValue;
  std::optional<float> gainValue;
  std::optional<float> slopeValue;
  std::optional<float> bandwidthValue;

  // Holds calculated coefficients
  std::optional<float> a1Value;
  std::optional<float> a2Value;
  std::optional<float> b0Value;
  std::optional<float> b1Value;
  std::optional<float> b2Value;

  // Generator methods for different filter types
  void highPassCoEffs(float sampleRate, float f, float q);
  void highPassFOCoEffs(float sampleRate, float f);
  void lowPassCoEffs(float sampleRate, float f, float q);
  void lowPassFOCoEffs(float sampleRate, float f);

  void peakCoEffs(float sampleRate, float f, float gain, float q);
  void peakCoEffsBandwidth(float sampleRate, float f, float gain,
                           float bandwidth);

  void highShelfCoEffs(float sampleRate, float f, float gain, float q);
  void highShelfCoEffsSlope(float sampleRate, float f, float gain, float slope);
  void highShelfFOCoEffs(float sampleRate, float f, float gain);

  void lowShelfCoEffs(float sampleRate, float f, float gain, float q);
  void lowShelfCoEffsSlope(float sampleRate, float f, float gain, float slope);
  void lowShelfFOCoEffs(float sampleRate, float f, float gain);

  void notchCoEffs(float sampleRate, float f, float q);
  void notchCoEffsBandwidth(float sampleRate, float f, float bandwidth);

  void bandPassCoEffs(float sampleRate, float f, float q);
  void bandPassCoEffsBandwidth(float sampleRate, float f, float bandwidth);

  void allPassCoEffs(float sampleRate, float f, float q);
  void allPassCoEffsBandwidth(float sampleRate, float f, float bandwidth);
  void allPassFOCoEffs(float sampleRate, float f);

  void normalizeCoEffs(float a0, float a1, float a2, float b0, float b1,
                       float b2);
};
}  // namespace bell::dsp
