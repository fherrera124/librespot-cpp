#include "bell/dsp/TransformPipeline.h"

#ifndef BELL_DISABLE_TAOJSON
// Used for JSON deserialization of the transforms
#include <tao/json.hpp>
#include <tao/json/contrib/traits.hpp>
#endif

using namespace bell::dsp;

void Transform::setChannels(const std::vector<int>& channels) {
  std::scoped_lock lock(accessMutex);
  this->channels = channels;
}

void TransformPipeline::addTransform(
    const std::shared_ptr<Transform>& transform) {
  std::scoped_lock lock(accessMutex);
  transforms.push_back(transform);
}

void TransformPipeline::process(DataSlots& sampleSlots) {
  std::scoped_lock lock(accessMutex);

  // Check if the sample rate has been updated
  bool sampleRateUpdated =
      !lastSampleRate.has_value() ||
      lastSampleRate.value() != sampleSlots.sampleFormat.getSampleRate();
  lastSampleRate = sampleSlots.sampleFormat.getSampleRate();

  // Process the samples through each transform in the pipeline
  for (const auto& transform : transforms) {
    if (sampleRateUpdated) {
      // Notify the transform of the updated sample rate
      transform->sampleRateUpdated(sampleSlots.sampleFormat.getSampleRate());
    }

    transform->process(sampleSlots);
  }
}
