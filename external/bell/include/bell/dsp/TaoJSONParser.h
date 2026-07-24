#pragma once

#include "bell/dsp/TransformPipeline.h"
#include "tao/json/forward.hpp"

#ifndef BELL_DISABLE_TAOJSON
namespace bell::dsp {
/**
 * @brief Parses a provided tao json structure into a bell dsp pipeline. The JSON structure is supposed to be an array of transforms.
 * 
 * @param pipelineJson JSON array to be parsed
 * @return std::shared_ptr<TransformPipeline> parsed dsp pipeline
 */
std::shared_ptr<TransformPipeline> parseTaoJsonPipeline(
    const tao::json::value& pipelineJson);
}  // namespace bell::dsp
#endif