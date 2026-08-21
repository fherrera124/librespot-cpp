#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <vector>

#include "audio/LoudnessNormalisation.h"

using namespace cspot;

namespace {
std::vector<std::byte> makeHeader(size_t size, float trackGainDb,
                                  float trackPeak) {
  std::vector<std::byte> header(size, std::byte{0});
  if (size >= 152) {
    std::memcpy(header.data() + 144, &trackGainDb, sizeof(float));
    std::memcpy(header.data() + 148, &trackPeak, sizeof(float));
  }
  return header;
}
}  // namespace

TEST_CASE("computeNormalizationGain") {
  SUBCASE("matches a real captured track's values") {
    // track_gain=-8.84dB track_peak=0.994 -> factor=0.362 (10^(-8.84/20),
    // peak not the binding constraint here).
    float gain = computeNormalizationGain(-8.84f, 0.994f);
    CHECK(gain == doctest::Approx(0.362f).epsilon(0.01));
  }

  SUBCASE("clamped so the track's own peak never clips") {
    // A large positive gain would push a 0.9-peak track past full scale -
    // the peak clamp should cap it at 1/0.9 instead.
    float gain = computeNormalizationGain(20.0f, 0.9f);
    CHECK(gain == doctest::Approx(1.0f / 0.9f));
  }

  SUBCASE("zero/negative peak skips the clamp instead of dividing by it") {
    float gain = computeNormalizationGain(-6.0f, 0.0f);
    CHECK(gain == doctest::Approx(std::pow(10.0f, -6.0f / 20.0f)));
  }
}

TEST_CASE("parseNormalizationData") {
  SUBCASE("too short (151 bytes) returns nullopt") {
    auto header = makeHeader(151, -8.84f, 0.994f);
    CHECK_FALSE(parseNormalizationData(header).has_value());
  }

  SUBCASE("exactly 152 bytes parses both floats") {
    auto header = makeHeader(152, -8.84f, 0.994f);
    auto data = parseNormalizationData(header);
    REQUIRE(data.has_value());
    CHECK(data->trackGainDb == doctest::Approx(-8.84f));
    CHECK(data->trackPeak == doctest::Approx(0.994f));
  }

  SUBCASE("longer header (real probe size) still parses correctly") {
    auto header = makeHeader(256, 2.5f, 0.87f);
    auto data = parseNormalizationData(header);
    REQUIRE(data.has_value());
    CHECK(data->trackGainDb == doctest::Approx(2.5f));
    CHECK(data->trackPeak == doctest::Approx(0.87f));
  }
}

TEST_CASE("applyLoudnessGain") {
  SUBCASE("scales samples") {
    std::vector<int16_t> samples = {1000, -1000, 0};
    applyLoudnessGain(
        tcb::span<std::byte>(reinterpret_cast<std::byte*>(samples.data()),
                             samples.size() * sizeof(int16_t)),
        0.5f);
    CHECK(samples[0] == 500);
    CHECK(samples[1] == -500);
    CHECK(samples[2] == 0);
  }

  SUBCASE("saturates instead of wrapping past int16 range") {
    std::vector<int16_t> samples = {30000, -30000};
    applyLoudnessGain(
        tcb::span<std::byte>(reinterpret_cast<std::byte*>(samples.data()),
                             samples.size() * sizeof(int16_t)),
        2.0f);
    CHECK(samples[0] == 32767);
    CHECK(samples[1] == -32768);
  }
}
