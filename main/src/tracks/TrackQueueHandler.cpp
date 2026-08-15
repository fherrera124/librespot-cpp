#include "tracks/TrackQueueHandler.h"
#include <algorithm>
#include <random>
#include "bell/Result.h"
#include "bell/http/Client.h"
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

// Upper bound on tracks fetched by enableShuffle(true)'s full-context fetch.
const uint32_t maxShuffleTracks = 20000;

// Converts a spotify URI to a 16byte GID, returns nullopt on failure
// (unrecognized prefix, e.g. spotify:local:..., or malformed base62).
std::optional<std::array<std::byte, 16>> uriToGid(const std::string& uri) {
  auto parsed = cspot::SpotifyId::tryParse(uri);
  if (!parsed) {
    return std::nullopt;
  }
  return parsed->gid;
}

// Shuffles `order` in place; tracks `pinIndex` (an index into `order`)
// through the swaps so the caller can find where it ended up.
void fisherYatesShuffle(std::vector<cspot_proto::ContextIndex>& order,
                        size_t& pinIndex, std::default_random_engine& rng) {
  if (order.size() <= 1) {
    return;
  }
  for (size_t i = order.size() - 1; i > 0; i--) {
    size_t j = std::uniform_int_distribution<size_t>(0, i)(rng);
    std::swap(order[i], order[j]);
    if (i == pinIndex) {
      pinIndex = j;
    } else if (j == pinIndex) {
      pinIndex = i;
    }
  }
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

  void addToQueue(const cspot_proto::ContextTrack& track) override;

  void reorderQueue(
      const std::vector<cspot_proto::ContextTrack>& queuedTracksInOrder,
      const std::vector<cspot_proto::ContextTrack>& contextTracksInOrder)
      override;

  std::optional<cspot_proto::ProvidedTrack> currentTrack() override;

  std::optional<cspot_proto::ContextIndex> currentContextIndex() override;

  bell::Result<TrackAdvanceResult> skipToNextTrack(
      const std::string& targetTrackUri,
      const std::string& targetTrackUid) override;
  bell::Result<> skipToPreviousTrack(const std::string& trackUri) override;

  bell::Result<> enableShuffle(bool enable) override;

  tcb::span<cspot_proto::ProvidedTrack> nextTracks() override;
  tcb::span<cspot_proto::ProvidedTrack> previousTracks() override;

  void updateTrackWindows(bool forceNotify) override;

  void clearContext() override;

 private:
  const char* LOG_TAG = "TrackQueueHandler";

  std::shared_ptr<SpClient> spClient;
  std::shared_ptr<EventLoop> eventLoop;

  // Gid is always 16bytes
  using GidBytes = std::array<std::byte, 16>;

  struct FetchedContextPage {
    std::optional<std::string> url{};
    std::vector<GidBytes> trackGids{};
    // Parallel to trackGids, always pushed together in onTrackParsed().
    std::vector<std::string> trackUids{};
    std::vector<std::string> trackArtistUris{};
    std::vector<std::string> trackAlbumUris{};

    bool operator==(const FetchedContextPage& other) const {
      return url == other.url && trackGids == other.trackGids;
    }
  };

  std::vector<cspot_proto::ContextTrack> queue;
  bool isPlayingQueue = false;

  // Count of queue-front entries that came from a context reorder
  // (reorderQueue()), not a real add - contextIndex can't advance past
  // them yet since it also identifies the currently-playing track.
  // Applied in skipToNextTrack() once queue drains back to context.
  size_t pendingContextAdvanceCount = 0;

  std::string currentContextUri;

  ContextPageParser pageParser;

  SpotifyIdType contextIdType = SpotifyIdType::Track;
  std::vector<FetchedContextPage> contextPages;
  std::optional<cspot_proto::ContextIndex> contextIndex;

  // Permutation of {page,track} pairs; contextIndex always stays the
  // physical position, shuffled or not.
  bool shuffled = false;
  std::vector<cspot_proto::ContextIndex> shuffleOrder;
  size_t shufflePos = 0;

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

  // Moves the cursor by `offset`, keeping shufflePos in sync when
  // shuffled. Returns false (no-op) if offset runs past either end.
  bool advanceContextBy(int32_t offset);

  // Resets the cursor to the start of the sequence: shuffleOrder[0]
  // when shuffled, physical {0,0} otherwise.
  void resetContextToStart();

  // True at the start of the sequence: shufflePos==0 when shuffled,
  // physical page==0&&track==0 otherwise. Assumes contextIndex is set.
  bool atContextStart() const;

  // Relocates shufflePos to match contextIndex; turns shuffle off if
  // the track isn't in shuffleOrder.
  void syncShufflePosToContextIndex();

  // Fetches every remaining context page (ignoring trackFetchThreshold),
  // bounded by maxShuffleTracks.
  bell::Result<> fetchAllContextPages();

  // skipToNextTrack()'s explicit-target path: searches nextTracksWindow
  // (queue entries first, then context - the exact set of tracks the
  // client was last shown via next_tracks) for targetTrackUid/
  // targetTrackUri and jumps straight there, dropping any queue entries
  // skipped over along the way. Falls back to WrappedToStart (reset to
  // context start, queue untouched - mirrors go-librespot's own
  // TrySeek-failure fallback) when the target isn't found in the window,
  // e.g. a stale client message.
  bell::Result<TrackAdvanceResult> skipToTargetTrack(
      const std::string& targetTrackUri, const std::string& targetTrackUid);

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

  // True if the caller wants the context started from the beginning,
  // not pointed at a specific track.
  bool noTrackHintGiven =
      !currentTrackUri && !currentTrackUid && !currentTrackIndex;

  // In case we only have UID, we need to refetch the pages either way - we only keep the gids
  if (currentContextUri != contextUri || !haveFastPathTarget) {
    // New context, reset everything - resetContext() leaves
    // queue/isPlayingQueue alone (ad-hoc queue-only sessions rely on
    // that), so clear them here too.
    resetContext();
    queue.clear();
    isPlayingQueue = false;
    targetTrackIds = {currentTrackUri.value_or(""),
                      currentTrackUid.value_or("")};
    targetTrackIndex = currentTrackIndex;

    contextIdType = SpotifyId::getTypeFromContext(contextUri);

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

    // contextIndex above was set directly, not via advanceContextBy() -
    // relocate shufflePos to match.
    syncShufflePosToContextIndex();
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
    if (noTrackHintGiven) {
      BELL_LOG(info, LOG_TAG, "No specific track requested, starting at index=[0,0]");
    } else {
      BELL_LOG(error, LOG_TAG,
               "Could not find current track in the given context, default to zero");
    }
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

  auto& page = contextPages[pageIndex];
  page.trackGids.push_back(*trackGid);
  page.trackUids.push_back(track.uid);
  page.trackArtistUris.push_back(track.artistUri);
  page.trackAlbumUris.push_back(track.albumUri);
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

  while (bytesToRead > 0 && !stream->eof() && !stream->bad()) {
    size_t toRead = std::min(buffer.size(), bytesToRead);
    stream->read(reinterpret_cast<char*>(buffer.data()), toRead);

    if (stream->bad()) {
      break;
    }

    size_t bytesRead = stream->gcount();

    auto res = pageParser.feed(buffer.data(), bytesRead);
    bytesToRead -= bytesRead;

    if (!res) {
      BELL_LOG(error, LOG_TAG, "Error occured while parsing page, err={}",
               res.error());
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
  shuffled = false;
  shuffleOrder.clear();
  shufflePos = 0;
  pendingContextAdvanceCount = 0;
}

void DefaultTrackQueueHandler::clearContext() {
  resetContext();
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
        .uid = track.uid.empty() ? "q0" : track.uid,
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

    auto& page = contextPages[contextIndex->page];
    // Reconstruct spotify ID from the bare gid
    SpotifyId trackId(contextIdType, page.trackGids[contextIndex->track]);

    return cspot_proto::ProvidedTrack{
        .uri = trackId.uri,
        .uid = page.trackUids[contextIndex->track],
        .provider = "context",
        .artistUri = page.trackArtistUris[contextIndex->track],
        .albumUri = page.trackAlbumUris[contextIndex->track],
        .gid = std::nullopt,
    };
  }

  return std::nullopt;
}

void DefaultTrackQueueHandler::setQueue(
    const std::vector<cspot_proto::ContextTrack>& queue) {
  this->queue = queue;
  isPlayingQueue = false;
  // Wholesale replacement (e.g. from a transfer) - any catch-up owed to a
  // previous reorderQueue() no longer refers to what's actually queued.
  pendingContextAdvanceCount = 0;
}

void DefaultTrackQueueHandler::setPlayingQueue(bool isPlayingQueue) {
  this->isPlayingQueue = isPlayingQueue;
}

void DefaultTrackQueueHandler::addToQueue(
    const cspot_proto::ContextTrack& track) {
  queue.push_back(track);
}

void DefaultTrackQueueHandler::reorderQueue(
    const std::vector<cspot_proto::ContextTrack>& queuedTracksInOrder,
    const std::vector<cspot_proto::ContextTrack>& contextTracksInOrder) {
  if (isPlayingQueue && !queue.empty()) {
    queue.resize(1);
  } else {
    queue.clear();
  }
  queue.insert(queue.end(), queuedTracksInOrder.begin(),
               queuedTracksInOrder.end());

  pendingContextAdvanceCount = 0;

  if (contextTracksInOrder.empty()) {
    return;
  }

  // contextTracksInOrder may include a track foreign to this context (e.g.
  // dragged in from an album view), so it's absorbed into `queue` wholesale
  // (playable by uri/uid alone) instead of assuming every entry matches a
  // known context slot. contextIndex only advances once queue drains back
  // to context (skipToNextTrack()) - see pendingContextAdvanceCount.
  //
  // contextTracksInOrder.size() upper-bounds the real context matches - a
  // foreign entry only adds to the total, never replaces a real one.
  struct SlotData {
    std::string uri;
    std::string uid;
  };
  std::vector<SlotData> snapshot;
  snapshot.reserve(contextTracksInOrder.size());
  for (size_t i = 1; i <= contextTracksInOrder.size(); i++) {
    auto idx = getOffsetIndex(static_cast<int32_t>(i));
    if (!idx) {
      break;  // Context has fewer upcoming tracks than that - nothing more to match.
    }
    auto& page = contextPages[idx->page];
    snapshot.push_back({SpotifyId(contextIdType, page.trackGids[idx->track]).uri,
                        page.trackUids[idx->track]});
  }

  size_t realContextMatches = 0;
  for (auto& want : contextTracksInOrder) {
    // Erase each match as it's consumed - the same track can legitimately
    // repeat in the window (e.g. a playlist that repeats a song), and each
    // occurrence must bind to a distinct original slot rather than all of
    // them collapsing onto the first match.
    auto it = std::find_if(
        snapshot.begin(), snapshot.end(), [&](const SlotData& s) {
          return (!want.uid.empty() && s.uid == want.uid) ||
                 (!want.uri.empty() && s.uri == want.uri);
        });
    if (it == snapshot.end()) {
      continue;  // Foreign track, or ran past what we could match - fine.
    }
    realContextMatches++;
    snapshot.erase(it);
  }

  queue.insert(queue.end(), contextTracksInOrder.begin(),
               contextTracksInOrder.end());
  pendingContextAdvanceCount = realContextMatches;
}

std::optional<cspot_proto::ContextIndex>
DefaultTrackQueueHandler::currentContextIndex() {
  if (isPlayingQueue) {
    return std::nullopt;  // No context index when playing from queue
  }

  return contextIndex;
}

bell::Result<TrackAdvanceResult> DefaultTrackQueueHandler::skipToNextTrack(
    const std::string& targetTrackUri, const std::string& targetTrackUid) {
  if (!targetTrackUri.empty() || !targetTrackUid.empty()) {
    return skipToTargetTrack(targetTrackUri, targetTrackUid);
  }

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
      // Catch contextIndex up past the real context tracks reorderQueue()
      // absorbed into the queue that just drained.
      if (pendingContextAdvanceCount > 0) {
        if (!advanceContextBy(static_cast<int32_t>(pendingContextAdvanceCount))) {
          BELL_LOG(error, LOG_TAG,
                   "Could not advance context by {} after draining a "
                   "reordered queue segment",
                   pendingContextAdvanceCount);
        }
        pendingContextAdvanceCount = 0;
      }
    }
    return TrackAdvanceResult::Advanced;
  }

  if (contextIndex) {
    auto res = ensureEnoughTracks();
    if (!res) {
      BELL_LOG(error, LOG_TAG, "Could not ensure tracks, err={}", res.error());
    }

    if (advanceContextBy(1)) {
      return TrackAdvanceResult::Advanced;
    }

    // End of context - wrap the cursor back to the start regardless;
    // caller decides whether that counts as a real advance (repeat-
    // context) or should stop there instead.
    BELL_LOG(debug, LOG_TAG, "At end of context, wrapping to start");
    resetContextToStart();
    return TrackAdvanceResult::WrappedToStart;
  }

  // Nothing loaded at all (no queue, no context) - same "nothing to
  // advance to" case as the sole-queue-track check above.
  return TrackAdvanceResult::WrappedToStart;
}

bell::Result<TrackAdvanceResult> DefaultTrackQueueHandler::skipToTargetTrack(
    const std::string& targetTrackUri, const std::string& targetTrackUid) {
  // nextTracksWindow is exactly the next_tracks list last PUT to Spotify -
  // the only tracks the client could have shown (and so the only tracks a
  // remote skip_next's "track" field could legitimately name). Searching
  // it directly, instead of re-deriving uid/uri from queue/contextPages,
  // guarantees this can't drift from what the client actually saw.
  for (size_t x = 0; x < nextTracksWindow.size(); x++) {
    auto& candidate = nextTracksWindow[x];
    if (candidate.uri.empty()) {
      break;  // unpopulated tail of the window
    }

    bool matches = (!targetTrackUid.empty() && candidate.uid == targetTrackUid) ||
                   (!targetTrackUri.empty() && candidate.uri == targetTrackUri);
    if (!matches) {
      continue;
    }

    size_t offsetInQueue = isPlayingQueue ? 1 : 0;
    if (candidate.provider == "queue") {
      // Inverts updateTrackWindows()'s own construction of this same
      // window slot (nextTracksWindow[x] <- queue[x + offsetInQueue]):
      // drop the current track (if playing from queue) plus every queue
      // entry skipped over to reach the target, which becomes the new
      // queue[0].
      queue.erase(queue.begin(), queue.begin() + (x + offsetInQueue));
      setPlayingQueue(true);
    } else {
      // Context portion of the window starts right after the queue
      // portion (queueOffset entries) - same split updateTrackWindows()
      // uses, inverted here to recover the context offset this window
      // slot was built from.
      size_t queueOffset =
          queue.size() > offsetInQueue ? queue.size() - offsetInQueue : 0;
      if (!advanceContextBy(static_cast<int32_t>(x - queueOffset) + 1)) {
        // Shouldn't happen - the window was built from this same
        // getOffsetIndex() call - but degrade to "not found" rather than
        // leaving contextIndex unset.
        break;
      }
      // Skipping straight into context discards whatever's left of the
      // manual queue, same as skipping past it one track at a time would.
      queue.clear();
      setPlayingQueue(false);
      // advanceContextBy() above already lands on the exact target -
      // discard any pending catch-up so it isn't double-applied.
      pendingContextAdvanceCount = 0;
    }
    return TrackAdvanceResult::Advanced;
  }

  // Target not found in the exposed window (stale client state) - mirrors
  // go-librespot's own TrySeek-failure fallback (tracks/tracks.go
  // moveStart): reset the context cursor to the start, leave the manual
  // queue untouched.
  BELL_LOG(debug, LOG_TAG,
           "Could not find target track in next-tracks window, wrapping to "
           "start (uri={}, uid={})",
           targetTrackUri, targetTrackUid);
  resetContextToStart();
  return TrackAdvanceResult::WrappedToStart;
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

  if (atContextStart()) {
    BELL_LOG(debug, LOG_TAG,
             "At start of context, cannot skip to previous track");
    return {};
  }

  if (!advanceContextBy(-1)) {
    BELL_LOG(debug, LOG_TAG,
             "At beggining of context, cannot skip to prev track");
  }

  return {};
}

bell::Result<> DefaultTrackQueueHandler::fetchAllContextPages() {
  size_t totalTracks = 0;
  for (auto& page : contextPages) {
    totalTracks += page.trackGids.size();
  }

  // Index-based, not a range-for: fetchContextPage() can append to
  // contextPages mid-loop, invalidating cached iterators.
  size_t pageIndex = 0;
  while (pageIndex < contextPages.size()) {
    if (contextPages[pageIndex].trackGids.empty()) {
      auto res = fetchContextPage(contextPages[pageIndex]);
      if (!res) {
        BELL_LOG(error, LOG_TAG,
                 "Could not fetch context page while shuffling, err={}",
                 res.error());
        return nonstd::make_unexpected(res.error());
      }
      totalTracks += contextPages[pageIndex].trackGids.size();
      if (totalTracks > maxShuffleTracks) {
        BELL_LOG(error, LOG_TAG,
                 "Context too large to shuffle (>{} tracks), aborting",
                 maxShuffleTracks);
        return bell::make_unexpected_errc(std::errc::value_too_large);
      }
    }
    pageIndex++;
  }
  return {};
}

bell::Result<> DefaultTrackQueueHandler::enableShuffle(bool shuffle) {
  if (shuffle == shuffled) {
    return {};
  }

  if (!shuffle) {
    shuffled = false;
    shuffleOrder.clear();
    shufflePos = 0;
    return {};  // contextIndex is already the real physical position
  }

  if (!contextIndex) {
    return {};  // nothing loaded (queue-only/ad-hoc session) to shuffle
  }

  auto fetchRes = fetchAllContextPages();
  if (!fetchRes) {
    return fetchRes;
  }

  std::vector<cspot_proto::ContextIndex> order;
  size_t pinIndex = 0;
  bool foundPin = false;
  for (uint32_t page = 0; page < contextPages.size(); page++) {
    uint32_t trackCount =
        static_cast<uint32_t>(contextPages[page].trackGids.size());
    for (uint32_t track = 0; track < trackCount; track++) {
      if (page == contextIndex->page && track == contextIndex->track) {
        pinIndex = order.size();
        foundPin = true;
      }
      order.push_back({page, track});
    }
  }

  if (!foundPin) {
    // Shouldn't happen - contextIndex always points at an already-fetched
    // track. Degrades to a no-op instead of leaving state inconsistent.
    BELL_LOG(error, LOG_TAG, "Could not locate current track while shuffling");
    return {};
  }

  static std::default_random_engine rng{std::random_device{}()};
  fisherYatesShuffle(order, pinIndex, rng);

  if (pinIndex != 0) {
    // Keeps the currently playing track at position 0; the rest is
    // shuffled around it.
    std::swap(order[0], order[pinIndex]);
  }

  shuffleOrder = std::move(order);
  shufflePos = 0;
  shuffled = true;
  return {};
}

bool DefaultTrackQueueHandler::advanceContextBy(int32_t offset) {
  auto next = getOffsetIndex(offset);
  if (!next) {
    return false;
  }
  contextIndex = *next;
  if (shuffled) {
    shufflePos =
        static_cast<size_t>(static_cast<int64_t>(shufflePos) + offset);
  }
  return true;
}

void DefaultTrackQueueHandler::resetContextToStart() {
  if (shuffled && !shuffleOrder.empty()) {
    shufflePos = 0;
    contextIndex = shuffleOrder[0];
  } else {
    contextIndex = cspot_proto::ContextIndex{0, 0};
  }
}

bool DefaultTrackQueueHandler::atContextStart() const {
  if (shuffled) {
    return shufflePos == 0;
  }
  return contextIndex->page == 0 && contextIndex->track == 0;
}

void DefaultTrackQueueHandler::syncShufflePosToContextIndex() {
  if (!shuffled || !contextIndex) {
    return;
  }
  auto it = std::find_if(shuffleOrder.begin(), shuffleOrder.end(),
                         [this](const cspot_proto::ContextIndex& idx) {
                           return idx.page == contextIndex->page &&
                                  idx.track == contextIndex->track;
                         });
  if (it != shuffleOrder.end()) {
    shufflePos = static_cast<size_t>(std::distance(shuffleOrder.begin(), it));
  } else {
    // Track set changed since shuffleOrder was built - it's stale, so
    // shuffle turns off instead of leaving shufflePos pointed at nothing.
    shuffled = false;
    shuffleOrder.clear();
  }
}

std::optional<cspot_proto::ContextIndex>
DefaultTrackQueueHandler::getOffsetIndex(int32_t offset) const {
  if (!contextIndex) {
    return std::nullopt;
  }

  if (shuffled) {
    // Walks shuffleOrder instead of contextPages.
    int64_t newPos = static_cast<int64_t>(shufflePos) + offset;
    if (newPos < 0 || newPos >= static_cast<int64_t>(shuffleOrder.size())) {
      return std::nullopt;
    }
    return shuffleOrder[static_cast<size_t>(newPos)];
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
        nextTracksWindow[x].uid =
            queueTrack.uid.empty() ? "q" + std::to_string(x) : queueTrack.uid;
        nextTracksWindow[x].provider = "queue";
        nextTracksWindow[x].gid.reset();
        // Marks this as queue-provided for the client's later set_queue
        // echo (handleSetQueueCommandLocked() keys off it).
        nextTracksWindow[x].metadata = {{"is_queued", "true"}};
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
        auto& page = contextPages[offsetIndex->page];
        auto& gid = page.trackGids[offsetIndex->track];

        if (!nextTracksWindow[x].gid || (nextTracksWindow[x].gid != gid)) {
          updated = true;

          // Construct ProvidedTrack from next context track
          SpotifyId trackId(contextIdType, gid);
          nextTracksWindow[x].uri = trackId.uri;
          nextTracksWindow[x].uid = page.trackUids[offsetIndex->track];
          nextTracksWindow[x].provider = "context";
          nextTracksWindow[x].artistUri =
              page.trackArtistUris[offsetIndex->track];
          nextTracksWindow[x].albumUri =
              page.trackAlbumUris[offsetIndex->track];
          nextTracksWindow[x].gid = gid;
          // nextTracksWindow[x] is reused across calls - clear a leftover
          // is_queued from when this slot was last a queue entry.
          nextTracksWindow[x].metadata.clear();
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
        auto& page = contextPages[offsetIndex->page];
        auto& gid = page.trackGids[offsetIndex->track];

        // Construct ProvidedTrack from previous context track
        SpotifyId trackId(contextIdType, gid);
        previousTracksWindow[x].uri = trackId.uri;
        previousTracksWindow[x].uid = page.trackUids[offsetIndex->track];
        previousTracksWindow[x].provider = "context";
        previousTracksWindow[x].artistUri =
            page.trackArtistUris[offsetIndex->track];
        previousTracksWindow[x].albumUri =
            page.trackAlbumUris[offsetIndex->track];
        previousTracksWindow[x].gid = gid;
      } else {
        previousTracksWindow[x] = {};
      }
    }

    BELL_LOG(info, LOG_TAG, "Track windows updated");

    lastNotifiedCurrentTrackUri = newCurrentTrackUri;

    TrackQueueUpdate updateEvent{};
    if (isPlayingQueue && !queue.empty()) {
      std::string resolvedUri = queue[0].resolvedUri(contextIdType);
      // resolvedUri may be a well-formed but unrecognized uri (e.g. a
      // spotify:local: track) - SpotifyId{} would throw on that, so use
      // tryParse() instead.
      if (!resolvedUri.empty()) {
        updateEvent.currentTrackId = SpotifyId::tryParse(resolvedUri);
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
