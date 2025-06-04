#include "ContextTrackResolver.h"

#include <span>
#include <system_error>
#include <utility>

#include "bell/Logger.h"
#include "bell/http/Common.h"
#include "picojson.h"
#include "proto/ConnectPb.h"

using namespace cspot;

namespace {
// PicoJSON parse for context track data
class ContextTrackParseContext : public picojson::null_parse_context {
 public:
  ContextTrackParseContext(cspot_proto::ContextTrack* contextTrack)
      : contextTrack(contextTrack) {}

  template <typename Iter>
  bool parse_string(picojson::input<Iter>& in) {
    if (currentObjectKey == "uid") {
      contextTrack->uid.clear();  // Clear previous value
      return picojson::_parse_string(contextTrack->uid, in);
    }

    if (currentObjectKey == "uri") {
      contextTrack->uri.clear();  // Clear previous value
      return picojson::_parse_string(contextTrack->uri, in);
    }

    return picojson::null_parse_context::parse_string(in);
  }

  template <typename Iter>
  bool parse_object_item(picojson::input<Iter>& in, const std::string& key) {
    // Store the key for later use
    currentObjectKey = key;
    return _parse(*this, in);
  }

 private:
  std::string currentObjectKey;
  cspot_proto::ContextTrack* contextTrack;
};

// PicoJSON parser for context page data
class ContextPageParseContext : public picojson::null_parse_context {
 public:
  ContextPageParseContext(
      ContextTrackResolver::FetchParameters* fetchParameters,
      ContextTrackResolver::ResolvedContextPage* contextPage,
      bool isRoot = false)
      : fetchParameters(fetchParameters),
        contextPage(contextPage),
        isRoot(isRoot) {
    // Ensure no fetched tracks are present at the start
    if (fetchParameters->trackCache) {
      fetchParameters->trackCache->clear();
    }
  }

  template <typename Iter>
  bool parse_array_item(picojson::input<Iter>& in, size_t idx) {
    if (currentObjectKey == "tracks") {
      _parse(contextTrackParser, in);

      ContextTrackResolver::TrackId trackId(currentTrack.uid, currentTrack.uri);

      if (contextPage->trackIndexes.size() < idx + 1) {
        // Keep track of the index of each track in the context page
        contextPage->trackIndexes.push_back(idx);

        if ((contextPage->fetchWindowEnd - contextPage->fetchWindowStart) <
            fetchParameters->maxWindowSize) {
          contextPage->fetchWindowEnd++;  // Expand the fetch window
        }
      }

      if (idx == 0) {
        contextPage->firstId = trackId;
      }

      contextPage->lastId = trackId;

      // Check if running from the root context
      contextPage->isInRoot = isRoot;

      if (!fetchParameters->trackCache ||
          (fetchParameters->targetPageIndex.has_value() &&
           (fetchParameters->targetPageIndex != contextPage->pageIndex))) {
        BELL_LOG(
            debug, "parsero",
            "Skipping track {}-{} in page {}, not in target page ({} != {})",
            contextPage->pageIndex, idx, contextPage->pageUrl.value_or("N/A"),
            fetchParameters->targetPageIndex.value_or(555),
            contextPage->pageIndex);
        // Ignore if not needed
        return true;
      }

      // Store the current track in the context state
      currentTrack.index.track = static_cast<int32_t>(idx);
      currentTrack.index.page = contextPage->pageIndex;

      if (fetchParameters->slidingWindow &&
          (contextPage->fetchWindowEnd - contextPage->fetchWindowStart) >
              fetchParameters->maxWindowSize) {
        // Move the window forward
        contextPage->fetchWindowStart++;
        contextPage->fetchWindowEnd++;

        // Remove the oldest track from the cache
        fetchParameters->trackCache->erase(
            fetchParameters->trackCache->begin());
      }

      if (fetchParameters->slidingWindow &&
          fetchParameters->targetTrackId.has_value() &&
          (trackId == fetchParameters->targetTrackId)) {

        targetTrack = currentTrack;

        uint32_t previousTracksInWindow = (idx - contextPage->fetchWindowStart);
        uint32_t maxPreviousTracks = (fetchParameters->maxWindowSize - 1) / 2;
        if (previousTracksInWindow > maxPreviousTracks) {
          uint32_t tracksToRemove = previousTracksInWindow - maxPreviousTracks;
          contextPage->fetchWindowStart += tracksToRemove;
          fetchParameters->trackCache->erase(
              fetchParameters->trackCache->begin(),
              fetchParameters->trackCache->begin() + tracksToRemove);
        }

        // Found the target track, no longer using sliding window
        fetchParameters->slidingWindow = false;
      }

      // Construct the fetch window
      tcb::span<uint32_t> fetchWindowIds = {
          contextPage->trackIndexes.data() + contextPage->fetchWindowStart,
          contextPage->trackIndexes.data() + contextPage->fetchWindowEnd};

      auto* idxInWindow =
          std::find(fetchWindowIds.begin(), fetchWindowIds.end(), idx);

      if (idxInWindow != fetchWindowIds.end()) {
        uint32_t indexToInsert =
            std::distance(fetchWindowIds.begin(), idxInWindow);
        if (fetchParameters->trackCache->size() < indexToInsert + 1) {
          fetchParameters->trackCache->resize(indexToInsert + 1);
        }

        // Insert the current track at the correct index
        fetchParameters->trackCache->at(indexToInsert) = currentTrack;
      } else {
        BELL_LOG(error, "parsero",
                 "Track {}-{} not found in fetch window [{}-{}]",
                 contextPage->pageIndex, idx, contextPage->fetchWindowStart,
                 contextPage->fetchWindowEnd);
      }

      return true;
    }

    return _parse(*this, in);
  }

  template <typename Iter>
  bool parse_string(picojson::input<Iter>& in) {
    if (currentObjectKey == "page_url") {
      contextPage->pageUrl.emplace();
      return picojson::_parse_string(contextPage->pageUrl.value(), in);
    }

    if (currentObjectKey == "next_page_url") {
      contextPage->nextPageUrl.emplace();
      return picojson::_parse_string(contextPage->nextPageUrl.value(), in);
    }

    return picojson::null_parse_context::parse_string(in);
  }

  template <typename Iter>
  bool parse_object_item(picojson::input<Iter>& in, const std::string& key) {
    // Store the key for later use
    currentObjectKey = key;
    return _parse(*this, in);
  }

  std::optional<cspot_proto::ContextTrack> getTargetTrack() const {
    return targetTrack;
  }

 private:
  std::string currentObjectKey;

  ContextTrackResolver::FetchParameters* fetchParameters;
  ContextTrackResolver::ResolvedContextPage* contextPage;
  bool isRoot = false;

  std::optional<cspot_proto::ContextTrack> targetTrack = std::nullopt;
  cspot_proto::ContextTrack currentTrack;
  ContextTrackParseContext contextTrackParser{&currentTrack};
};

// PicoJSON parser for context root data
class ContextRootParseContext : public picojson::null_parse_context {
 public:
  ContextRootParseContext(
      ContextTrackResolver::FetchParameters initialFetchParameters,
      std::vector<ContextTrackResolver::ResolvedContextPage>* contextPages,
      std::function<void(const std::optional<cspot_proto::ContextTrack>&,
                         ContextTrackResolver::FetchParameters&)>
          pageParsedCallback)
      : fetchParameters(std::move(initialFetchParameters)),
        contextPages(contextPages),
        pageParsedCallback(std::move(pageParsedCallback)) {}

  template <typename Iter>
  bool parse_array_item(picojson::input<Iter>& in, size_t idx) {
    if (currentObjectKey == "pages") {
      if (contextPages->size() <= idx) {
        // Ensure we have enough space in the context pages vector
        contextPages->resize(idx + 1);
      }

      contextPages->at(idx).pageIndex = static_cast<int>(idx);

      bool callCallback = !fetchParameters.targetPageIndex.has_value() ||
                          (fetchParameters.targetPageIndex.value() == idx);

      auto pageCtx = ContextPageParseContext(&fetchParameters,
                                             &(*contextPages)[idx], true);

      // Parse the context page
      _parse(pageCtx, in);

      auto& currentPage = contextPages->at(idx);
      if (idx == contextPages->size() - 1 &&
          (currentPage.nextPageUrl.has_value() || currentPage.isInRoot)) {
        // Push dummy page for the next page
        contextPages->push_back(ContextTrackResolver::ResolvedContextPage{});
      }

      // This can modify the fetch parameters, so we need to pass them back
      if (callCallback) {
        pageParsedCallback(pageCtx.getTargetTrack(), this->fetchParameters);
      }

      return true;
    }

    return _parse(*this, in);
  }

  template <typename Iter>
  bool parse_object_item(picojson::input<Iter>& in, const std::string& key) {
    // Store the key for later use
    currentObjectKey = key;
    return _parse(*this, in);
  }

  bool parse_object_stop() { return true; }

 private:
  std::string currentObjectKey;

  ContextTrackResolver::FetchParameters fetchParameters;
  std::vector<ContextTrackResolver::ResolvedContextPage>* contextPages;
  std::function<void(const std::optional<cspot_proto::ContextTrack>&,
                     ContextTrackResolver::FetchParameters&)>
      pageParsedCallback;
};
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
}

bell::Result<cspot_proto::ContextTrack>
ContextTrackResolver::getCurrentTrack() {
  auto res = ensureContextTracks();
  if (!res) {
    BELL_LOG(error, LOG_TAG, "Failed to ensure context tracks: {}",
             res.errorMessage());
    return res.getError();
  }

  return trackCache[currentTrackInCacheIndex.value()];
}

tcb::span<cspot_proto::ContextTrack> ContextTrackResolver::previousTracks() {
  if (!currentTrackInCacheIndex.has_value()) {
    return {};
  }
  return {trackCache.data(), currentTrackInCacheIndex.value()};
}

tcb::span<cspot_proto::ContextTrack> ContextTrackResolver::nextTracks() {
  if (!currentTrackInCacheIndex.has_value()) {
    return {};
  }
  return {trackCache.data() + currentTrackInCacheIndex.value() + 1,
          trackCache.size() - currentTrackInCacheIndex.value() - 1};
}

bell::Result<cspot_proto::ContextTrack> ContextTrackResolver::next() {
  auto res = ensureContextTracks();
  if (!res) {
    BELL_LOG(error, LOG_TAG, "Failed to ensure context tracks: {}",
             res.errorMessage());
    return res.getError();
  }

  if (currentTrackInCacheIndex.value() + 1 >= trackCache.size()) {
    BELL_LOG(error, LOG_TAG, "No next track available");
    return std::errc::no_message_available;
  }

  if (currentTrackInCacheIndex.value() > trackUpdateThreshold) {
    // Erase oldest track
    trackCache.erase(trackCache.begin());
  } else {
    currentTrackInCacheIndex.value()++;
  }

  return trackCache[currentTrackInCacheIndex.value()];
}

bell::Result<cspot_proto::ContextTrack> ContextTrackResolver::previous() {
  auto res = ensureContextTracks();
  if (!res) {
    BELL_LOG(error, LOG_TAG, "Failed to ensure context tracks: {}",
             res.errorMessage());
    return res.getError();
  }

  if (currentTrackInCacheIndex.value() + 1 >= trackCache.size()) {
    BELL_LOG(error, LOG_TAG, "No next track available");
    return std::errc::no_message_available;
  }

  trackCache.erase(trackCache.end());

  // Go to previous track
  currentTrackInCacheIndex.value() -= 1;

  return trackCache[currentTrackInCacheIndex.value()];
}

bell::Result<> ContextTrackResolver::ensureContextTracks() {
  FetchParameters fetchParameters;
  prepareFetchParams(fetchParameters, trackUpdateThreshold);

  if (fetchParameters.fetchMode == FetchMode::Ignore) {
    // No need to fetch more tracks, return early
    return {};
  }

  if (fetchParameters.targetPageIndex.has_value()) {
    auto& page = resolvedContextPages[fetchParameters.targetPageIndex.value()];
    if (page.isInRoot) {
      return resolveRootContext(fetchParameters);
    }
    return resolveContextPage(fetchParameters);
  }

  return resolveRootContext(fetchParameters);
}

void ContextTrackResolver::prepareFetchParams(FetchParameters& fetchParameters,
                                              uint32_t fetchThreshold) {
  if (!currentTrackInCacheIndex.has_value()) {
    fetchParameters = {
        .fetchMode = FetchMode::AddNext,
        .slidingWindow = true,
        .maxWindowSize = maxWindowSize,
        .targetTrackId = currentTrackId,
        .trackCache = &unprocessedTracksCache,
        .targetPageIndex = std::nullopt,
    };

    return;
  }

  if ((trackCache.size() - currentTrackInCacheIndex.value()) < fetchThreshold &&
      !isAtEndOfContext()) {
    uint32_t pageIdx = trackCache.back().index.page;
    auto& page = resolvedContextPages[pageIdx];

    uint32_t windowSize = maxWindowSize;

    if (page.fetchWindowEnd == page.trackIndexes.size()) {
      pageIdx++;
      page = resolvedContextPages[pageIdx];
      windowSize = std::min(maxWindowSize,
                            static_cast<uint32_t>(page.trackIndexes.size()));

      page.fetchWindowStart = 0;
      page.fetchWindowEnd = windowSize;  // Reset to start of next page
    } else {
      windowSize = std::min(maxWindowSize,
                            static_cast<uint32_t>(page.trackIndexes.size()) -
                                page.fetchWindowEnd);
      page.fetchWindowStart = page.fetchWindowEnd;
      page.fetchWindowEnd = page.fetchWindowEnd + windowSize;
    }

    fetchParameters = {
        .fetchMode = FetchMode::AddNext,
        .slidingWindow = false,
        .maxWindowSize = windowSize,
        .targetTrackId = std::nullopt,
        .trackCache = &unprocessedTracksCache,
        .targetPageIndex = pageIdx,
    };
  } else if ((currentTrackInCacheIndex.value() <= fetchThreshold) &&
             !isAtStartOfContext()) {
    uint32_t pageIdx = trackCache.front().index.page;
    auto& page = resolvedContextPages[pageIdx];

    uint32_t windowSize = maxWindowSize;

    if (page.fetchWindowStart == 0) {
      assert(pageIdx > 0);

      pageIdx--;
      page = resolvedContextPages[pageIdx];

      windowSize = std::min(maxWindowSize,
                            static_cast<uint32_t>(page.trackIndexes.size()));
      page.fetchWindowEnd = page.trackIndexes.size();
      page.fetchWindowStart = page.fetchWindowEnd - windowSize;
    } else {
      windowSize = std::min(maxWindowSize, page.fetchWindowStart);

      page.fetchWindowEnd = page.fetchWindowStart;
      page.fetchWindowStart = page.fetchWindowStart - windowSize;
    }

    fetchParameters = {
        .fetchMode = FetchMode::AddPrevious,
        .slidingWindow = false,
        .maxWindowSize = windowSize,
        .targetTrackId = std::nullopt,
        .trackCache = &unprocessedTracksCache,
        .targetPageIndex = pageIdx,
    };
  } else {
    BELL_LOG(
        debug, LOG_TAG,
        "No need to fetch more tracks, current index: {}, cache size: {} - {}",
        fetchThreshold, currentTrackInCacheIndex.value(), trackCache.size());
    // No need to fetch more tracks, reset parameters
    fetchParameters.trackCache = nullptr;
    fetchParameters.fetchMode = FetchMode::Ignore;
  }

  // If max window size is not set, use the default
  if (fetchParameters.maxWindowSize == 0) {
    fetchParameters.maxWindowSize = maxWindowSize;
  }

  if (fetchParameters.fetchMode != FetchMode::Ignore) {
    BELL_LOG(debug, LOG_TAG,
             "Prepared fetch parameters: mode={}, slidingWindow={}, "
             "maxWindowSize={},  targetPageIndex={}",
             static_cast<int>(fetchParameters.fetchMode),
             fetchParameters.slidingWindow, fetchParameters.maxWindowSize,
             fetchParameters.targetPageIndex.has_value()
                 ? std::to_string(fetchParameters.targetPageIndex.value())
                 : "N/A");

    if (fetchParameters.targetPageIndex.has_value()) {
      auto& page =
          resolvedContextPages[fetchParameters.targetPageIndex.value()];
      BELL_LOG(debug, LOG_TAG,
               "Target page index: {}, window start: {}, end: {}, ",
               fetchParameters.targetPageIndex.value(), page.fetchWindowStart,
               page.fetchWindowEnd);
    }
  }
}

void ContextTrackResolver::updateTracks(
    FetchMode fetchMode, std::vector<cspot_proto::ContextTrack>& parsedTracks,
    const std::optional<cspot_proto::ContextTrack>& targetTrackResult) {
  BELL_LOG(debug, LOG_TAG, "Updating tracks from parse state, found index: {}",
           currentTrackInCacheIndex.has_value()
               ? std::to_string(currentTrackInCacheIndex.value())
               : "N/A");
  if (fetchMode == FetchMode::AddNext) {
    BELL_LOG(debug, LOG_TAG, "Adding {} tracks to the cache in AddNext mode",
             parsedTracks.size());
    if (!currentTrackInCacheIndex.has_value() &&
        (trackCache.size() + parsedTracks.size() > maxWindowSize)) {
      uint32_t tracksToRemove =
          trackCache.size() + parsedTracks.size() - maxWindowSize;
      trackCache.erase(trackCache.begin(), trackCache.begin() + tracksToRemove);
      if (currentTrackInCacheIndex.has_value()) {
        currentTrackInCacheIndex.value() -= tracksToRemove;
      }
    }

    // If we are in add next, we only add tracks after the found index
    trackCache.insert(trackCache.end(), parsedTracks.begin(),
                      parsedTracks.end());

  } else if (fetchMode == FetchMode::AddPrevious) {
    // If we are in add prev mode, we only add tracks before the found index
    trackCache.insert(trackCache.begin(), parsedTracks.begin(),
                      parsedTracks.end());
    currentTrackInCacheIndex.value() += parsedTracks.size();
  }

  if (targetTrackResult.has_value()) {
    // If we found the target track, we need to update the current track index
    auto it = std::find(trackCache.begin(), trackCache.end(),
                        targetTrackResult.value());
    if (it != trackCache.end()) {
      currentTrackInCacheIndex = std::distance(trackCache.begin(), it);
      BELL_LOG(debug, LOG_TAG, "Found target track at index: {}",
               currentTrackInCacheIndex.value());
    } else {
      BELL_LOG(error, LOG_TAG, "Target track not found in the updated cache");
      currentTrackInCacheIndex = std::nullopt;
    }
  }
}

bell::Result<> ContextTrackResolver::resolveRootContext(
    FetchParameters& fetchParameters) {
  BELL_LOG(info, LOG_TAG, "Resolving root context: {}", rootContextUrl);

  auto reader = spClient->contextResolve(rootContextUrl);
  if (!reader) {
    BELL_LOG(error, LOG_TAG, "Failed to resolve root context: {}",
             reader.errorMessage());
    return reader.getError();
  }

  auto* rawDataStream = reader.getValue().getStream();

  auto parseCtx = ContextRootParseContext(
      fetchParameters, &resolvedContextPages,
      [this](auto& resolvedTrackIndex, auto& fetchParams) {
        // Update the tracks
        updateTracks(fetchParams.fetchMode, unprocessedTracksCache,
                     resolvedTrackIndex);

        // Prepare next fetch parameters
        prepareFetchParams(fetchParams, (maxWindowSize - 1) / 2);
      });
  std::string parseError;
  picojson::_parse(parseCtx,
                   std::istreambuf_iterator<char>(rawDataStream->rdbuf()),
                   std::istreambuf_iterator<char>(), &parseError);

  if (!parseError.empty()) {
    BELL_LOG(error, LOG_TAG, "Failed to parse context data: {}", parseError);
    return std::errc::invalid_argument;
  }

  if (resolvedContextPages.back().nextPageUrl.has_value()) {
    // If the last page has a next page URL, we need to add it to the resolved pages
    resolvedContextPages.push_back(
        {.pageUrl = resolvedContextPages.back().nextPageUrl});
  }

  BELL_LOG(info, LOG_TAG, "Root context resolved successfully");
  uint32_t nextPageIndex = trackCache.back().index.page;

  while (!currentTrackInCacheIndex.has_value()) {
    nextPageIndex++;
    BELL_LOG(info, LOG_TAG,
             "Current track index not found, resolving next page");
    BELL_LOG(info, LOG_TAG, "Next page index: {}", nextPageIndex);
    if (nextPageIndex >= resolvedContextPages.size()) {
      BELL_LOG(error, LOG_TAG,
               "No more pages to resolve, cannot find current track index");
      return std::errc::invalid_argument;
    }

    prepareFetchParams(fetchParameters, (maxWindowSize - 1) / 2);
    fetchParameters.targetPageIndex = nextPageIndex;

    auto res = resolveContextPage(fetchParameters);
    if (!res) {
      BELL_LOG(error, LOG_TAG, "Failed to resolve context page: {}",
               res.errorMessage());
      return res.getError();
    }

    if (resolvedContextPages.back().nextPageUrl.has_value()) {
      // If the last page has a next page URL, we need to add it to the resolved pages
      resolvedContextPages.push_back(
          {.pageUrl = resolvedContextPages.back().nextPageUrl});
    }
  }

  BELL_LOG(info, LOG_TAG, "Current track index found: {}",
           currentTrackInCacheIndex.has_value()
               ? std::to_string(currentTrackInCacheIndex.value())
               : "N/A");

  return {};
}

bell::Result<> ContextTrackResolver::resolveContextPage(
    FetchParameters& fetchParameters) {
  auto& page = resolvedContextPages[fetchParameters.targetPageIndex.value()];

  if (!page.pageUrl.has_value()) {
    BELL_LOG(error, LOG_TAG, "Context page URL is not set");
  }
  BELL_LOG(info, LOG_TAG, "Resolving context page: {}", page.pageUrl.value());

  auto reader = spClient->doRequest(bell::http::Method::GET,
                                    page.pageUrl.value().substr(5));
  if (!reader) {
    BELL_LOG(error, LOG_TAG, "Failed to resolve context page: {}",
             reader.errorMessage());
    return reader.getError();
  }

  auto* rawDataStream = reader.getValue().getStream();

  auto parseCtx = ContextPageParseContext(&fetchParameters, &page);
  std::string parseError;
  picojson::_parse(parseCtx,
                   std::istreambuf_iterator<char>(rawDataStream->rdbuf()),
                   std::istreambuf_iterator<char>(), &parseError);

  if (!parseError.empty()) {
    BELL_LOG(error, LOG_TAG, "Failed to parse context page data: {}",
             parseError);
    return std::errc::invalid_argument;
  }

  updateTracks(fetchParameters.fetchMode, unprocessedTracksCache,
               parseCtx.getTargetTrack());

  BELL_LOG(info, LOG_TAG, "Context page resolved successfully");
  return {};
}

bool ContextTrackResolver::isAtStartOfContext() const {
  if (trackCache.empty()) {
    return false;
  }

  if (trackCache.front().index.page != 0) {
    return false;
  }

  if (resolvedContextPages[0].fetchWindowStart > 0) {
    return false;
  }

  return true;
}

bool ContextTrackResolver::isAtEndOfContext() const {
  if (trackCache.empty()) {
    return false;
  }

  const auto& lastTrack = trackCache.back();
  if ((lastTrack.index.page + 1) < resolvedContextPages.size()) {
    return false;  // More pages available
  }

  if (resolvedContextPages.back().fetchWindowEnd <
      resolvedContextPages.back().trackIndexes.size()) {
    return false;  // More tracks available in the last page
  }
  return true;
}
