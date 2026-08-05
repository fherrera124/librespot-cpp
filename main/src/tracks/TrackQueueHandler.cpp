#include "tracks/TrackQueueHandler.h"
#include <algorithm>
#include "bell/Result.h"
#include "bell/http/Client.h"
#include "crypto/Base62.h"
#include "events/EventLoop.h"
#include "proto/ConnectPb.h"
#include "proto/SpotifyId.h"
#include "nonstd/expected.hpp"
#include "tracks/ContextPageParser.h"

using namespace cspot;

namespace {
// Fetch new page when less than this many tracks remain
const uint32_t trackFetchThreshold = 8;
const uint32_t trackWindowLen = 6;

// Converts a spotify URI to a 16byte GID, returns empty array on failure
std::optional<std::array<std::byte, 16>> uriToGid(const std::string& uri) {
  std::array<std::byte, 16> trackGid{};
  size_t outLen = 16;
  auto uriDelimiter = uri.find_last_of(':');
  if (uriDelimiter == std::string::npos) {
    return std::nullopt;
  }

  if (!cspot::base62Decode(uri.substr(uriDelimiter + 1), trackGid.data(),
                           outLen)) {

    return std::nullopt;
  }
  if (outLen < 16) {
    // Move gid right to fill leading zeros
    std::memmove(trackGid.data() + (16 - outLen), trackGid.data(), outLen);
  }

  return trackGid;
}

class DefaultTrackQueueHandler : public TrackQueueHandler {
 public:
  DefaultTrackQueueHandler(std::shared_ptr<SpClient> spClient,
                           std::shared_ptr<EventLoop> eventLoop);

  bell::Result<> loadContext(
      const std::string& contextUri, std::optional<std::string> currentTrackUri,
      std::optional<std::string> currentTrackUid,
      std::optional<uint32_t> currentTrackIndex) override;

  void setQueue(const std::vector<cspot_proto::ContextTrack>& queue) override;

  void setPlayingQueue(bool isPlayingQueue) override;

  std::optional<cspot_proto::ProvidedTrack> currentTrack() override;

  std::optional<cspot_proto::ContextIndex> currentContextIndex() override;

  bell::Result<TrackAdvanceResult> skipToNextTrack(
      const std::string& trackUri) override;
  bell::Result<> skipToPreviousTrack(const std::string& trackUri) override;

  bell::Result<> enableShuffle(bool enable) override;

  tcb::span<cspot_proto::ProvidedTrack> nextTracks() override;
  tcb::span<cspot_proto::ProvidedTrack> previousTracks() override;

  void updateTrackWindows(bool forceNotify) override;

 private:
  const char* LOG_TAG = "TrackQueueHandler";

  std::shared_ptr<SpClient> spClient;
  std::shared_ptr<EventLoop> eventLoop;

  // Gid is always 16bytes
  using GidBytes = std::array<std::byte, 16>;

  struct FetchedContextPage {
    std::optional<std::string> url{};
    std::vector<GidBytes> trackGids{};

    bool operator==(const FetchedContextPage& other) const {
      return url == other.url && trackGids == other.trackGids;
    }
  };

  std::vector<cspot_proto::ContextTrack> queue;
  bool isPlayingQueue = false;

  std::string currentContextUri;

  ContextPageParser pageParser;

  SpotifyIdType contextIdType = SpotifyIdType::Track;
  std::vector<FetchedContextPage> contextPages;
  std::optional<cspot_proto::ContextIndex> contextIndex;

  std::pair<std::string, std::string> targetTrackIds{};
  std::optional<uint32_t> targetTrackIndex;

  // Current window of tracks, used for providing next/previous tracks to UI and track player
  std::array<cspot_proto::ProvidedTrack, trackWindowLen> nextTracksWindow{};
  std::array<cspot_proto::ProvidedTrack, trackWindowLen> previousTracksWindow{};

  // Last current-track uri actually reported via QUEUE_UPDATED - see
  // updateTrackWindows()'s own comment on why this, not just the next/
  // previous window diff, has to gate that event.
  std::string lastNotifiedCurrentTrackUri;

  void resetContext();

  void onTrackParsed(uint32_t pageIndex, uint32_t trackIndex,
                     const cspot_proto::ContextTrack& track);

  void onPageMetadataParsed(uint32_t pageIndex,
                            const PageMetadata& pageMetadata);

  std::optional<cspot_proto::ContextIndex> getOffsetIndex(int32_t offset) const;

  // Converts a flat 0-based track index (position across the whole
  // context, not per-page - matches go-librespot's own SkipTo.TrackIndex
  // semantics in daemon/player.go) into a {page, track} pair, by walking
  // contextPages. Only correct once every page up to the target has
  // actually been fetched; returns nullopt if the walk runs past what's
  // currently populated (not yet fetched, or genuinely out of range) -
  // callers fall back to loadContext()'s own "default to zero" handling
  // in that case.
  std::optional<cspot_proto::ContextIndex> resolveFlatIndex(
      uint32_t flatIndex) const;

  bell::Result<> ensureEnoughTracks();
  bell::Result<> fetchRootPage(const std::string& rootContextUri);
  bell::Result<> fetchContextPage(FetchedContextPage& page);
  bell::Result<> feedResponseToParser(bell::HTTPResponse& response);
};
};  // namespace

DefaultTrackQueueHandler::DefaultTrackQueueHandler(
    std::shared_ptr<SpClient> spClient, std::shared_ptr<EventLoop> eventLoop)
    : spClient(std::move(spClient)), eventLoop(std::move(eventLoop)) {
  pageParser.setCallbacks(
      [this](auto pageIndex, auto trackIndex, const auto& track) {
        this->onTrackParsed(pageIndex, trackIndex, track);
      },
      [this](auto pageIndex, const auto& pageMetadata) {
        this->onPageMetadataParsed(pageIndex, pageMetadata);
      });
}

bell::Result<> DefaultTrackQueueHandler::loadContext(
    const std::string& contextUri, std::optional<std::string> currentTrackUri,
    std::optional<std::string> currentTrackUid,
    std::optional<uint32_t> currentTrackIndex) {
  // The "same context" fast path below only knows how to resolve a uri
  // (cached GID search) or an index (resolveFlatIndex against the cache) -
  // a uid-only target still needs a fresh fetch+parse for onTrackParsed()'s
  // own uid check to find it, same as before this function took an index
  // at all.
  bool haveFastPathTarget =
      currentTrackUri.has_value() || currentTrackIndex.has_value();

  // In case we only have UID, we need to refetch the pages either way - we only keep the gids
  if (currentContextUri != contextUri || !haveFastPathTarget) {
    // New context, reset everything
    resetContext();
    targetTrackIds = {currentTrackUri.value_or(""),
                      currentTrackUid.value_or("")};
    targetTrackIndex = currentTrackIndex;

    contextIdType = SpotifyIdType::Track;

    if (!currentTrackUri && !currentTrackUid && !currentTrackIndex) {
      contextIndex = {
          0,
          0,
      };  // Start from the beginning if no current track is provided
    }

    if (contextUri.starts_with("spotify:show")) {
      // TODO: Better handling of the id types here
      contextIdType = SpotifyIdType::Episode;
    }

    auto res = fetchRootPage(contextUri);
    if (!res) {
      BELL_LOG(error, LOG_TAG, "Could not resolve context root, uri={}, err={}",
               contextUri, res.error());
      return nonstd::make_unexpected(res.error());
    }

    if (targetTrackIndex && !contextIndex) {
      contextIndex = resolveFlatIndex(*targetTrackIndex);
    }
  } else {
    // Same context, only reset the current index & queue - contextPages is
    // already fully populated from the earlier load, so both target kinds
    // resolve from that cache directly, no network fetch needed.
    contextIndex.reset();
    queue.clear();
    isPlayingQueue = false;

    targetTrackIds = {currentTrackUri.value_or(""),
                      currentTrackUid.value_or("")};
    targetTrackIndex = currentTrackIndex;

    if (currentTrackUri) {
      auto targetGid = uriToGid(*currentTrackUri);

      for (size_t pageIdx = 0; pageIdx < contextPages.size(); pageIdx++) {
        auto trackItr =
            std::find(contextPages[pageIdx].trackGids.begin(),
                      contextPages[pageIdx].trackGids.end(), targetGid);
        if (trackItr != contextPages[pageIdx].trackGids.end()) {
          // Found current track in the parsed data
          contextIndex = {
              static_cast<uint32_t>(pageIdx),
              static_cast<uint32_t>(std::distance(
                  contextPages[pageIdx].trackGids.begin(), trackItr))};
          break;
        }
      }
    } else {
      contextIndex = resolveFlatIndex(*currentTrackIndex);
    }
  }

  for (auto& page : contextPages) {
    if (contextIndex.has_value()) {
      // Found context index, break loop
      break;
    }

    // Find page that does not have track ids
    if (!page.trackGids.empty()) {
      continue;
    }

    auto res = fetchContextPage(page);
    if (!res) {
      BELL_LOG(error, LOG_TAG, "Could not resolve context page, uri={}, err={}",
               *page.url, res.error());
      return nonstd::make_unexpected(res.error());
    }

    if (targetTrackIndex && !contextIndex) {
      contextIndex = resolveFlatIndex(*targetTrackIndex);
    }
  }

  if (contextIndex.has_value()) {
    BELL_LOG(info, LOG_TAG, "Found current track at index=[{},{}]",
             contextIndex->track, contextIndex->page);
  } else if (!contextPages.empty() && !contextPages[0].trackGids.empty()) {
    BELL_LOG(
        error, LOG_TAG,
        "Could not find current track in the given context, default to zero");
    contextIndex = {
        0,
        0,
    };  // Default to start if we could not find the current track
  } else {
    // contextPages has no actual track data (e.g. an unsupported/empty
    // context) - defaulting contextIndex to {0,0} here used to leave it
    // pointing at a page that was never populated, crashing later in
    // ensureEnoughTracks(). Matches master's own ContextResolver::resolve(),
    // which explicitly treats "resolved OK but zero tracks" as failure
    // (`ok && !tracksOut.empty()`), not success.
    BELL_LOG(error, LOG_TAG, "Context resolved with no tracks, uri={}",
             contextUri);
    return bell::make_unexpected_errc(std::errc::no_such_file_or_directory);
  }

  return {};
}

void DefaultTrackQueueHandler::onTrackParsed(
    uint32_t pageIndex, uint32_t trackIndex,
    const cspot_proto::ContextTrack& track) {
  if (contextPages.size() < pageIndex + 1) {
    contextPages.resize(pageIndex + 1);
  }

  if ((!track.uid.empty() && track.uid == targetTrackIds.second) ||
      (!track.uri.empty() && track.uri == targetTrackIds.first)) {
    // Found current track in the parsed data
    contextIndex = {pageIndex, trackIndex};
  }

  auto trackGid = uriToGid(track.uri);

  if (!trackGid) {
    BELL_LOG(error, LOG_TAG, "Could not parse uri={}", track.uri);
    return;
  }

  contextPages[pageIndex].trackGids.push_back(*trackGid);
}

void DefaultTrackQueueHandler::onPageMetadataParsed(
    uint32_t pageIndex, const PageMetadata& pageMetadata) {
  if (contextPages.size() < pageIndex + 1) {
    contextPages.resize(pageIndex + 1);
  }

  auto& contextPage = contextPages[pageIndex];
  contextPage.url = pageMetadata.pageUrl;
  if (pageMetadata.nextPageUrl.has_value() &&
      contextPages.size() == pageIndex + 1) {
    // Push next page as url if available
    contextPages.push_back({
        .url = pageMetadata.nextPageUrl,
    });
  }
}

bell::Result<> DefaultTrackQueueHandler::ensureEnoughTracks() {
  if (!contextIndex) {
    return {};  // Cant ensure tracks without context index
  }

  // Same invariant getOffsetIndex()/currentTrack() already guard
  // defensively instead of trusting - contextIndex pointing past what's
  // actually in contextPages (e.g. a context that resolved with no tracks)
  // used to hit these as raw assert()s, aborting the whole device instead
  // of degrading gracefully.
  if (contextIndex->page >= contextPages.size() ||
      contextIndex->track >= contextPages[contextIndex->page].trackGids.size()) {
    return {};
  }
  size_t nextTracksCount = contextPages[contextIndex->page].trackGids.size() -
                           (contextIndex->track + 1);

  size_t nextPageIndex = contextIndex->page + 1;

  // Iterate over next pages until we have enough tracks or run out of pages
  while (contextPages.size() > (nextPageIndex) &&
         (nextTracksCount < trackFetchThreshold)) {
    if (!contextPages[nextPageIndex].trackGids.empty()) {
      nextTracksCount += contextPages[nextPageIndex].trackGids.size();
      nextPageIndex++;
      continue;
    }

    auto res = fetchContextPage(contextPages[nextPageIndex]);
    if (!res) {
      BELL_LOG(error, LOG_TAG, "Could not resolve context page, uri={}, err={}",
               *contextPages[nextPageIndex].url, res.error());
      return nonstd::make_unexpected(res.error());
    }
    nextTracksCount += contextPages[nextPageIndex].trackGids.size();
    nextPageIndex++;
  }

  return {};
}

bell::Result<> DefaultTrackQueueHandler::fetchRootPage(
    const std::string& rootContextUri) {
  BELL_LOG(info, LOG_TAG, "Fetching context root, uri={}", rootContextUri);
  pageParser.reset();
  auto res = spClient->contextResolve(rootContextUri);
  if (!res) {
    return nonstd::make_unexpected(res.error());
  }

  auto feedRes = feedResponseToParser(*res);
  if (!feedRes) {
    return feedRes;
  }

  // Only claim this context as loaded once it actually is - a real hardware
  // crash (LoadProhibited in getOffsetIndex(), out-of-bounds contextPages[0]
  // on an empty vector) traced back to this being set unconditionally
  // before the fetch/parse could fail: a failed first attempt still left
  // currentContextUri pointing at the target playlist, so the app's retry
  // took loadContext()'s "same context, don't refetch" branch against a
  // contextPages that was never actually populated.
  this->currentContextUri = rootContextUri;
  return {};
}

bell::Result<> DefaultTrackQueueHandler::fetchContextPage(
    DefaultTrackQueueHandler::FetchedContextPage& page) {
  if (!page.url) {
    // Called with no valid url set on page
    return bell::make_unexpected_errc(std::errc::invalid_argument);
  }
  BELL_LOG(info, LOG_TAG, "Fetching context page, uri={}", *page.url);
  pageParser.reset();

  std::string pageUrl = *page.url;
  // Strip hm:// from pageurl
  if (pageUrl.starts_with("hm://")) {
    pageUrl = pageUrl.substr(5);
  }

  auto itr = std::find(contextPages.begin(), contextPages.end(), page);
  if (itr == contextPages.end()) {
    // No such page in context pages
    return bell::make_unexpected_errc(std::errc::invalid_argument);
  }

  // Reset page parser to work in pre-defined page mode
  size_t idx = std::distance(contextPages.begin(), itr);
  pageParser.reset(idx);

  auto res = spClient->rawRequest(pageUrl);
  if (!res) {
    return nonstd::make_unexpected(res.error());
  }

  return feedResponseToParser(*res);
}

bell::Result<> DefaultTrackQueueHandler::feedResponseToParser(
    bell::HTTPResponse& response) {
  if (response.statusCode != 200) {
    // Drain before returning - a pooled HTTP/1.1 connection is only safe
    // to reuse once the body's been read, error responses included. Real
    // hardware failure: skipping this on an error path (SpClient's own
    // putConnectState/putInactive/extendedMetadataRaw had the same bug)
    // left the error body sitting unread on the wire, so the next request
    // to reuse this connection - here, spClient's own contextResolve() -
    // read that leftover body instead of its own response headers.
    (void)response.bytes();
    return bell::make_unexpected_errc(std::errc::bad_message);
  }

  auto* stream = response.stream();
  size_t bytesToRead = *response.contentLength;
  std::array<std::byte, 512> buffer{};

  // Temp diagnostic: capture what actually arrives on the wire (bounded, to
  // avoid flooding the log on a real, populated page) - to confirm/rule out
  // whether rawRequest()'s missing Accept header (see its own comment) is
  // the reason contextPages ends up empty, before touching that header.
  constexpr size_t kDiagCap = 1024;
  std::string diagBody;
  diagBody.reserve(std::min<size_t>(bytesToRead, kDiagCap));

  while (bytesToRead > 0 && !stream->eof() && !stream->bad()) {
    size_t toRead = std::min(buffer.size(), bytesToRead);
    stream->read(reinterpret_cast<char*>(buffer.data()), toRead);

    if (stream->bad()) {
      break;
    }

    size_t bytesRead = stream->gcount();

    if (diagBody.size() < kDiagCap) {
      size_t toCopy = std::min(bytesRead, kDiagCap - diagBody.size());
      diagBody.append(reinterpret_cast<const char*>(buffer.data()), toCopy);
    }

    auto res = pageParser.feed(buffer.data(), bytesRead);
    bytesToRead -= bytesRead;

    if (!res) {
      BELL_LOG(error, LOG_TAG, "Error occured while parsing page, err={}",
               res.error());
      BELL_LOG(error, LOG_TAG,
               "RAW BODY DIAG (parse failed): status={} contentLength={} "
               "body[0..{}]={}",
               response.statusCode, *response.contentLength, diagBody.size(),
               diagBody);
      return nonstd::make_unexpected(res.error());
    }
  }

  if (stream->bad()) {
    // Error while reading stream
    return bell::make_unexpected_errc(std::errc::io_error);
  }

  if (!pageParser.finish()) {
    BELL_LOG(error, LOG_TAG, "Error occured while finalizing page parse");
    return bell::make_unexpected_errc(std::errc::io_error);
  }

  BELL_LOG(info, LOG_TAG,
           "RAW BODY DIAG: status={} contentLength={} body[0..{}]={}",
           response.statusCode, *response.contentLength, diagBody.size(),
           diagBody);

  return {};
}

void DefaultTrackQueueHandler::resetContext() {
  pageParser.reset();
  contextPages = {};
  contextIndex.reset();
  targetTrackIds = {};
  targetTrackIndex.reset();
  nextTracksWindow.fill(cspot_proto::ProvidedTrack{});
  previousTracksWindow.fill(cspot_proto::ProvidedTrack{});
  currentContextUri.clear();
}

std::optional<cspot_proto::ProvidedTrack>
DefaultTrackQueueHandler::currentTrack() {
  auto res = ensureEnoughTracks();
  if (!res) {
    BELL_LOG(error, LOG_TAG, "Could not ensure enough tracks, err={}",
             res.error());
  }

  if (isPlayingQueue && !queue.empty()) {
    auto& track = queue[0];
    return cspot_proto::ProvidedTrack{
        .uri = track.resolvedUri(contextIdType),
        .uid = "q0",
        .provider = "queue",
        .gid = std::nullopt,
    };
  }

  if (contextIndex) {
    // Ensure context index is valid
    if ((contextPages.size() < contextIndex->page + 1) ||
        (contextPages[contextIndex->page].trackGids.size() <
         contextIndex->track + 1)) {
      return std::nullopt;
    }

    // Reconstruct spotify ID from the bare gid
    SpotifyId trackId(
        contextIdType,
        contextPages[contextIndex->page].trackGids[contextIndex->track]);

    return cspot_proto::ProvidedTrack{
        .uri = trackId.uri,
        .uid = "",
        .provider = "context",
        .gid = std::nullopt,
    };
  }

  return std::nullopt;
}

void DefaultTrackQueueHandler::setQueue(
    const std::vector<cspot_proto::ContextTrack>& queue) {
  this->queue = queue;
  isPlayingQueue = false;
}

void DefaultTrackQueueHandler::setPlayingQueue(bool isPlayingQueue) {
  this->isPlayingQueue = isPlayingQueue;
}

std::optional<cspot_proto::ContextIndex>
DefaultTrackQueueHandler::currentContextIndex() {
  if (isPlayingQueue) {
    return std::nullopt;  // No context index when playing from queue
  }

  return contextIndex;
}

bell::Result<TrackAdvanceResult> DefaultTrackQueueHandler::skipToNextTrack(
    const std::string& trackUri) {
  (void)trackUri;  //TODO: Implement skipping to specific track in context

  if (!isPlayingQueue && !queue.empty()) {
    setPlayingQueue(true);
    return TrackAdvanceResult::Advanced;
  }

  if (isPlayingQueue && !queue.empty()) {
    if (queue.size() == 1 && !contextIndex) {
      // Sole queue track with no context to fall back to (an ad-hoc/
      // single-track transfer) - nothing to advance to. Matches
      // skipToPreviousTrack()'s own boundary handling: stay put rather
      // than erasing the only track and leaving currentTrack() with
      // nothing to report. The caller (advanceToNextTrackLocked())
      // already treats a non-Advanced result plus no repeat-context as
      // "pause here".
      return TrackAdvanceResult::WrappedToStart;
    }

    queue.erase(queue.begin());

    if (queue.empty()) {
      BELL_LOG(debug, LOG_TAG,
               "Finished playing queue, switching to context tracks");
      isPlayingQueue = false;  // No more tracks in queue, switch to context
    }
    return TrackAdvanceResult::Advanced;
  }

  if (contextIndex) {
    auto res = ensureEnoughTracks();
    if (!res) {
      BELL_LOG(error, LOG_TAG, "Could not ensure tracks, err={}", res.error());
    }

    auto nextIndex = getOffsetIndex(1);
    if (nextIndex.has_value()) {
      contextIndex = *nextIndex;
      return TrackAdvanceResult::Advanced;
    }

    // End of context - wrap the cursor back to the start regardless;
    // caller decides whether that counts as a real advance (repeat-
    // context) or should stop there instead.
    BELL_LOG(debug, LOG_TAG, "At end of context, wrapping to start");
    contextIndex = cspot_proto::ContextIndex{0, 0};
    return TrackAdvanceResult::WrappedToStart;
  }

  return TrackAdvanceResult::Advanced;
}

bell::Result<> DefaultTrackQueueHandler::skipToPreviousTrack(
    const std::string& trackUri) {
  (void)trackUri;  //TODO: Implement skipping to specific track in context
  if (!contextIndex) {
    return {};  // No context index, cannot skip to previous track
  }

  if (isPlayingQueue) {
    isPlayingQueue = false;  // Switch back to context
    return {};
  }

  if ((contextIndex->page == 0) && (contextIndex->track == 0)) {
    BELL_LOG(debug, LOG_TAG,
             "At start of context, cannot skip to previous track");
    return {};
  }

  auto prevIndex = getOffsetIndex(-1);
  if (prevIndex.has_value()) {
    contextIndex = *prevIndex;
  } else {
    BELL_LOG(debug, LOG_TAG,
             "At beggining of context, cannot skip to prev track");
  }

  return {};
}

bell::Result<> DefaultTrackQueueHandler::enableShuffle(bool shuffle) {
  return {};  // TODO: Implement shuffle
}

std::optional<cspot_proto::ContextIndex>
DefaultTrackQueueHandler::getOffsetIndex(int32_t offset) const {
  if (!contextIndex) {
    return std::nullopt;
  }

  // Defensive: contextIndex should always refer to a real page once set,
  // but a real hardware crash (out-of-bounds contextPages[page] on an empty
  // vector, in the case where contextIndex got defaulted to {0, 0} against
  // a context that failed to actually populate any pages) showed this
  // invariant can't be fully trusted at every call site. Bounds-checking
  // here once, rather than at every caller, matches this function's own
  // existing contract of returning nullopt for any "can't get there" case.
  if (contextIndex->page >= contextPages.size()) {
    return std::nullopt;
  }

  int32_t totalOffset = static_cast<int32_t>(contextIndex->track) + offset;
  if (totalOffset < 0) {
    // Walk back as many previous pages as needed - a single-page step
    // isn't enough once the lookahead window (up to trackWindowLen tracks)
    // reaches past a short page. Without this loop a still-negative
    // totalOffset got cast straight to uint32_t (a huge number) and used
    // to index trackGids, an out-of-bounds vector access/crash near short
    // pages - mirrors the while loop already used below for positive
    // offsets crossing page boundaries.
    uint32_t remaining = static_cast<uint32_t>(-totalOffset);
    uint32_t page = contextIndex->page;

    while (remaining > 0) {
      if (page == 0) {
        return std::nullopt;  // No previous track available
      }
      page -= 1;

      auto pageSize = static_cast<uint32_t>(contextPages[page].trackGids.size());
      if (remaining <= pageSize) {
        return cspot_proto::ContextIndex{page, pageSize - remaining};
      }
      remaining -= pageSize;
    }

    return std::nullopt;
  }

  if (totalOffset >=
      static_cast<int32_t>(contextPages[contextIndex->page].trackGids.size())) {

    size_t skipByPages = 1;
    int32_t remainingOffset =
        totalOffset -
        static_cast<int32_t>(contextPages[contextIndex->page].trackGids.size());

    while (contextPages.size() > contextIndex->page + skipByPages) {
      if (remainingOffset <
          static_cast<int32_t>(contextPages[contextIndex->page + skipByPages]
                                   .trackGids.size())) {
        return cspot_proto::ContextIndex{
            static_cast<uint32_t>(contextIndex->page + skipByPages),
            static_cast<uint32_t>(remainingOffset),
        };
      }
      remainingOffset -= static_cast<int32_t>(
          contextPages[contextIndex->page + skipByPages].trackGids.size());
      skipByPages++;
    }
    return std::nullopt;  // No next page available
  }

  // Fits in current page
  return cspot_proto::ContextIndex{
      contextIndex->page,
      static_cast<uint32_t>(totalOffset),
  };
}

std::optional<cspot_proto::ContextIndex>
DefaultTrackQueueHandler::resolveFlatIndex(uint32_t flatIndex) const {
  uint32_t remaining = flatIndex;
  for (uint32_t page = 0; page < contextPages.size(); page++) {
    auto pageSize = static_cast<uint32_t>(contextPages[page].trackGids.size());
    if (remaining < pageSize) {
      return cspot_proto::ContextIndex{page, remaining};
    }
    remaining -= pageSize;
  }
  return std::nullopt;
}

void DefaultTrackQueueHandler::updateTrackWindows(bool forceNotify) {
  bool updated = forceNotify;

  // The next/previous-window diffing below only catches a change in what's
  // *around* the current track - not the current track itself. That's
  // normally masked by there also being a real "next" track most of the
  // time (which flips updated=true as a side effect), but a track with
  // nothing before/after it (an ad-hoc single/queue track with no context,
  // or a single-track context) never touches either window, so updated
  // stayed false and QUEUE_UPDATED - and the currentTrackId it carries -
  // never got posted at all. Real hardware symptom: a single-track,
  // no-context transfer silently never told StreamPlayer a track was
  // ready to load, leaving isBuffering=true forever and the remote client
  // stuck on "Connecting...".
  std::string newCurrentTrackUri;
  if (isPlayingQueue && !queue.empty()) {
    newCurrentTrackUri = queue[0].resolvedUri(contextIdType);
  } else if (contextIndex && contextIndex->page < contextPages.size() &&
             contextIndex->track <
                 contextPages[contextIndex->page].trackGids.size()) {
    SpotifyId trackId(
        contextIdType,
        contextPages[contextIndex->page].trackGids[contextIndex->track]);
    newCurrentTrackUri = trackId.uri;
  }
  if (newCurrentTrackUri != lastNotifiedCurrentTrackUri) {
    updated = true;
  }

  size_t queueOffset = queue.size();
  size_t offsetInQueue = isPlayingQueue ? 1 : 0;
  if (queueOffset > 0) {
    queueOffset -= offsetInQueue;  // Offset by one to not include current track
  }

  size_t highestValidIndex = 0;
  for (size_t x = 0; x < nextTracksWindow.size(); x++) {
    if (x < queueOffset) {
      highestValidIndex = x;
      auto& queueTrack = queue[x + offsetInQueue];
      std::string resolvedUri = queueTrack.resolvedUri(contextIdType);
      if (nextTracksWindow[x].uri != resolvedUri) {
        updated = true;

        // Construct ProvidedTrack from queue track
        nextTracksWindow[x].uri = resolvedUri;
        nextTracksWindow[x].uid = "q" + std::to_string(x);
        nextTracksWindow[x].provider = "queue";
        nextTracksWindow[x].gid.reset();
      }
    } else {
      int32_t trackOffset = x - queueOffset;
      auto offsetIndex = getOffsetIndex(trackOffset + 1);

      // offsetIndex is nullopt whenever the lookahead window runs past the
      // last fetched context page (getOffsetIndex()'s own "no next page
      // available" case) - dereferencing it before this check was a real
      // hardware crash (LoadProhibited, near-null vector access) reproduced
      // with a short context whose track count didn't fill the whole
      // nextTracksWindow lookahead.
      if (offsetIndex.has_value()) {
        auto& gid =
            contextPages[offsetIndex->page].trackGids[offsetIndex->track];

        if (!nextTracksWindow[x].gid || (nextTracksWindow[x].gid != gid)) {
          updated = true;

          // Construct ProvidedTrack from next context track
          SpotifyId trackId(contextIdType, gid);
          nextTracksWindow[x].uri = trackId.uri;
          nextTracksWindow[x].uid = "";
          nextTracksWindow[x].provider = "context";
          nextTracksWindow[x].gid = gid;
        }

        highestValidIndex = x;
      }
    }
  }

  // Clear all tracks over highest valid index
  for (size_t x = highestValidIndex + 1; x < nextTracksWindow.size(); x++) {
    if (!nextTracksWindow[x].uri.empty()) {
      updated = true;
      nextTracksWindow[x] = {};
    }
  }

  if (updated) {
    // Construct previous tracks
    for (size_t x = 0; x < previousTracksWindow.size(); x++) {
      int32_t trackOffset = -(static_cast<int32_t>(x) + 1);
      if (isPlayingQueue) {
        trackOffset +=
            1;  // Include current track in previous when playing queue
      }

      auto offsetIndex = getOffsetIndex(trackOffset);

      if (offsetIndex.has_value()) {
        auto& gid =
            contextPages[offsetIndex->page].trackGids[offsetIndex->track];

        // Construct ProvidedTrack from previous context track
        SpotifyId trackId(contextIdType, gid);
        previousTracksWindow[x].uri = trackId.uri;
        previousTracksWindow[x].uid = "";
        previousTracksWindow[x].provider = "context";
        previousTracksWindow[x].gid = gid;
      } else {
        previousTracksWindow[x] = {};
      }
    }

    // TODO: Make an event with updated tracks
    BELL_LOG(info, LOG_TAG, "Track windows updated");

    lastNotifiedCurrentTrackUri = newCurrentTrackUri;

    TrackQueueUpdate updateEvent{};
    auto nextTracksItr = nextTracks();
    for (auto& nextTrack : nextTracksItr) {
      if (nextTrack.uri.empty()) {
        break;
      }
      updateEvent.nextTracks.emplace_back(nextTrack.uri);
    }

    cspot_proto::ProvidedTrack& prevTrack = previousTracks().back();
    if (!prevTrack.uri.empty()) {
      updateEvent.previousTrackId = SpotifyId{prevTrack.uri};
    }

    if (isPlayingQueue && !queue.empty()) {
      std::string resolvedUri = queue[0].resolvedUri(contextIdType);
      if (!resolvedUri.empty()) {
        updateEvent.currentTrackId = SpotifyId{resolvedUri};
      }
    } else if (currentContextIndex()) {
      auto& trackGid =
          contextPages[contextIndex->page].trackGids[contextIndex->track];

      updateEvent.currentTrackId = SpotifyId{contextIdType, trackGid};
    }

    // Post queue updated event
    eventLoop->post(EventLoop::EventType::QUEUE_UPDATED, updateEvent);
    BELL_LOG(info, LOG_TAG, "Posted queue update event");
  }
}

tcb::span<cspot_proto::ProvidedTrack> DefaultTrackQueueHandler::nextTracks() {
  return {nextTracksWindow.data(), nextTracksWindow.size()};
}

tcb::span<cspot_proto::ProvidedTrack>
DefaultTrackQueueHandler::previousTracks() {
  return {previousTracksWindow.data(), previousTracksWindow.size()};
};

std::unique_ptr<TrackQueueHandler> cspot::createDefaultTrackQueueHandler(
    std::shared_ptr<SpClient> spClient, std::shared_ptr<EventLoop> eventLoop) {
  return std::make_unique<DefaultTrackQueueHandler>(std::move(spClient),
                                                    std::move(eventLoop));
}
