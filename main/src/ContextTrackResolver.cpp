#include "ContextTrackResolver.h"

#include <random>
#include <system_error>
#include <utility>
#include "bell/Result.h"
#include "bell/http/Common.h"
#include "tl/expected.hpp"
#include "yajl_parse.h"

#include "bell/Logger.h"
#include "proto/ConnectPb.h"

using namespace cspot;

namespace {

int on_null(void* /*ctx*/) {
  return 1;
}

int on_boolean(void* /*ctx*/, int /*val*/) {
  return 1;
}

int on_number(void* /*ctx*/, const char* /*s*/, size_t /*len*/) {
  return 1;
}

int on_string(void* ctx, const unsigned char* str, size_t len) {
  auto* state = static_cast<ContextTrackResolver::PageParseState*>(ctx);
  std::string sval(reinterpret_cast<const char*>(str), len);
  if (state->level ==
      ContextTrackResolver::PageParseState::Level::InPageObject) {
    if (state->lastKey == "page_url") {
      state->parsedPageMetadata.pageUrl = sval;
    } else if (state->lastKey == "next_page_url") {
      state->parsedPageMetadata.nextPageUrl = sval;
    }
  } else if (state->level ==
             ContextTrackResolver::PageParseState::Level::InTrackObject) {
    if (state->lastKey == "uid") {
      state->parsedTrack.uid = sval;
    } else if (state->lastKey == "uri") {
      state->parsedTrack.uri = sval;
    };
  }

  return 1;
}

int on_map_key(void* ctx, const unsigned char* key, size_t len) {
  auto* state = static_cast<ContextTrackResolver::PageParseState*>(ctx);
  state->lastKey = {reinterpret_cast<const char*>(key), len};
  return 1;
}

int on_start_map(void* ctx) {
  auto* state = static_cast<ContextTrackResolver::PageParseState*>(ctx);
  state->depth++;
  if (state->level == ContextTrackResolver::PageParseState::InPagesArray) {
    state->level = ContextTrackResolver::PageParseState::InPageObject;
    state->parsedPageMetadata = {};
    state->trackIndexInPage = 0;
  } else if (state->level ==
             ContextTrackResolver::PageParseState::InTracksArray) {
    state->level = ContextTrackResolver::PageParseState::InTrackObject;
    state->parsedTrack = cspot_proto::ContextTrack{};
  } else if (state->level == ContextTrackResolver::PageParseState::ExpectKey &&
             state->lastKey == "pages") {
    state->level = ContextTrackResolver::PageParseState::InPagesArray;
  }
  return 1;
}

int on_end_map(void* ctx) {
  auto* state = static_cast<ContextTrackResolver::PageParseState*>(ctx);
  if (state->level == ContextTrackResolver::PageParseState::InTrackObject &&
      state->depth == 3) {
    // We are at the end of a track object
    if (state->onTrackResolved) {
      state->parsedTrack.index.track = state->trackIndexInPage;
      state->parsedTrack.index.page = state->currentPageIndex;

      state->onTrackResolved(state->currentPageIndex, state->trackIndexInPage,
                             state->parsedTrack);
    }
    state->trackIndexInPage++;
    state->level = ContextTrackResolver::PageParseState::InTracksArray;
  } else if (state->level ==
                 ContextTrackResolver::PageParseState::InPageObject &&
             state->depth == 2) {
    // We are at the end of a page object
    if (state->onPageMetadataResolved) {
      state->parsedPageMetadata.trackCount = state->trackIndexInPage;
      state->parsedPageMetadata.isValid = true;
      state->onPageMetadataResolved(state->currentPageIndex,
                                    state->parsedPageMetadata);
    }
    state->currentPageIndex++;
    state->level = ContextTrackResolver::PageParseState::InPagesArray;
  }

  state->depth--;
  return 1;
}

int on_start_array(void* ctx) {
  auto* state = static_cast<ContextTrackResolver::PageParseState*>(ctx);
  if (state->lastKey == "pages" && state->depth == 1) {
    state->level = ContextTrackResolver::PageParseState::InPagesArray;
    state->currentPageIndex = 0;
  } else if (state->level ==
                 ContextTrackResolver::PageParseState::InPageObject &&
             state->lastKey == "tracks") {
    state->level = ContextTrackResolver::PageParseState::InTracksArray;
    state->trackIndexInPage = 0;
  }
  return 1;
}

int on_end_array(void* ctx) {
  auto* state = static_cast<ContextTrackResolver::PageParseState*>(ctx);
  if (state->level == ContextTrackResolver::PageParseState::InPagesArray) {
    state->level = ContextTrackResolver::PageParseState::ExpectKey;
  }
  if (state->level == ContextTrackResolver::PageParseState::InTracksArray) {
    state->level = ContextTrackResolver::PageParseState::InPageObject;
  }
  return 1;
}

yajl_callbacks yajlCallbacks = {
    on_null,    on_boolean,     nullptr,      nullptr,
    on_number,  on_string,      on_start_map, on_map_key,
    on_end_map, on_start_array, on_end_array,
};

// RNG for shuffling
std::random_device randomDevice;
std::mt19937 randomGenerator(randomDevice());
}  // namespace

ContextTrackResolver::ContextTrackResolver(std::shared_ptr<SpClient> spClient,
                                           uint32_t maxWindowSize,
                                           uint32_t trackUpdateThreshold)
    : spClient(std::move(spClient)),
      maxWindowSize(maxWindowSize),
      trackUpdateThreshold(trackUpdateThreshold) {}

void ContextTrackResolver::updateContext(
    const std::string& rootContextUrl,
    std::optional<std::string>& currentTrackUid,
    std::optional<std::string>& currentTrackUri) {
  this->rootContextUrl = rootContextUrl.substr(10);  // remove context://
  this->currentTrackId.uid = currentTrackUid;
  this->currentTrackId.uri = currentTrackUri;

  pageParseState.yajlCallbacks = yajlCallbacks;
  pageParseState.onTrackResolved = [this](
                                       uint32_t pageIndex, uint32_t trackIndex,
                                       const cspot_proto::ContextTrack& track) {
    this->onTrackParsed(pageIndex, trackIndex, track);
  };

  pageParseState.onPageMetadataResolved = [this](uint32_t pageIndex,
                                                 const PageMetadata& metadata) {
    this->onPageMetadataParsed(pageIndex, metadata);
  };
}

bell::Result<cspot_proto::ContextTrack>
ContextTrackResolver::getCurrentTrack() {
  auto res = ensureContextTracks();
  if (!res) {
    BELL_LOG(error, LOG_TAG, "Failed to ensure context tracks: {}",
             res.error());
    return tl::make_unexpected(res.error());
  }

  auto currentTrackInQueueIndex = getCurrentTrackInQueueIndex();
  if (!currentTrackInQueueIndex.has_value()) {
    BELL_LOG(error, LOG_TAG, "Current track not found in the queue");
    return bell::make_unexpected_errc<cspot_proto::ContextTrack>(
        std::errc::no_message_available);
  }

  return contextTrackQueue[currentTrackInQueueIndex.value()];
}

tcb::span<cspot_proto::ContextTrack> ContextTrackResolver::previousTracks() {
  auto currentTrackInQueueIdx = getCurrentTrackInQueueIndex();
  if (!currentTrackInQueueIdx.has_value()) {
    return {};
  }
  return {contextTrackQueue.data(), currentTrackInQueueIdx.value()};
}

tcb::span<cspot_proto::ContextTrack> ContextTrackResolver::nextTracks() {
  auto currentTrackInQueueIdx = getCurrentTrackInQueueIndex();

  if (!currentTrackInQueueIdx.has_value()) {
    return {};
  }
  return {contextTrackQueue.data() + currentTrackInQueueIdx.value() + 1,
          contextTrackQueue.size() - currentTrackInQueueIdx.value() - 1};
}

bell::Result<cspot_proto::ContextTrack> ContextTrackResolver::next() {
  auto res = ensureContextTracks();
  if (!res) {
    BELL_LOG(error, LOG_TAG, "Failed to ensure context tracks: {}",
             res.error());
    return tl::make_unexpected(res.error());
  }

  auto currentTrackInCacheIndex = getCurrentTrackInQueueIndex();
  if (!currentTrackInCacheIndex.has_value()) {
    BELL_LOG(error, LOG_TAG, "Current track not found in the queue");
    return bell::make_unexpected_errc<cspot_proto::ContextTrack>(
        std::errc::no_message_available);
  }

  if (currentTrackInCacheIndex.value() + 1 >= contextTrackQueue.size()) {
    BELL_LOG(error, LOG_TAG, "No next track available");
    return bell::make_unexpected_errc<cspot_proto::ContextTrack>(
        std::errc::no_message_available);
  }

  if (currentTrackInCacheIndex.value() >= (maxWindowSize - 1) / 2) {
    // Erase oldest track
    contextTrackQueue.erase(contextTrackQueue.begin());

    // Increase lowest window index
    lowestWindowIndex.track++;
    if (lowestWindowIndex.track >=
        pageMetadata[lowestWindowIndex.page].trackCount) {
      lowestWindowIndex.track = 0;
      lowestWindowIndex.page++;
    }
    currentTrack = contextTrackQueue[currentTrackInCacheIndex.value()];
  } else {
    currentTrack = contextTrackQueue[currentTrackInCacheIndex.value() + 1];
  }

  BELL_LOG(info, LOG_TAG,
           "Lowest window - {},{} highest window - {},{} tracks in queue - {}",
           lowestWindowIndex.page, lowestWindowIndex.track,
           highestWindowIndex.page, highestWindowIndex.track,
           contextTrackQueue.size());
  for (auto& track : contextTrackQueue) {
    BELL_LOG(info, LOG_TAG, "- [{},{}] URI - {}, isCurrent={}",
             track.index.page, track.index.track, track.uri,
             currentTrack == track);
  }

  return contextTrackQueue[currentTrackInCacheIndex.value()];
}

bell::Result<cspot_proto::ContextTrack> ContextTrackResolver::previous() {
  auto res = ensureContextTracks();
  if (!res) {
    BELL_LOG(error, LOG_TAG, "Failed to ensure context tracks: {}",
             res.error());
    return tl::make_unexpected(res.error());
  }

  auto currentTrackInCacheIndex = getCurrentTrackInQueueIndex();
  if (!currentTrackInCacheIndex.has_value()) {
    BELL_LOG(error, LOG_TAG, "Current track not found in the queue");
    return bell::make_unexpected_errc<cspot_proto::ContextTrack>(
        std::errc::no_message_available);
  }

  if (currentTrackInCacheIndex.value() == 0) {
    BELL_LOG(error, LOG_TAG, "No previous track available");
    return bell::make_unexpected_errc<cspot_proto::ContextTrack>(
        std::errc::no_message_available);
  }

  currentTrack = contextTrackQueue[currentTrackInCacheIndex.value() - 1];
  if ((contextTrackQueue.size() - currentTrackInCacheIndex.value()) >
      (maxWindowSize - 1) / 2) {
    // Erase newest track
    contextTrackQueue.pop_back();

    // Move highest window index to the previous track
    if (highestWindowIndex.track == 0) {
      highestWindowIndex.page--;
      highestWindowIndex.track =
          pageMetadata[highestWindowIndex.page].trackCount - 1;
    } else {
      highestWindowIndex.track--;
    }
  }

  BELL_LOG(info, LOG_TAG,
           "Lowest window - {},{} highest window - {},{} tracks in queue - {}",
           lowestWindowIndex.page, lowestWindowIndex.track,
           highestWindowIndex.page, highestWindowIndex.track,
           contextTrackQueue.size());
  for (auto& track : contextTrackQueue) {
    BELL_LOG(info, LOG_TAG, "- [{},{}] UID - {}, isCurrent={}",
             track.index.page, track.index.track, track.uid,
             currentTrack == track);
  }

  return {};
}

bell::Result<> ContextTrackResolver::ensureContextTracks() {
  if (pageMetadata.empty()) {
    BELL_LOG(info, LOG_TAG, "No page metadata, resolving root context");
    this->isSlidingWindow = true;  // Reset sliding window mode
    currentTrack = std::nullopt;
    currentPageIndex = 0;

    auto res = resolveRootContext();
    if (!res) {
      BELL_LOG(error, LOG_TAG, "Failed to resolve root context: {}",
               res.error());
      return tl::make_unexpected(res.error());
    }

    return ensureContextTracks();
  }

  // Did not find current track in the queue yet, even after resolving root context
  // The target index is most likely in another page
  if (!currentTrack.has_value()) {
    // No current track found yet
    if (pageMetadata[currentPageIndex].pageUrl.has_value()) {
      auto res = resolveContextPage(currentPageIndex);
      if (!res) {
        BELL_LOG(error, LOG_TAG, "Failed to resolve context page: {}",
                 res.error());
        return tl::make_unexpected(res.error());
      }

      return ensureContextTracks();
    }

    // No page URL for the current page, we are most likely at the root page
    if (pageMetadata[currentPageIndex].isValid) {
      auto res = resolveRootContext();
      if (!res) {
        BELL_LOG(error, LOG_TAG, "Failed to resolve root context: {}",
                 res.error());
        return tl::make_unexpected(res.error());
      }

      return ensureContextTracks();
    }
  }

  auto currentTrackInQueueIndex = getCurrentTrackInQueueIndex();
  if (currentTrackInQueueIndex.has_value()) {
    if ((contextTrackQueue.size() - currentTrackInQueueIndex.value() - 1) <=
        trackUpdateThreshold) {
      BELL_LOG(info, LOG_TAG, "Not enough next tracks, setting advance window");
      setAdvanceWindow();
    } else if ((currentTrackInQueueIndex < trackUpdateThreshold) &&
               (lowestWindowIndex.page > 0 || lowestWindowIndex.track > 0)) {
      BELL_LOG(info, LOG_TAG,
               "Not enough previous tracks, setting retreat window, {},{}",
               lowestWindowIndex.page, lowestWindowIndex.track);

      setRetreatWindow();
    } else {
      return {};
    }

    if (!pageMetadata[currentPageIndex].isValid) {
      return {};  // Most likely at end of context. TODO: Autoplay
    }

    if (pageMetadata[currentPageIndex].pageUrl.has_value()) {
      auto res = resolveContextPage(currentPageIndex);
      if (!res) {
        BELL_LOG(error, LOG_TAG, "Failed to resolve context page: {}",
                 res.error());
        return tl::make_unexpected(res.error());
      }
    } else {
      // No page URL for the current page, we are most likely at the root page
      auto res = resolveRootContext();
      if (!res) {
        BELL_LOG(error, LOG_TAG, "Failed to resolve root context: {}",
                 res.error());
        return tl::make_unexpected(res.error());
      }
    }
  }

  return {};
}

bell::Result<> ContextTrackResolver::setShuffle(bool shuffle) {
  if (!isShuffle && shuffle) {
    BELL_LOG(info, LOG_TAG, "Enabling shuffle mode");
    isShuffle = true;

    currentPageIndex = currentTrack.value().index.page;

    // Shuffle all pages
    for (uint32_t idx = 0; idx < pageWindows.size(); idx++) {
      (void)shuffleIdsInPage(idx);
    }

    // Manually set a window around the current track
    if (currentTrack.value().index.track < (maxWindowSize - 1 / 2)) {
      pageWindows[currentPageIndex].start = 0;
      pageWindows[currentPageIndex].size =
          std::min(maxWindowSize, pageMetadata[currentPageIndex].trackCount);
    } else {
      pageWindows[currentPageIndex].start =
          currentTrack.value().index.track - (maxWindowSize - 1 / 2);
      pageWindows[currentPageIndex].size =
          std::min(maxWindowSize, pageMetadata[currentPageIndex].trackCount -
                                      pageWindows[currentPageIndex].start);
    }

    // Reset the context track queue
    contextTrackQueue = {};

    lowestWindowIndex = currentTrack.value().index;
    highestWindowIndex = currentTrack.value().index;

    // Update the lowest and highest window indexes
    lowestWindowIndex.track = pageWindows[currentPageIndex].start;
    highestWindowIndex.track = pageWindows[currentPageIndex].start +
                               pageWindows[currentPageIndex].size - 1;
    currentTrack.reset();

    currentTrackWindow.resize(pageWindows[currentPageIndex].size);

    return ensureContextTracks();
  }

  if (isShuffle && !shuffle) {
    BELL_LOG(info, LOG_TAG, "Disabling shuffle mode");
    isShuffle = false;

    for (auto& pageWindow : pageWindows) {
      pageWindow.shuffleIndexes.clear();
    }

    // Reset the context track queue
    contextTrackQueue = {currentTrack.value()};
    lowestWindowIndex = currentTrack.value().index;
    highestWindowIndex = currentTrack.value().index;

    return ensureContextTracks();
  }

  return {};
}

bell::Result<> ContextTrackResolver::shuffleIdsInPage(uint32_t pageIdx) {
  if (pageMetadata.size() <= pageIdx) {
    // No such page
    return bell::make_unexpected_errc(std::errc::invalid_argument);
  }

  if (pageMetadata[pageIdx].trackCount == 0) {
    // No track count in page
    return bell::make_unexpected_errc(std::errc::invalid_argument);
  }

  // Prepare shuffle indexes
  pageWindows[pageIdx].shuffleIndexes.resize(pageMetadata[pageIdx].trackCount);

  for (uint32_t i = 0; i < pageMetadata[pageIdx].trackCount; i++) {
    pageWindows[pageIdx].shuffleIndexes[i] = i;
  }

  bool shufflingCurrentPage =
      currentTrack.has_value() && currentTrack.value().index.page == pageIdx;

  if (shufflingCurrentPage) {
    // Swap 0-index and the current track when shuffling a currently played back page
    std::swap(
        pageWindows[pageIdx].shuffleIndexes[0],
        pageWindows[pageIdx].shuffleIndexes[currentTrack.value().index.track]);
  }

  // We want to offset shuffle by 1, in case the current track is to be inserted at beggining
  std::shuffle(pageWindows[pageIdx].shuffleIndexes.begin() +
                   (shufflingCurrentPage ? 1 : 0),
               pageWindows[pageIdx].shuffleIndexes.end(), randomGenerator);

  return {};
}

void ContextTrackResolver::setAdvanceWindow() {
  BELL_LOG(info, LOG_TAG, "Setting advance window");
  if (isSlidingWindow) {
    // We are in sliding window mode, aka no current track found yet. Always advance
    currentPageIndex++;
    return;
  }

  currentPageIndex = highestWindowIndex.page;
  if (highestWindowIndex.track + 1 >=
      pageMetadata[currentPageIndex].trackCount) {
    if (pageMetadata.size() <= currentPageIndex) {
      BELL_LOG(info, LOG_TAG,
               "Reached the end of the context pages, no more tracks to "
               "advance to. {}",
               currentPageIndex);
      return;
    }
    // We are at the end of the page, advance to the next page
    currentPageIndex++;

    // Insert a new window, even if it might not exist
    if (pageWindows.size() < currentPageIndex + 1) {
      pageWindows.resize(currentPageIndex + 1);
    }

    pageWindows[currentPageIndex].start = 0;
    pageWindows[currentPageIndex].size = maxWindowSize;
  } else {
    pageWindows[currentPageIndex].start =
        highestWindowIndex.track + 1;  // Advance to the next track
  }

  pageWindows[currentPageIndex].size =
      maxWindowSize;  // Reset the window size to max

  if (pageMetadata[currentPageIndex].trackCount > 0) {
    pageWindows[currentPageIndex].size =
        std::min(maxWindowSize, pageMetadata[currentPageIndex].trackCount);
  }

  currentTrackWindow.resize(pageWindows[currentPageIndex].size);

  BELL_LOG(info, LOG_TAG, "page window - {},{}, {}",
           pageWindows[currentPageIndex].start,
           pageWindows[currentPageIndex].size, currentPageIndex);
}

void ContextTrackResolver::setRetreatWindow() {
  if (lowestWindowIndex.page == 0 && lowestWindowIndex.track == 0) {
    // We are at the root page, no retreat possible
    return;
  }

  currentPageIndex = lowestWindowIndex.page;
  if (lowestWindowIndex.track == 0) {
    // We are at the start of the page, retreat to the previous page
    currentPageIndex--;
    pageWindows[currentPageIndex].start =
        std::max(0U, pageMetadata[currentPageIndex].trackCount - maxWindowSize);
    pageWindows[currentPageIndex].size =
        std::min(maxWindowSize, pageMetadata[currentPageIndex].trackCount -
                                    pageWindows[currentPageIndex].start);
    ;
  } else {
    uint32_t windowSize = lowestWindowIndex.track > maxWindowSize
                              ? maxWindowSize
                              : lowestWindowIndex.track;
    pageWindows[currentPageIndex].size = windowSize;
    pageWindows[currentPageIndex].start = lowestWindowIndex.track - windowSize;
  }

  currentTrackWindow.resize(pageWindows[currentPageIndex].size);
}

bell::Result<> ContextTrackResolver::resolveRootContext() {
  BELL_LOG(info, LOG_TAG, "Resolving root context: {}", rootContextUrl);
  auto response = spClient->contextResolve(rootContextUrl);
  if (!response) {
    BELL_LOG(error, LOG_TAG, "Failed to resolve root context: {}",
             response.error());
    return tl::make_unexpected(response.error());
  }

  auto dataVector = *response->bytes();

  // Prepare for root context parsing
  resetParseState();
  pageParseState.currentPageIndex = 0;

  yajl_handle handle = yajl_alloc(&yajlCallbacks, nullptr, &pageParseState);

  const unsigned char* buf =
      reinterpret_cast<const unsigned char*>(dataVector.data());
  yajl_status stat = yajl_parse(handle, buf, dataVector.size());
  if (stat != yajl_status_ok) {
    unsigned char* err_msg = yajl_get_error(handle, 1, buf, dataVector.size());
    BELL_LOG(error, LOG_TAG, "YAJL parse error");
    yajl_free_error(handle, err_msg);
    yajl_free(handle);
    return {};
  }

  stat = yajl_complete_parse(handle);
  if (stat != yajl_status_ok) {
    BELL_LOG(error, LOG_TAG, "YAJL complete parse error");
    yajl_free(handle);
  } else {
    BELL_LOG(info, LOG_TAG, "YAJL complete parse finished successfully");
  }

  yajl_free(handle);

  if ((pageMetadata.size() >= 2) &&
      std::prev(pageMetadata.end(), 2)->nextPageUrl.has_value()) {
    // If the last page has a next page URL, add it to the metadata
    pageMetadata.back().pageUrl = std::prev(pageMetadata.end(), 2)->nextPageUrl;
    pageMetadata.back().isValid = true;
  }

  return {};
}

bell::Result<> ContextTrackResolver::resolveContextPage(uint32_t pageIndex) {
  /*
  if (pageMetadata.size() < pageIndex + 1) {
    BELL_LOG(error, LOG_TAG,
             "Page index {} is out of bounds for page metadata size {}",
             pageIndex, pageMetadata.size());
    return {};
  }

  if (!pageMetadata[pageIndex].pageUrl.has_value()) {
    BELL_LOG(error, LOG_TAG,
             "Page metadata for index {} does not have a page URL. Most likely "
             "it is a root page.",
             pageIndex);
    return {};
  }
  auto& pageUrl = pageMetadata[pageIndex].pageUrl.value();

  // Fresh page requires two runs in shuffle mode, first to fetch track count, and another one to process the shuffled window
  bool requireRefetch = isShuffle && pageMetadata[pageIndex].trackCount == 0;

  BELL_LOG(info, LOG_TAG, "Resolving context page {}", pageUrl);
  auto reader = spClient->doRequest(bell::http::Method::GET, pageUrl.substr(5));
  if (!reader) {
    BELL_LOG(error, LOG_TAG, "Failed to resolve root context: {}",
             reader.error());
    return tl::make_unexpected(reader.error());
  }

  auto dataVector = *reader->getBodyStringView();

  resetParseState();
  pageParseState.currentPageIndex = pageIndex;
  pageParseState.depth = 1;  // Start at page level
  pageParseState.level = ContextTrackResolver::PageParseState::InPageObject;
  yajl_handle handle = yajl_alloc(&yajlCallbacks, nullptr, &pageParseState);

  const unsigned char* buf =
      reinterpret_cast<const unsigned char*>(dataVector.data());
  yajl_status stat = yajl_parse(handle, buf, dataVector.size());
  if (stat != yajl_status_ok) {
    unsigned char* err_msg = yajl_get_error(handle, 1, buf, dataVector.size());
    BELL_LOG(error, LOG_TAG, "YAJL parse error");
    yajl_free_error(handle, err_msg);
    yajl_free(handle);
    return {};
  }

  stat = yajl_complete_parse(handle);
  if (stat != yajl_status_ok) {
    BELL_LOG(error, LOG_TAG, "YAJL complete parse error");
    yajl_free(handle);
  } else {
    BELL_LOG(info, LOG_TAG, "YAJL complete parse finished successfully");
  }

  yajl_free(handle);

  if (!pageMetadata.empty() && pageMetadata.back().nextPageUrl.has_value()) {
    pageMetadata.push_back({
        .pageUrl = pageMetadata.back().nextPageUrl.value(),
        .nextPageUrl = std::nullopt,
        .trackCount = 0,
    });
  }

  if (requireRefetch) {
    // Ensure we have shuffled ids
    (void)shuffleIdsInPage(pageIndex);

    BELL_LOG(info, LOG_TAG,
             "Fetching page for the second time, due to the required shuffle "
             "window");
    return resolveContextPage(pageIndex);
  }
*/
  return {};
}

void ContextTrackResolver::onPageMetadataParsed(
    uint32_t pageIndex, const ContextTrackResolver::PageMetadata& metadata) {
  // Insert new page metadata if it does not exist. We do it N+1, to assume that a new page is coming
  if (pageMetadata.size() < pageIndex + 2) {
    pageMetadata.resize(pageIndex + 2);
    pageWindows.resize(pageIndex + 2);
    pageMetadata[pageIndex] = metadata;
  }

  if (metadata.trackCount > 0) {
    bool needsRefetch = pageMetadata[pageIndex].trackCount == 0 && isShuffle;
    // Update the track count for the page
    pageMetadata[pageIndex].trackCount = metadata.trackCount;

    if (needsRefetch) {
      // When track count is first fetched in shuffle index mode, we need to refetch the entire page to properly handle track window
      return;
    }
  }

  if (currentPageIndex != pageIndex) {
    return;
  }

  BELL_LOG(info, LOG_TAG, "Track count {} - idx {}", metadata.trackCount,
           pageIndex);
  if (metadata.trackCount > 0 && pageWindows.size() > pageIndex) {
    // Crop the page window size to max track count
    if ((pageWindows[pageIndex].start + pageWindows[pageIndex].size) >
        pageMetadata[pageIndex].trackCount) {
      pageWindows[pageIndex].size =
          pageMetadata[pageIndex].trackCount - pageWindows[pageIndex].start;
      currentTrackWindow.resize(pageWindows[pageIndex].size);
    }

    if (currentPageIndex == pageIndex) {
      // Update the current track window
      updateTracksFromWindow();
      setAdvanceWindow();
    }
  }
}

void ContextTrackResolver::onTrackParsed(
    uint32_t pageIndex, uint32_t trackIndex,
    const cspot_proto::ContextTrack& track) {
  if (isShuffle && pageMetadata[pageIndex].trackCount == 0) {
    // If we are in shuffle mode and the page does not have track count, we cannot
    // process the track
    return;
  }
  if (pageWindows.size() < pageIndex + 1) {
    // Insert a new page window if it does not exist
    pageWindows.resize(pageIndex + 1);
    pageWindows[pageIndex] = PageWindow{
        .size = maxWindowSize,
        .start = 0,
        .shuffleIndexes = {},
    };
    currentTrackWindow.resize(maxWindowSize);
  }

  if (currentPageIndex != pageIndex) {
    // If we are not on the current page, we do not process the track
    return;
  }

  BELL_LOG(info, LOG_TAG, "Processing track at index {} on page {}", trackIndex,
           pageIndex);

  auto& pageWindow = pageWindows[pageIndex];

  uint32_t actualIndex = trackIndex;

  if (isShuffle && pageWindow.shuffleIndexes.empty()) {
    BELL_LOG(info, LOG_TAG,
             "No shuffle indexes prepared in shuffle mode, cant do much");
    // No shuffle indexes prepared in shuffle mode, cant do much
    return;
  }

  if (isShuffle && pageWindow.shuffleIndexes.size() > pageIndex) {
    // Find the pageIndex in the shuffle indexes, and note its index in shuffleIndexes
    auto shuffleIndex = std::find(pageWindow.shuffleIndexes.begin(),
                                  pageWindow.shuffleIndexes.end(), trackIndex);
    if (shuffleIndex == pageWindow.shuffleIndexes.end()) {
      BELL_LOG(error, LOG_TAG,
               "Track index {} not found in shuffle indexes for page {}",
               trackIndex, pageIndex);
      return;  // Track index not found in shuffle indexes
    }
    actualIndex =
        std::distance(pageWindow.shuffleIndexes.begin(), shuffleIndex);
  }

  if (isSlidingWindow && (actualIndex > (pageWindow.start + pageWindow.size))) {
    // In sliding window mode, we advance the window during track parsing
    pageWindow.start++;

    // Move items in the current track window one step to the right
    for (uint32_t idx = 0; idx < pageWindow.size - 1; idx++) {
      currentTrackWindow[idx] = currentTrackWindow[idx + 1];
    }
  }

  if (actualIndex >= pageWindow.start &&
      actualIndex < pageWindow.start + pageWindow.size) {
    currentTrackWindow[actualIndex - pageWindow.start] = track;
    BELL_LOG(info, LOG_TAG, "Track in window, {}, uid-{}, uri-{}, index-{},{}",
             actualIndex - pageWindow.start, track.uid, track.uri,
             track.index.page, track.index.track);

    if (!currentTrack.has_value() && (currentTrackId == track)) {
      // If we are in sliding window mode and the target track is found,
      // we set the current track to it
      currentTrack = track;
      BELL_LOG(info, LOG_TAG, "Found current track");
      isSlidingWindow = false;  // Disable sliding window mode

      uint32_t maxPreviousTracks = (maxWindowSize - 1) / 2;
      if ((actualIndex - pageWindow.start) > maxPreviousTracks) {
        // Too many previous tracks, we need to adjust the current track index
        uint32_t shift = actualIndex - pageWindow.start - maxPreviousTracks;
        pageWindow.start += shift;

        for (uint32_t idx = 0; idx < pageWindow.size - shift; idx++) {
          currentTrackWindow[idx] = currentTrackWindow[idx + shift];
        }
      }
    }
  } else {
    BELL_LOG(info, LOG_TAG,
             "Track not in window, {} - size {} - actualIndex {}",
             pageWindow.start, pageWindow.size, actualIndex);
  }
}

void ContextTrackResolver::updateTracksFromWindow() {
  BELL_LOG(info, LOG_TAG,
           "Updating tracks from window, currentPageIndex={}, "
           "currentTrackWindow.size()={}",
           currentPageIndex, currentTrackWindow.size());
  if (currentTrackWindow.empty()) {
    BELL_LOG(info, LOG_TAG, "Current track window is empty");
    return;
  }

  if (!getCurrentTrackInQueueIndex().has_value()) {
    contextTrackQueue.insert(contextTrackQueue.end(),
                             currentTrackWindow.begin(),
                             currentTrackWindow.end());

    highestWindowIndex = {
        .page = currentPageIndex,
        .track = pageWindows[currentPageIndex].start +
                 pageWindows[currentPageIndex].size - 1,
    };

    if (contextTrackQueue.size() > maxWindowSize) {
      uint32_t tracksToErase = contextTrackQueue.size() - maxWindowSize;
      BELL_LOG(info, LOG_TAG, "Erasing {} tracks from the start of the queue",
               tracksToErase);
      lowestWindowIndex.track += tracksToErase;

      if (lowestWindowIndex.track >=
          pageMetadata[lowestWindowIndex.page].trackCount) {
        lowestWindowIndex.track -=
            pageMetadata[lowestWindowIndex.page].trackCount;
        lowestWindowIndex.page++;
      }
    }
  } else if (currentPageIndex < lowestWindowIndex.page ||
             (currentPageIndex == lowestWindowIndex.page &&
              pageWindows[currentPageIndex].start < lowestWindowIndex.track)) {
    // If the current page is before the lowest window index, we insert the tracks before end of queue
    uint32_t tracksToInsert = std::min(
        ((maxWindowSize - 1) / 2) - getCurrentTrackInQueueIndex().value(),
        lowestWindowIndex.track);

    BELL_LOG(info, LOG_TAG,
             "Inserting {} tracks at the start of the queue, window size {}",
             tracksToInsert, currentTrackWindow.size());

    if (currentPageIndex < lowestWindowIndex.page) {
      lowestWindowIndex.page--;
      lowestWindowIndex.track =
          pageMetadata[lowestWindowIndex.page].trackCount - tracksToInsert;
    } else {
      lowestWindowIndex.track -= tracksToInsert;
    }

    contextTrackQueue.insert(contextTrackQueue.begin(),
                             currentTrackWindow.end() - tracksToInsert,
                             currentTrackWindow.end());
  } else {
    uint32_t currentNextTracks =
        contextTrackQueue.size() - getCurrentTrackInQueueIndex().value() - 1;
    uint32_t tracksToInsert =
        std::min((maxWindowSize - 1) / 2 - currentNextTracks,
                 static_cast<uint32_t>(currentTrackWindow.size()));
    highestWindowIndex = {
        currentPageIndex,
        pageWindows[currentPageIndex].start + tracksToInsert - 1};
    contextTrackQueue.insert(contextTrackQueue.end(),
                             currentTrackWindow.begin(),
                             currentTrackWindow.begin() + tracksToInsert);
  }

  BELL_LOG(info, LOG_TAG,
           "Lowest point - {},{} Highest point - {},{}, trackCount={}, "
           "trackIndexInQueue={}, currPageStart={}",
           lowestWindowIndex.page, lowestWindowIndex.track,
           highestWindowIndex.page, highestWindowIndex.track,
           contextTrackQueue.size(),
           getCurrentTrackInQueueIndex().value_or(555),
           pageWindows[currentPageIndex].start);
}

std::optional<uint32_t> ContextTrackResolver::getCurrentTrackInQueueIndex() {
  if (!currentTrack.has_value()) {
    return std::nullopt;  // No current track set
  }

  auto trackInQueueItr = std::find(contextTrackQueue.begin(),
                                   contextTrackQueue.end(), *currentTrack);

  if (trackInQueueItr == contextTrackQueue.end()) {
    return std::nullopt;  // Current track not found in the queue
  }
  return std::distance(contextTrackQueue.begin(), trackInQueueItr);
}

void ContextTrackResolver::resetParseState() {
  pageParseState.depth = 0;
  pageParseState.level = ContextTrackResolver::PageParseState::ExpectKey;
  pageParseState.lastKey.clear();
  pageParseState.parsedPageMetadata = {};
  pageParseState.parsedTrack = {};
  pageParseState.currentPageIndex = 0;
  pageParseState.trackIndexInPage = 0;
}
