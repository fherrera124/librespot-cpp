#include "audio/SpotifySeekTable.h"

#include <array>
#include <cstring>

using namespace cspot;

namespace {
// Same 256-entry lookup table go-librespot's own vorbis/metadata.go uses
// to decode both the seek table's cumulative offsets and its initial
// negative bias - copied verbatim, the values aren't otherwise derivable.
constexpr std::array<uint16_t, 256> kSeekTableLookup = {
    0,    112,  197,  327,  374,  394,  407,  417,  425,  433,  439,  444,
    449,  454,  458,  462,  466,  470,  473,  477,  480,  483,  486,  489,
    491,  494,  497,  499,  502,  504,  506,  509,  511,  513,  515,  517,
    519,  521,  523,  525,  527,  529,  531,  533,  535,  537,  538,  540,
    542,  544,  545,  547,  549,  550,  552,  554,  555,  557,  558,  560,
    562,  563,  565,  566,  568,  569,  571,  572,  574,  575,  577,  578,
    580,  581,  583,  584,  585,  587,  588,  590,  591,  593,  594,  595,
    597,  598,  599,  601,  602,  604,  605,  606,  608,  609,  610,  612,
    613,  615,  616,  617,  619,  620,  621,  623,  624,  625,  627,  628,
    629,  631,  632,  634,  635,  636,  638,  639,  640,  642,  643,  644,
    646,  647,  649,  650,  651,  653,  654,  655,  657,  658,  660,  661,
    662,  664,  665,  667,  668,  669,  671,  672,  674,  675,  677,  678,
    679,  681,  682,  684,  685,  687,  688,  690,  691,  693,  694,  696,
    697,  699,  700,  702,  704,  705,  707,  708,  710,  712,  713,  715,
    716,  718,  720,  721,  723,  725,  727,  728,  730,  732,  734,  735,
    737,  739,  741,  743,  745,  747,  748,  750,  752,  754,  756,  758,
    760,  763,  765,  767,  769,  771,  773,  776,  778,  780,  782,  785,
    787,  790,  792,  795,  797,  800,  803,  805,  808,  811,  814,  817,
    820,  823,  826,  829,  833,  836,  840,  843,  847,  851,  855,  859,
    863,  868,  872,  877,  882,  887,  893,  898,  904,  911,  918,  925,
    933,  941,  951,  961,  972,  985,  1000, 1017, 1039, 1067, 1108, 1183,
    1520, 2658, 4666, 8191,
};

constexpr size_t kOggPageHeaderSize = 27;
constexpr uint8_t kSpotifyMetadataMarker = 0x81;
constexpr uint8_t kSeekTableSegmentType = 0;
// seekSamples(4) + seekSize(4) + offset(1) + 100 index bytes
constexpr size_t kSeekTableSegmentDataLen = 4 + 4 + 1 + 100;

uint16_t readU16LE(tcb::span<const std::byte> data, size_t offset) {
  return static_cast<uint16_t>(std::to_integer<uint8_t>(data[offset])) |
         (static_cast<uint16_t>(std::to_integer<uint8_t>(data[offset + 1]))
          << 8);
}

uint32_t readU32LE(tcb::span<const std::byte> data, size_t offset) {
  uint32_t value = 0;
  for (size_t i = 0; i < 4; i++) {
    value |= static_cast<uint32_t>(std::to_integer<uint8_t>(data[offset + i]))
             << (8 * i);
  }
  return value;
}
}  // namespace

std::optional<SpotifySeekTable> SpotifySeekTable::tryParse(
    tcb::span<const std::byte> rawHeaderBytes) {
  if (rawHeaderBytes.size() < kOggPageHeaderSize + 1) {
    return std::nullopt;
  }
  if (std::memcmp(rawHeaderBytes.data(), "OggS", 4) != 0) {
    return std::nullopt;
  }

  auto pageSegments = std::to_integer<uint8_t>(rawHeaderBytes[26]);
  if (rawHeaderBytes.size() < kOggPageHeaderSize + pageSegments) {
    return std::nullopt;
  }

  size_t packetLen = 0;
  for (size_t i = 0; i < pageSegments; i++) {
    packetLen += std::to_integer<uint8_t>(rawHeaderBytes[kOggPageHeaderSize + i]);
  }
  size_t packetStart = kOggPageHeaderSize + pageSegments;
  if (packetLen < 1 || rawHeaderBytes.size() < packetStart + packetLen) {
    return std::nullopt;
  }
  if (std::to_integer<uint8_t>(rawHeaderBytes[packetStart]) !=
      kSpotifyMetadataMarker) {
    return std::nullopt;
  }

  size_t pos = packetStart + 1;
  size_t packetEnd = packetStart + packetLen;

  uint32_t seekSamples = 0;
  uint32_t seekSize = 0;
  std::array<int32_t, 100> table{};
  bool foundSeekTable = false;

  while (pos + 2 <= packetEnd) {
    uint16_t segLen = readU16LE(rawHeaderBytes, pos);
    pos += 2;
    if (segLen < 1 || pos + segLen > packetEnd) {
      break;
    }
    auto segType = std::to_integer<uint8_t>(rawHeaderBytes[pos]);
    size_t segDataStart = pos + 1;
    size_t segDataLen = segLen - 1;
    pos += segLen;

    if (segType != kSeekTableSegmentType ||
        segDataLen != kSeekTableSegmentDataLen) {
      continue;
    }

    auto offsetIdx =
        std::to_integer<uint8_t>(rawHeaderBytes[segDataStart + 8]);
    if (offsetIdx >= kSeekTableLookup.size()) {
      continue;
    }

    int32_t cum = -static_cast<int32_t>(kSeekTableLookup[offsetIdx]);
    bool validIndices = true;
    for (size_t i = 0; i < 100; i++) {
      auto idx =
          std::to_integer<uint8_t>(rawHeaderBytes[segDataStart + 9 + i]);
      if (idx >= kSeekTableLookup.size()) {
        validIndices = false;
        break;
      }
      cum += kSeekTableLookup[idx];
      table[i] = cum;
    }
    if (!validIndices) {
      continue;
    }

    seekSamples = readU32LE(rawHeaderBytes, segDataStart);
    seekSize = readU32LE(rawHeaderBytes, segDataStart + 4);
    foundSeekTable = true;
  }

  if (!foundSeekTable || seekSamples == 0) {
    return std::nullopt;
  }

  SpotifySeekTable result;
  result.seekSamples = seekSamples;
  result.seekSize = seekSize;
  result.table = table;
  return result;
}

size_t SpotifySeekTable::getBytePosition(uint64_t samplePos) const {
  float samplesRelPos =
      static_cast<float>(samplePos) * 100.0f / static_cast<float>(seekSamples);
  int32_t samplesIntPos = static_cast<int32_t>(samplesRelPos);
  if (samplesIntPos <= 0) {
    samplesIntPos = 1;
  } else if (samplesIntPos > 99) {
    samplesIntPos = 99;
  }

  int32_t tablePrev = table[samplesIntPos - 1];
  int32_t tableCurr = table[samplesIntPos];
  float interpolatedPos =
      static_cast<float>(tableCurr - tablePrev) *
          (samplesRelPos - static_cast<float>(samplesIntPos)) +
      static_cast<float>(tablePrev);

  float bytesPos = interpolatedPos * 1.525879e-05f * static_cast<float>(seekSize);
  if (bytesPos < 0) {
    return 0;
  }
  return static_cast<size_t>(bytesPos);
}
