#include "bell/dsp/BiquadParameters.h"

// Standard includes
#include <cmath>
#include <optional>
#include <stdexcept>
#include <unordered_map>

// Library includes
#include "IQmathLib.h"

// Bell includes
#include "bell/Logger.h"

using namespace bell::dsp;

namespace {
const float floatPi = M_PI;

// Calculates q-values for butterworth filter of given order
std::vector<float> calculateBWQ(int order) {
  std::vector<float> qValues;
  for (int n = 0; n < order / 2; n++) {
    float angle = M_PI * (2.0F * (((float)n + 1.0F) + (float)order - 1) /
                          (2.0F * (float)order));
    float q = 1.0F / (2.0F * sinf(angle));
    qValues.push_back(q);
  }

  return qValues;
}

// Calculates q-values for linkwitz-riley filter of given type
std::vector<float> calculateLRQ(int order) {
  auto qValues = calculateBWQ(order / 2);

  if (order % 4 > 0) {
    qValues.pop_back();
    qValues.insert(qValues.end(), qValues.begin(), qValues.end());
    qValues.push_back(0.5F);
  } else {
    qValues.insert(qValues.end(), qValues.begin(), qValues.end());
  }

  return qValues;
}
};  // namespace

BiquadParameters::BiquadParameters(BiquadParameters::Type type,
                                   std::optional<float> frequency,
                                   std::optional<float> q,
                                   std::optional<float> gain,
                                   std::optional<float> slope,
                                   std::optional<float> bandwidth)
    : filterType(type),
      fValue(frequency),
      qValue(q),
      gainValue(gain),
      slopeValue(slope),
      bandwidthValue(bandwidth) {}

BiquadParameters::BiquadParameters(float a1, float a2, float b0, float b1,
                                   float b2)
    : a1Value(a1), a2Value(a2), b0Value(b0), b1Value(b1), b2Value(b2) {}

std::vector<BiquadParameters> BiquadParameters::linkwitzRiley(Type type,
                                                              float frequency,
                                                              int order) {
  std::vector<BiquadParameters> params;
  std::vector<float> qValues = calculateLRQ(order);

  if (type != Type::Lowpass && type != Type::Highpass) {
    throw std::runtime_error(
        "Linkwitz-riley filter can only be of highpass or lowpass type");
  }

  for (const auto& q : qValues) {
    if (q >= 0.0) {
      BiquadParameters biquadParams(type, frequency, q, std::nullopt,
                                    std::nullopt, std::nullopt);
      params.push_back(biquadParams);
    } else {
      BiquadParameters biquadParams(
          type == Type::Lowpass ? Type::LowpassFO : Type::HighpassFO, frequency,
          std::nullopt, std::nullopt, std::nullopt, std::nullopt);
      params.push_back(biquadParams);
    }
  }

  return params;
}

std::vector<BiquadParameters> BiquadParameters::butterworth(Type type,
                                                            float frequency,
                                                            int order) {
  std::vector<BiquadParameters> params;
  std::vector<float> qValues = calculateBWQ(order);

  if (type != Type::Lowpass && type != Type::Highpass) {
    throw std::runtime_error(
        "Butterworth filter can only be of highpass or lowpass type");
  }

  for (const auto& q : qValues) {

    if (q >= 0.0) {
      BiquadParameters biquadParams(type, frequency, q, std::nullopt,
                                    std::nullopt, std::nullopt);
      params.push_back(biquadParams);
    } else {
      BiquadParameters biquadParams(
          type == Type::Lowpass ? Type::LowpassFO : Type::HighpassFO, frequency,
          std::nullopt, std::nullopt, std::nullopt, std::nullopt);
      params.push_back(biquadParams);
    }
  }

  return params;
}

std::array<int32_t, 5> BiquadParameters::calculateCoefficients(
    const audio::SampleRate sampleRate) {
  // Flag to check if the parameters are correct
  bool correctParams = true;

  float sampleRateFloat = static_cast<float>(sampleRate);

  switch (this->filterType) {
    case Type::Highpass:
      if (!fValue.has_value() || !qValue.has_value()) {
        correctParams = false;
        break;
      }
      this->highPassCoEffs(sampleRateFloat, fValue.value(), qValue.value());
      break;
    case Type::HighpassFO:
      if (!fValue.has_value()) {
        correctParams = false;
        break;
      }

      this->highPassFOCoEffs(sampleRateFloat, fValue.value());
      break;
    case Type::Lowpass:
      if (!fValue.has_value() || !qValue.has_value()) {
        correctParams = false;
        break;
      }
      this->lowPassCoEffs(sampleRateFloat, fValue.value(), qValue.value());
      break;
    case Type::LowpassFO:
      if (!fValue.has_value()) {
        correctParams = false;
        break;
      }
      this->lowPassFOCoEffs(sampleRateFloat, fValue.value());
      break;
    case Type::Highshelf:
      // check if config has slope key
      if (slopeValue.has_value()) {
        if (!fValue.has_value() || !gainValue.has_value()) {
          correctParams = false;
          break;
        }
        this->highShelfCoEffsSlope(sampleRateFloat, fValue.value(),
                                   gainValue.value(), slopeValue.value());
      } else {
        if (!fValue.has_value() || !gainValue.has_value() ||
            !qValue.has_value()) {
          correctParams = false;
          break;
        }
        this->highShelfCoEffs(sampleRateFloat, fValue.value(),
                              gainValue.value(), qValue.value());
      }
      break;
    case Type::HighshelfFO:
      if (!fValue.has_value() || !gainValue.has_value()) {
        correctParams = false;
        break;
      }
      this->highShelfFOCoEffs(sampleRateFloat, fValue.value(),
                              gainValue.value());
      break;
    case Type::Lowshelf:
      // check if config has slope key
      if (slopeValue.has_value()) {
        if (!fValue.has_value() || !gainValue.has_value()) {
          correctParams = false;
          break;
        }
        this->lowShelfCoEffsSlope(sampleRateFloat, fValue.value(),
                                  gainValue.value(), slopeValue.value());
      } else {
        if (!fValue.has_value() || !gainValue.has_value() ||
            !qValue.has_value()) {
          correctParams = false;
          break;
        }
        this->lowShelfCoEffs(sampleRateFloat, fValue.value(), gainValue.value(),
                             qValue.value());
      }
      break;
    case Type::LowshelfFO:
      if (!fValue.has_value() || !gainValue.has_value()) {
        correctParams = false;
        break;
      }
      this->lowShelfFOCoEffs(sampleRateFloat, fValue.value(),
                             gainValue.value());
      break;
    case Type::Peaking:
      // check if config has bandwidth key
      if (bandwidthValue.has_value()) {
        if (!fValue.has_value() || !gainValue.has_value()) {
          correctParams = false;

          break;
        }
        this->peakCoEffsBandwidth(sampleRateFloat, fValue.value(),
                                  gainValue.value(), bandwidthValue.value());
      } else {
        if (!fValue.has_value() || !gainValue.has_value() ||
            !qValue.has_value()) {
          correctParams = false;
          break;
        }
        this->peakCoEffs(sampleRateFloat, fValue.value(), gainValue.value(),
                         qValue.value());
      }
      break;
    case Type::Notch:
      // check if config has bandwidth key
      if (bandwidthValue.has_value()) {
        if (!fValue.has_value()) {
          correctParams = false;
          break;
        }
        this->notchCoEffsBandwidth(sampleRateFloat, fValue.value(),
                                   bandwidthValue.value());
      } else {
        if (!fValue.has_value() || !qValue.has_value()) {
          correctParams = false;
          break;
        }
        this->notchCoEffs(sampleRateFloat, fValue.value(), qValue.value());
      }
      break;
    case Type::Bandpass:
      // check if config has bandwidth key
      if (bandwidthValue.has_value()) {
        if (!fValue.has_value()) {
          correctParams = false;
          break;
        }
        this->bandPassCoEffsBandwidth(sampleRateFloat, fValue.value(),
                                      bandwidthValue.value());
      } else {
        if (!fValue.has_value() || !qValue.has_value()) {
          correctParams = false;
          break;
        }
        this->bandPassCoEffs(sampleRateFloat, fValue.value(), qValue.value());
      }
      break;
    case Type::Allpass:
      // check if config has bandwidth key
      if (bandwidthValue.has_value()) {
        if (!fValue.has_value()) {
          correctParams = false;
          break;
        }
        this->allPassCoEffsBandwidth(sampleRateFloat, fValue.value(),
                                     bandwidthValue.value());
      } else {
        if (!fValue.has_value() || !qValue.has_value()) {
          correctParams = false;
          break;
        }
        this->allPassCoEffs(sampleRateFloat, fValue.value(), qValue.value());
      }
      break;
    case Type::AllpassFO:
      if (!fValue.has_value()) {
        correctParams = false;
        break;
      }
      this->allPassFOCoEffs(sampleRateFloat, fValue.value());
      break;
    case Type::Free:
      // No need to calculate coefficients, as they are pre-assigned
      break;
  }

  if (!correctParams) {
    throw std::invalid_argument("Invalid parameters for filter type");
  }

  // Convert the coefficients to IQ28 format
  std::array<int32_t, 5> coeffs = {
      _IQ28(a1Value.value()), _IQ28(a2Value.value()), _IQ28(b0Value.value()),
      _IQ28(b1Value.value()), _IQ28(b2Value.value()),
  };

  // Ensure the coefficients are in correct range
  for (auto& coeff : coeffs) {
    if (coeff < _IQ28(-8.0F)) {
      coeff = _IQ28(-8.0F);
      BELL_LOG(warn, LOG_TAG, "Coefficient out of range, clamping to -8.0");
    } else if (coeff > _IQ28(7.99999999)) {
      coeff = _IQ28(7.99999999);
      BELL_LOG(warn, LOG_TAG,
               "Coefficient out of range, clamping to 7.99999999");
    }
  }

  return coeffs;
}

// coefficients for a high pass biquad filter
void BiquadParameters::highPassCoEffs(float sampleRate, float f, float q) {
  float w0 = 2.0F * floatPi * f / sampleRate;
  float c = cosf(w0);
  float s = sinf(w0);
  float alpha = s / (2 * q);

  float b0 = (1 + c) / 2;
  float b1 = -(1 + c);
  float b2 = b0;
  float a0 = 1 + alpha;
  float a1 = -2 * c;
  float a2 = 1 - alpha;

  this->normalizeCoEffs(a0, a1, a2, b0, b1, b2);
}

// coefficients for a high pass first order biquad filter
void BiquadParameters::highPassFOCoEffs(float sampleRate, float f) {
  float w0 = 2 * floatPi * f / sampleRate;
  float k = tanf(w0 / 2.0F);

  float alpha = 1.0F + k;

  float b0 = 1.0F / alpha;
  float b1 = -1.0F / alpha;
  float b2 = 0.0F;
  float a0 = 1.0F;
  float a1 = -(1.0F - k) / alpha;
  float a2 = 0.0F;

  this->normalizeCoEffs(a0, a1, a2, b0, b1, b2);
}

// coefficients for a low pass biquad filter
void BiquadParameters::lowPassCoEffs(float sampleRate, float f, float q) {
  float w0 = 2 * floatPi * f / sampleRate;
  float c = cosf(w0);
  float s = sinf(w0);
  float alpha = s / (2 * q);

  float b0 = (1 - c) / 2;
  float b1 = 1 - c;
  float b2 = b0;
  float a0 = 1 + alpha;
  float a1 = -2 * c;
  float a2 = 1 - alpha;

  this->normalizeCoEffs(a0, a1, a2, b0, b1, b2);
}

// coefficients for a low pass first order biquad filter
void BiquadParameters::lowPassFOCoEffs(float sampleRate, float f) {
  float w0 = 2 * floatPi * f / sampleRate;
  float k = tanf(w0 / 2.0F);

  float alpha = 1.0F + k;

  float b0 = k / alpha;
  float b1 = k / alpha;
  float b2 = 0.0F;
  float a0 = 1.0F;
  float a1 = -(1.0F - k) / alpha;
  float a2 = 0.0F;

  this->normalizeCoEffs(a0, a1, a2, b0, b1, b2);
}

// coefficients for a peak biquad filter
void BiquadParameters::peakCoEffs(float sampleRate, float f, float gain,
                                  float q) {
  float w0 = 2 * floatPi * f / sampleRate;
  float c = cosf(w0);
  float s = sinf(w0);
  float alpha = s / (2 * q);

  float ampl = powf(10.0F, gain / 40.0F);
  float b0 = 1.0F + (alpha * ampl);
  float b1 = -2.0F * c;
  float b2 = 1.0F - (alpha * ampl);
  float a0 = 1 + (alpha / ampl);
  float a1 = -2 * c;
  float a2 = 1 - (alpha / ampl);

  this->normalizeCoEffs(a0, a1, a2, b0, b1, b2);
}
void BiquadParameters::peakCoEffsBandwidth(float sampleRate, float f,
                                           float gain, float bandwidth) {
  float w0 = 2 * floatPi * f / sampleRate;
  float c = cosf(w0);
  float s = sinf(w0);
  float alpha = s * sinh(logf(2.0F) / 2.0F * bandwidth * w0 / s);

  float ampl = powf(10.0F, gain / 40.0F);
  float b0 = 1.0F + (alpha * ampl);
  float b1 = -2.0F * c;
  float b2 = 1.0F - (alpha * ampl);
  float a0 = 1 + (alpha / ampl);
  float a1 = -2 * c;
  float a2 = 1 - (alpha / ampl);

  this->normalizeCoEffs(a0, a1, a2, b0, b1, b2);
}

void BiquadParameters::highShelfCoEffs(float sampleRate, float f, float gain,
                                       float q) {
  float A = powf(10.0F, gain / 40.0F);
  float w0 = 2 * floatPi * f / sampleRate;
  float c = cosf(w0);
  float s = sinf(w0);
  float beta = s * sqrtf(A) / q;
  float b0 = A * ((A + 1.0F) + (A - 1.0F) * c + beta);
  float b1 = -2.0F * A * ((A - 1.0F) + (A + 1.0F) * c);
  float b2 = A * ((A + 1.0F) + (A - 1.0F) * c - beta);
  float a0 = (A + 1.0F) - (A - 1.0F) * c + beta;
  float a1 = 2.0F * ((A - 1.0F) - (A + 1.0F) * c);
  float a2 = (A + 1.0F) - (A - 1.0F) * c - beta;

  this->normalizeCoEffs(a0, a1, a2, b0, b1, b2);
}
void BiquadParameters::highShelfCoEffsSlope(float sampleRate, float f,
                                            float gain, float slope) {
  float A = powf(10.0F, gain / 40.0F);
  float w0 = 2 * floatPi * f / sampleRate;
  float c = cosf(w0);
  float s = sinf(w0);
  float alpha =
      s / 2.0F * sqrtf((A + 1.0F / A) * (1.0F / (slope / 12.0F) - 1.0F) + 2.0F);
  float beta = 2.0F * sqrtf(A) * alpha;
  float b0 = A * ((A + 1.0F) + (A - 1.0F) * c + beta);
  float b1 = -2.0F * A * ((A - 1.0F) + (A + 1.0F) * c);
  float b2 = A * ((A + 1.0F) + (A - 1.0F) * c - beta);
  float a0 = (A + 1.0F) - (A - 1.0F) * c + beta;
  float a1 = 2.0F * ((A - 1.0F) - (A + 1.0F) * c);
  float a2 = (A + 1.0F) - (A - 1.0F) * c - beta;

  this->normalizeCoEffs(a0, a1, a2, b0, b1, b2);
}
void BiquadParameters::highShelfFOCoEffs(float sampleRate, float f,
                                         float gain) {
  float A = powf(10.0F, gain / 40.0F);
  float w0 = 2 * floatPi * f / sampleRate;
  float tn = tanf(w0 / 2.0F);

  float b0 = A * tn + powf(A, 2);
  float b1 = A * tn - powf(A, 2);
  float b2 = 0.0F;
  float a0 = A * tn + 1.0F;
  float a1 = A * tn - 1.0F;
  float a2 = 0.0F;

  this->normalizeCoEffs(a0, a1, a2, b0, b1, b2);
}

void BiquadParameters::lowShelfCoEffs(float sampleRate, float f, float gain,
                                      float q) {
  float A = powf(10.0F, gain / 40.0F);
  float w0 = 2 * floatPi * f / sampleRate;
  float c = cosf(w0);
  float s = sinf(w0);
  float beta = s * sqrtf(A) / q;

  float b0 = A * ((A + 1.0F) - (A - 1.0F) * c + beta);
  float b1 = 2.0F * A * ((A - 1.0F) - (A + 1.0F) * c);
  float b2 = A * ((A + 1.0F) - (A - 1.0F) * c - beta);
  float a0 = (A + 1.0F) + (A - 1.0F) * c + beta;
  float a1 = -2.0F * ((A - 1.0F) + (A + 1.0F) * c);
  float a2 = (A + 1.0F) + (A - 1.0F) * c - beta;

  this->normalizeCoEffs(a0, a1, a2, b0, b1, b2);
}

void BiquadParameters::lowShelfCoEffsSlope(float sampleRate, float f,
                                           float gain, float slope) {
  float A = powf(10.0F, gain / 40.0F);
  float w0 = 2 * floatPi * f / sampleRate;
  float c = cosf(w0);
  float s = sinf(w0);
  float alpha =
      s / 2.0F * sqrtf((A + 1.0F / A) * (1.0F / (slope / 12.0F) - 1.0F) + 2.0F);
  float beta = 2.0F * sqrtf(A) * alpha;

  float b0 = A * ((A + 1.0F) - (A - 1.0F) * c + beta);
  float b1 = 2.0F * A * ((A - 1.0F) - (A + 1.0F) * c);
  float b2 = A * ((A + 1.0F) - (A - 1.0F) * c - beta);
  float a0 = (A + 1.0F) + (A - 1.0F) * c + beta;
  float a1 = -2.0F * ((A - 1.0F) + (A + 1.0F) * c);
  float a2 = (A + 1.0F) + (A - 1.0F) * c - beta;

  this->normalizeCoEffs(a0, a1, a2, b0, b1, b2);
}

void BiquadParameters::lowShelfFOCoEffs(float sampleRate, float f, float gain) {
  float A = powf(10.0F, gain / 40.0F);
  float w0 = 2 * floatPi * f / sampleRate;
  float tn = tanf(w0 / 2.0F);

  float b0 = powf(A, 2) * tn + A;
  float b1 = powf(A, 2) * tn - A;
  float b2 = 0.0F;
  float a0 = tn + A;
  float a1 = tn - A;
  float a2 = 0.0F;

  this->normalizeCoEffs(a0, a1, a2, b0, b1, b2);
}

void BiquadParameters::notchCoEffs(float sampleRate, float f, float q) {
  float w0 = 2 * floatPi * f / sampleRate;
  float c = cosf(w0);
  float s = sinf(w0);
  float alpha = s / (2.0F * q);

  float b0 = 1.0F;
  float b1 = -2.0F * c;
  float b2 = 1.0F;
  float a0 = 1.0F + alpha;
  float a1 = -2.0F * c;
  float a2 = 1.0F - alpha;

  this->normalizeCoEffs(a0, a1, a2, b0, b1, b2);
}
void BiquadParameters::notchCoEffsBandwidth(float sampleRate, float f,
                                            float bandwidth) {
  float w0 = 2 * floatPi * f / sampleRate;
  float c = cosf(w0);
  float s = sinf(w0);
  float alpha = s * sinhf(logf(2.0F) / 2.0F * bandwidth * w0 / s);

  float b0 = 1.0F;
  float b1 = -2.0F * c;
  float b2 = 1.0F;
  float a0 = 1.0F + alpha;
  float a1 = -2.0F * c;
  float a2 = 1.0F - alpha;

  this->normalizeCoEffs(a0, a1, a2, b0, b1, b2);
}

void BiquadParameters::bandPassCoEffs(float sampleRate, float f, float q) {
  float w0 = 2 * floatPi * f / sampleRate;
  float c = cosf(w0);
  float s = sinf(w0);
  float alpha = s / (2.0F * q);

  float b0 = alpha;
  float b1 = 0.0F;
  float b2 = -alpha;
  float a0 = 1.0F + alpha;
  float a1 = -2.0F * c;
  float a2 = 1.0F - alpha;

  this->normalizeCoEffs(a0, a1, a2, b0, b1, b2);
}

void BiquadParameters::bandPassCoEffsBandwidth(float sampleRate, float f,
                                               float bandwidth) {
  float w0 = 2 * floatPi * f / sampleRate;
  float c = cosf(w0);
  float s = sinf(w0);
  float alpha = s * sinhf(logf(2.0F) / 2.0F * bandwidth * w0 / s);

  float b0 = alpha;
  float b1 = 0.0F;
  float b2 = -alpha;
  float a0 = 1.0F + alpha;
  float a1 = -2.0F * c;
  float a2 = 1.0F - alpha;

  this->normalizeCoEffs(a0, a1, a2, b0, b1, b2);
}

void BiquadParameters::allPassCoEffs(float sampleRate, float f, float q) {
  float w0 = 2 * floatPi * f / sampleRate;
  float c = cosf(w0);
  float s = sinf(w0);
  float alpha = s / (2.0F * q);

  float b0 = 1.0F - alpha;
  float b1 = -2.0F * c;
  float b2 = 1.0F + alpha;
  float a0 = 1.0F + alpha;
  float a1 = -2.0F * c;
  float a2 = 1.0F - alpha;

  this->normalizeCoEffs(a0, a1, a2, b0, b1, b2);
}
void BiquadParameters::allPassCoEffsBandwidth(float sampleRate, float f,
                                              float bandwidth) {
  float w0 = 2.0F * floatPi * f / sampleRate;
  float c = cosf(w0);
  float s = sinf(w0);
  float alpha = s * sinhf(logf(2.0F) / 2.0F * bandwidth * w0 / s);

  float b0 = 1.0F - alpha;
  float b1 = -2.0F * c;
  float b2 = 1.0F + alpha;
  float a0 = 1.0F + alpha;
  float a1 = -2.0F * c;
  float a2 = 1.0F - alpha;

  this->normalizeCoEffs(a0, a1, a2, b0, b1, b2);
}
void BiquadParameters::allPassFOCoEffs(float sampleRate, float f) {
  float w0 = 2 * floatPi * f / sampleRate;
  float tn = tanf(w0 / 2.0F);

  float alpha = (tn + 1.0F) / (tn - 1.0F);

  float b0 = 1.0F;
  float b1 = alpha;
  float b2 = 0.0F;
  float a0 = alpha;
  float a1 = 1.0F;
  float a2 = 0.0F;

  this->normalizeCoEffs(a0, a1, a2, b0, b1, b2);
}

void BiquadParameters::normalizeCoEffs(float a0, float a1, float a2, float b0,
                                       float b1, float b2) {
  // Normalize and scale coefficients
  b0Value = (b0 / a0);
  b1Value = (b1 / a0);
  b2Value = (b2 / a0);
  a1Value = (-a1 / a0);
  a2Value = (-a2 / a0);
}

BiquadParameters::Type BiquadParameters::stringToType(const std::string& type) {
  const std::unordered_map<std::string, Type> typeMap = {
      {"free", Type::Free},
      {"highpass", Type::Highpass},
      {"lowpass", Type::Lowpass},
      {"highpass_fo", Type::HighpassFO},
      {"lowpass_fo", Type::LowpassFO},
      {"peaking", Type::Peaking},
      {"highshelf", Type::Highshelf},
      {"highshelf_fo", Type::HighshelfFO},
      {"lowshelf", Type::Lowshelf},
      {"lowshelf_fo", Type::LowshelfFO},
      {"notch", Type::Notch},
      {"bandpass", Type::Bandpass},
      {"allpass", Type::Allpass},
      {"allpass_fo", Type::AllpassFO},
  };

  if (typeMap.find(type) == typeMap.end()) {
    throw std::invalid_argument("Invalid filter type");
  }

  return typeMap.at(type);
}
