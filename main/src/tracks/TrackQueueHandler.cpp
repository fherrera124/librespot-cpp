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

  ContextPageParser pageParser;

  SpotifyIdType contextIdType = SpotifyIdType::Track;
  std::vector<FetchedContextPage> contextPages;
  std::optional<cspot_proto::ContextIndex> contextIndex;

  std::pair<std::string, std::string> targetTrackIds{};

  void resetContext();

  void onTrackParsed(uint32_t pageIndex, uint32_t trackIndex,
                     const cspot_proto::ContextTrack& track);

  void onPageMetadataParsed(uint32_t pageIndex,
                            const PageMetadata& pageMetadata);

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
  resetContext();
  targetTrackIds = {currentTrackUri.value_or(""), currentTrackUid.value_or("")};

  contextIdType = SpotifyIdType::Track;
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
    BELL_LOG(error, LOG_TAG,
             "Could not find current track in the given context");
    return bell::make_unexpected_errc(std::errc::invalid_argument);
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

  GidBytes trackGid{};
  size_t outLen = 16;
  auto uriDelimiter = track.uri.find_last_of(':');
  if (uriDelimiter == std::string::npos) {
    BELL_LOG(error, LOG_TAG, "Could not parse track uri={}", track.uri);
    return;
  }

  cspot::base62Decode(track.uri.substr(uriDelimiter + 1), trackGid.data(),
                      outLen);
  assert(outLen == trackGid.size());

  contextPages[pageIndex].trackGids.push_back(trackGid);
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

bell::Result<> DefaultTrackQueueHandler::fetchRootPage(
    const std::string& rootContextUri) {
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
}

std::optional<cspot_proto::ProvidedTrack>
DefaultTrackQueueHandler::currentTrack() {
  if (isPlayingQueue) {
    size_t indexInQueue = 0;  // TODO: impl
    auto& track = queue[indexInQueue];
    return cspot_proto::ProvidedTrack{
        .uri = track.uri,
        .uid = fmt::format("q{}", indexInQueue),
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

void DefaultTrackQueueHandler::setPlayingQueue(bool isPlayingQueue) {
  this->isPlayingQueue = isPlayingQueue;
}

void DefaultTrackQueueHandler::setQueue(
    const std::vector<cspot_proto::ContextTrack>& queue) {
  this->queue = queue;
}

std::unique_ptr<TrackQueueHandler> cspot::createDefaultTrackQueueHandler(
    std::shared_ptr<SpClient> spClient, std::shared_ptr<EventLoop> eventLoop) {
  return std::make_unique<DefaultTrackQueueHandler>(std::move(spClient),
                                                    std::move(eventLoop));
}
