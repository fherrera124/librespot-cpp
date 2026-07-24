#include "tracks/TrackQueueHandler.h"
#include "bell/Result.h"
#include "bell/http/Client.h"
#include "crypto/Base62.h"
#include "events/EventLoop.h"
#include "proto/ConnectPb.h"
#include "proto/SpotifyId.h"
#include "tl/expected.hpp"
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
      std::optional<std::string> currentTrackUid) override;

  void setQueue(const std::vector<cspot_proto::ContextTrack>& queue) override;

  void setPlayingQueue(bool isPlayingQueue) override;

  std::optional<cspot_proto::ProvidedTrack> currentTrack() override;

  std::optional<cspot_proto::ContextIndex> currentContextIndex() override;

  bell::Result<> skipToNextTrack(const std::string& trackUri) override;
  bell::Result<> skipToPreviousTrack(const std::string& trackUri) override;

  bell::Result<> enableShuffle(bool enable) override;

  tcb::span<cspot_proto::ProvidedTrack> nextTracks() override;
  tcb::span<cspot_proto::ProvidedTrack> previousTracks() override;

  void updateTrackWindows() override;

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

  // Current window of tracks, used for providing next/previous tracks to UI and track player
  std::array<cspot_proto::ProvidedTrack, trackWindowLen> nextTracksWindow{};
  std::array<cspot_proto::ProvidedTrack, trackWindowLen> previousTracksWindow{};

  void resetContext();

  void onTrackParsed(uint32_t pageIndex, uint32_t trackIndex,
                     const cspot_proto::ContextTrack& track);

  void onPageMetadataParsed(uint32_t pageIndex,
                            const PageMetadata& pageMetadata);

  std::optional<cspot_proto::ContextIndex> getOffsetIndex(int32_t offset) const;

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
    std::optional<std::string> currentTrackUid) {
  // In case we only have UID, we need to refetch the pages either way - we only keep the gids
  if (currentContextUri != contextUri || !currentTrackUri.has_value()) {
    // New context, reset everything
    resetContext();
    targetTrackIds = {currentTrackUri.value_or(""),
                      currentTrackUid.value_or("")};

    contextIdType = SpotifyIdType::Track;

    if (!currentTrackUid && !currentTrackUri) {
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
      return tl::make_unexpected(res.error());
    }
  } else {
    // Same context, only reset the current index & queue
    contextIndex.reset();
    queue.clear();
    isPlayingQueue = false;

    targetTrackIds = {currentTrackUri.value_or(""),
                      currentTrackUid.value_or("")};

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
      return tl::make_unexpected(res.error());
    }
  }

  if (contextIndex.has_value()) {
    BELL_LOG(info, LOG_TAG, "Found current track at index=[{},{}]",
             contextIndex->track, contextIndex->page);
  } else {
    BELL_LOG(
        error, LOG_TAG,
        "Could not find current track in the given context, default to zero");
    contextIndex = {
        0,
        0,
    };  // Default to start if we could not find the current track
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

  assert(contextPages.size() > contextIndex->page);
  assert(contextPages[contextIndex->page].trackGids.size() >
         contextIndex->track);
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
      return tl::make_unexpected(res.error());
    }
    nextTracksCount += contextPages[nextPageIndex].trackGids.size();
    nextPageIndex++;
  }

  return {};
}

bell::Result<> DefaultTrackQueueHandler::fetchRootPage(
    const std::string& rootContextUri) {
  this->currentContextUri = rootContextUri;
  BELL_LOG(info, LOG_TAG, "Fetching context root, uri={}", rootContextUri);
  pageParser.reset();
  auto res = spClient->contextResolve(rootContextUri);
  if (!res) {
    return tl::make_unexpected(res.error());
  }

  return feedResponseToParser(*res);
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
  pageUrl = pageUrl.substr(5);

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
    return tl::make_unexpected(res.error());
  }

  return feedResponseToParser(*res);
}

bell::Result<> DefaultTrackQueueHandler::feedResponseToParser(
    bell::HTTPResponse& response) {
  if (response.statusCode != 200) {
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
      return tl::make_unexpected(res.error());
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
        .uri = track.uri,
        .uid = "q0",
        .provider = "queue",
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

bell::Result<> DefaultTrackQueueHandler::skipToNextTrack(
    const std::string& trackUri) {
  (void)trackUri;  //TODO: Implement skipping to specific track in context

  if (!isPlayingQueue && !queue.empty()) {
    setPlayingQueue(true);
    return {};
  }

  if (isPlayingQueue && !queue.empty()) {
    queue.erase(queue.begin());

    if (queue.empty()) {
      BELL_LOG(debug, LOG_TAG,
               "Finished playing queue, switching to context tracks");
      isPlayingQueue = false;  // No more tracks in queue, switch to context
    }
  } else if (contextIndex) {
    auto res = ensureEnoughTracks();
    if (!res) {
      BELL_LOG(error, LOG_TAG, "Could not ensure tracks, err={}", res.error());
    }

    auto nextIndex = getOffsetIndex(1);
    if (nextIndex.has_value()) {
      contextIndex = *nextIndex;
    } else {
      BELL_LOG(debug, LOG_TAG, "At end of context, cannot skip to next track");
    }
  }

  return {};
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

  int32_t totalOffset = static_cast<int32_t>(contextIndex->track) + offset;
  if (totalOffset < 0) {
    if (contextIndex->page == 0) {
      return std::nullopt;  // No previous track available
    }

    return cspot_proto::ContextIndex{
        static_cast<uint32_t>(contextIndex->page - 1),
        static_cast<uint32_t>(
            contextPages[contextIndex->page - 1].trackGids.size() +
            totalOffset),
    };
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

void DefaultTrackQueueHandler::updateTrackWindows() {
  bool updated = false;

  size_t queueOffset = queue.size();
  size_t offsetInQueue = isPlayingQueue ? 1 : 0;
  if (queueOffset > 0) {
    queueOffset -= offsetInQueue;  // Offset by one to not include current track
  }

  size_t highestValidIndex = 0;
  for (size_t x = 0; x < nextTracksWindow.size(); x++) {
    if (x < queueOffset) {
      highestValidIndex = x;
      if (nextTracksWindow[x].uri != queue[x + offsetInQueue].uri) {
        updated = true;

        // Construct ProvidedTrack from queue track
        nextTracksWindow[x].uri = queue[x + offsetInQueue].uri;
        nextTracksWindow[x].uid = "q" + std::to_string(x);
        nextTracksWindow[x].provider = "queue";
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

    TrackQueueUpdate updateEvent{};
    auto nextTracksItr = nextTracks();
    for (auto& nextTrack : nextTracksItr) {
      if (nextTrack.uri.empty()) {
        break;
      }
      updateEvent.nextTracks.emplace_back(nextTrack.uri);
    }

    std::string& prevTrackUri = previousTracks().back().uri;
    if (!prevTrackUri.empty()) {
      updateEvent.previousTrackId = SpotifyId{prevTrackUri};
    }

    if (isPlayingQueue && !queue.empty()) {
      updateEvent.currentTrackId = SpotifyId{queue[0].uri};
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
