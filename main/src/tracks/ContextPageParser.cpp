#include "tracks/ContextPageParser.h"

#include <yajl_parse.h>
#include "bell/Result.h"

using namespace cspot;

namespace {
int onNull(void* /*ctx*/) {
  return 1;
}

int onBoolean(void* /*ctx*/, int /*val*/) {
  return 1;
}

int onNumber(void* /*ctx*/, const char* /*s*/, size_t /*len*/) {
  return 1;
}
}  // namespace

ContextPageParser::ContextPageParser(OnTrackCallback trackCallback,
                                     OnPageMetadataCallback pageCallback)
    : trackCallback(std::move(trackCallback)),
      pageCallback(std::move(pageCallback)) {
  callbacks = {
      onNull,      onBoolean, nullptr,   nullptr,       onNumber,    &onString,
      &onStartMap, &onMapKey, &onEndMap, &onStartArray, &onEndArray,
  };
}

ContextPageParser::~ContextPageParser() {
  (void)finish();  // Ensure the parser is freed
}

bell::Result<> ContextPageParser::feed(const std::byte* data, size_t len) {
  if (yajlHandle == nullptr) {
    yajlHandle = yajl_alloc(&callbacks, nullptr, this);
  }

  const unsigned char* buf = reinterpret_cast<const unsigned char*>(data);
  yajl_status stat = yajl_parse(yajlHandle, buf, len);
  if (stat != yajl_status_ok) {
    finish();
    return bell::make_unexpected_errc(std::errc::bad_message);
  }

  return {};
}

bool ContextPageParser::finish() {
  if (yajlHandle != nullptr) {
    yajl_status status = yajl_complete_parse(yajlHandle);
    yajl_free(yajlHandle);
    return status == yajl_status_ok;
  }

  return true;
}

void ContextPageParser::reset(std::optional<uint32_t> startAtPage) {
  depth = 0;
  level = Level::ExpectKey;
  lastKey.clear();
  parsedPageMetadata = {};
  parsedTrack = {};
  currentPageIndex = 0;
  trackIndexInPage = 0;

  if (startAtPage.has_value()) {
    currentPageIndex = *startAtPage;
    depth = 1;  // Start at page level
    level = Level::InPageObject;
  }
}

int ContextPageParser::onStartArray(void* ctx) {
  auto* parser = static_cast<ContextPageParser*>(ctx);
  if (parser->lastKey == "pages" && parser->depth == 1) {
    parser->level = Level::InPagesArray;
    parser->currentPageIndex = 0;
  } else if (parser->level == Level::InPageObject &&
             parser->lastKey == "tracks") {
    parser->level = Level::InTracksArray;
    parser->trackIndexInPage = 0;
  }
  return 1;
}

int ContextPageParser::onEndArray(void* ctx) {
  auto* parser = static_cast<ContextPageParser*>(ctx);
  if (parser->level == Level::InPagesArray) {
    parser->level = Level::ExpectKey;
  }
  if (parser->level == Level::InTracksArray) {
    parser->level = Level::InPageObject;
  }

  return 1;
}

int ContextPageParser::onStartMap(void* ctx) {
  auto* parser = static_cast<ContextPageParser*>(ctx);
  parser->depth++;
  if (parser->level == Level::InPagesArray) {
    parser->level = Level::InPageObject;
    parser->parsedPageMetadata = {};
    parser->trackIndexInPage = 0;
  } else if (parser->level == Level::InTracksArray) {
    parser->level = Level::InTrackObject;
    parser->parsedTrack = cspot_proto::ContextTrack{};
  } else if (parser->level == Level::ExpectKey && parser->lastKey == "pages") {
    parser->level = Level::InPagesArray;
  }
  return 1;
}

int ContextPageParser::onEndMap(void* ctx) {
  auto* parser = static_cast<ContextPageParser*>(ctx);
  if (parser->level == Level::InTrackObject && parser->depth == 3) {
    // We are at the end of a track object
    if (parser->trackCallback) {
      parser->parsedTrack.index.track = parser->trackIndexInPage;
      parser->parsedTrack.index.page = parser->currentPageIndex;

      parser->trackCallback(parser->currentPageIndex, parser->trackIndexInPage,
                            parser->parsedTrack);
    }
    parser->trackIndexInPage++;
    parser->level = Level::InTracksArray;
  } else if (parser->level == Level::InPageObject && parser->depth == 2) {
    // We are at the end of a page object
    if (parser->pageCallback) {
      parser->parsedPageMetadata.trackCount = parser->trackIndexInPage;
      // state->parsedPageMetadata.isValid = true;
      parser->pageCallback(parser->currentPageIndex,
                           parser->parsedPageMetadata);
    }
    parser->currentPageIndex++;
    parser->level = Level::InPagesArray;
  }

  parser->depth--;
  return 1;
}

int ContextPageParser::onMapKey(void* ctx, const uint8_t* str, size_t len) {
  auto* state = static_cast<ContextPageParser*>(ctx);
  state->lastKey = {reinterpret_cast<const char*>(str), len};
  return 1;
}

int ContextPageParser::onString(void* ctx, const uint8_t* str, size_t len) {
  auto* parser = static_cast<ContextPageParser*>(ctx);
  std::string sval(reinterpret_cast<const char*>(str), len);
  if (parser->level == Level::InPageObject) {
    if (parser->lastKey == "page_url") {
      parser->parsedPageMetadata.pageUrl = sval;
    } else if (parser->lastKey == "next_page_url") {
      parser->parsedPageMetadata.nextPageUrl = sval;
    }
  } else if (parser->level == Level::InTrackObject) {
    if (parser->lastKey == "uid") {
      parser->parsedTrack.uid = sval;
    } else if (parser->lastKey == "uri") {
      parser->parsedTrack.uri = sval;
    };
  }
  return 1;
}
