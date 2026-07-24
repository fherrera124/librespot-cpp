
#ifndef BELL_DISABLE_TAOJSON
#include "bell/dsp/TaoJSONParser.h"

// Library includes
#include <tao/json.hpp>
#include <tao/json/contrib/traits.hpp>

// Bell includes
#include "bell/dsp/BiquadParameters.h"
#include "bell/dsp/BiquadTransform.h"
#include "bell/dsp/GainTransform.h"
#include "bell/dsp/MixerTransform.h"
#include "bell/dsp/TransformPipeline.h"

using namespace bell::dsp;

namespace {
std::shared_ptr<Transform> parseTransformJson(
    const tao::json::value& json,
    const std::shared_ptr<Transform>& lastTransform,
    const std::shared_ptr<TransformPipeline>& transformPipeline) {
  if (!json.is_object()) {
    return nullptr;
  }

  if (!json.find("type")) {
    return nullptr;
  }

  const auto& transformType = json.at("type").get_string_type();

  std::vector<int> channels;

  if (json.find("channels") != nullptr && json.at("channels").is_array()) {
    // Parse the channels
    channels = json.at("channels").as<std::vector<int>>();
  } else if (json.find("channel") != nullptr &&
             json.at("channel").is_number()) {
    // Parse the channel
    channels.push_back(json.at("channel").as<int>());
  } else if (transformType !=
             "mixer") {  // Mixer transform doesnt specify channels
    throw std::invalid_argument("Channels not specified for transform");
  }

  if (transformType == "biquad_combo") {
    const auto& comboType = json.at("combo_type").get_string();
    // Try to fetch the optional parameters
    std::optional<float> f = json.optional<float>("freq");
    std::optional<int> order = json.optional<int>("order");

    std::vector<BiquadParameters> params;
    if (comboType == "lr_lowpass" || comboType == "lr_highpass") {
      bool isHighpass = comboType == "lr_highpass";
      params = BiquadParameters::linkwitzRiley(
          isHighpass ? BiquadParameters::Type::Highpass
                     : BiquadParameters::Type::Lowpass,
          f.value(), order.value());
    } else if (comboType == "bw_lowpass" || comboType == "bw_highpass") {
      bool isHighpass = comboType == "bw_highpass";
      params = BiquadParameters::butterworth(
          isHighpass ? BiquadParameters::Type::Highpass
                     : BiquadParameters::Type::Lowpass,
          f.value(), order.value());
    }

    std::shared_ptr<BiquadTransform> biquad;
    for (const auto& param : params) {
      biquad = std::make_shared<BiquadTransform>();
      biquad->setChannels(channels);
      biquad->addStage(param);
      transformPipeline->addTransform(biquad);
    }

    return biquad;
  }

  if (transformType == "biquad") {
    const auto& biquadType = json.at("biquad_type").get_string();

    // Try to fetch the optional parameters
    std::optional<float> f = json.optional<float>("freq");
    std::optional<float> q = json.optional<float>("q");
    std::optional<float> gain = json.optional<float>("gain");
    std::optional<float> slope = json.optional<float>("slope");
    std::optional<float> bandwidth = json.optional<float>("bandwidth");
    std::optional<bool> cascade = json.optional<bool>("cascade");

    BiquadParameters params(BiquadParameters::stringToType(biquadType), f, q,
                            gain, slope, bandwidth);

    // Check if the previous filter is a biquad filter valid for cascading
    bool canCascade = lastTransform &&
                      lastTransform->getType() == Transform::Type::BIQUAD &&
                      lastTransform->getChannels() == channels;

    // Cascade
    if (cascade && canCascade) {
      reinterpret_cast<BiquadTransform*>(lastTransform.get())->addStage(params);
      return lastTransform;
    }

    auto biquad = std::make_shared<BiquadTransform>();
    biquad->setChannels(channels);
    biquad->addStage(params);

    transformPipeline->addTransform(biquad);

    return biquad;
  }

  // Parse the gain transform
  if (transformType == "gain") {
    auto gain = std::make_shared<GainTransform>();

    // Assign channels
    gain->setChannels(channels);

    if (json.find("gain") != nullptr && json.at("gain").is_number()) {
      gain->configure(json.at("gain").as<float>());
    } else {
      throw std::invalid_argument("Gain not specified for gain transform");
    }

    transformPipeline->addTransform(gain);

    return gain;
  }

  // Parse the gain transform
  if (transformType == "mixer") {
    auto mixer = std::make_shared<MixerTransform>();

    if (json.find("mapping") != nullptr && json.at("mapping").is_array()) {
      mixer->configure(json.at("mapping").as<std::vector<std::vector<int>>>());
    } else {
      throw std::invalid_argument("Invalid mixer mapping");
    }

    transformPipeline->addTransform(mixer);

    return mixer;
  }

  return nullptr;
}
}  // namespace

std::shared_ptr<TransformPipeline> bell::dsp::parseTaoJsonPipeline(
    const tao::json::value& pipelineJson) {
  auto pipeline = std::make_shared<TransformPipeline>();

  if (!pipelineJson.is_array()) {
    throw std::runtime_error("Pipeline needs to be a JSON array");
  }

  std::shared_ptr<Transform> lastTransform = nullptr;

  for (const auto& transformJson : pipelineJson.get_array()) {
    auto result = parseTransformJson(transformJson, lastTransform, pipeline);

    if (result != nullptr) {
      lastTransform = result;
    } else {
      throw std::runtime_error("Invalid transform in pipeline");
    }
  }

  return pipeline;
}

#endif
