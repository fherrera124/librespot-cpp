#pragma once

#include <cstddef>
#include <functional>

#include <yajl_parse.h>
#include "bell/Result.h"

#include "proto/ConnectPb.h"

namespace cspot {
struct PageMetadata {
  std::optional<uint32_t> trackCount;
  std::optional<std::string> pageUrl;
  std::optional<std::string> nextPageUrl;
};
class ContextPageParser {
 public:
  using OnTrackCallback =
      std::function<void(uint32_t, uint32_t, const cspot_proto::ContextTrack&)>;

  using OnPageMetadataCallback =
      std::function<void(uint32_t, const PageMetadata&)>;

  ContextPageParser(OnTrackCallback trackCallback,
                    OnPageMetadataCallback pageCallback);

  ~ContextPageParser();

  // Feed streaming data
  bell::Result<> feed(const std::byte* data, size_t len);

  bool finish();

  // Resets internal state
  void reset(std::optional<uint32_t> startAtPage = std::nullopt);

 private:
  int depth = 0;
  uint32_t currentPageIndex = 0;
  uint32_t trackIndexInPage = 0;
  std::string lastKey;
  PageMetadata parsedPageMetadata;
  cspot_proto::ContextTrack parsedTrack;
  enum class Level {
    ExpectKey,
    InPagesArray,
    InPageObject,
    InTracksArray,
    InTrackObject
  } level = Level::ExpectKey;

  OnTrackCallback trackCallback;
  OnPageMetadataCallback pageCallback;

  yajl_handle yajlHandle = nullptr;

  // Yajl callbacks
  static yajl_callbacks callbacks;
  static int onStartArray(void* ctx);
  static int onEndArray(void* ctx);
  static int onStartMap(void* ctx);
  static int onEndMap(void* ctx);
  static int onMapKey(void* ctx, const uint8_t* str, size_t len);
  static int onString(void* ctx, const uint8_t* str, size_t len);
};
}  // namespace cspot
