#include <doctest/doctest.h>

#include <algorithm>
#include <memory>

#include "tracks/TrackQueueHandler.h"

namespace {
const std::string firstUri = "spotify:track:2zWayiqyrdrqZTAAqnUM8y";
const std::string secondUri = "spotify:track:5sc8Hhp7hBwDbtsg4aGVcf";
const std::string thirdUri = "spotify:track:6JV1qSUJh03F3XnNSKX2CY";

struct QueueFixture {
  std::shared_ptr<cspot::EventLoop> loop = std::make_shared<cspot::EventLoop>();
  std::unique_ptr<cspot::TrackQueueHandler> queue =
      cspot::createDefaultTrackQueueHandler(nullptr, loop);
  size_t notifications = 0;

  QueueFixture() {
    // Dispatch explicitly to check notifications without thread timing.
    loop->stopTask();
    loop->registerHandler(cspot::EventLoop::EventType::QUEUE_UPDATED,
        [this](cspot::EventLoop::Event&&) { ++notifications; });
  }

  void update() {
    queue->updateTrackWindows();
    loop->processEvents(1);
  }

  bool emptyFrom(size_t index) {
    auto tracks = queue->nextTracks();
    return std::all_of(tracks.begin() + index, tracks.end(), [](const auto& t) {
      return t.uri.empty() && t.uid.empty() && t.provider.empty() &&
             !t.gid && t.metadata.empty();
    });
  }
};
}  // namespace

TEST_CASE("Removing the last upcoming queue entry clears slot zero and notifies") {
  QueueFixture f;
  f.queue->setQueue({{.uri = firstUri}, {.uri = secondUri}, {.uri = thirdUri}});
  f.update();
  REQUIRE(f.queue->nextTracks()[0].uri == firstUri);
  REQUIRE(f.queue->nextTracks()[2].uri == thirdUri);
  f.queue->setQueue({{.uri = secondUri}});
  f.update();
  CHECK(f.queue->nextTracks()[0].uri == secondUri);
  CHECK(f.emptyFrom(1));
  auto notifications = f.notifications;
  f.queue->setQueue({});
  f.update();
  CHECK(f.emptyFrom(0));
  CHECK(f.notifications == notifications + 1);
  f.update();
  CHECK(f.notifications == notifications + 1);
}

TEST_CASE("Playing the final manual queue track leaves no upcoming entry") {
  QueueFixture f;
  f.queue->setQueue({{.uri = firstUri}, {.uri = secondUri}});
  f.queue->setPlayingQueue(true);
  f.update();
  REQUIRE(f.queue->nextTracks()[0].uri == secondUri);
  REQUIRE(f.queue->skipToNextTrack());
  f.update();
  REQUIRE(f.queue->currentTrack());
  CHECK(f.queue->currentTrack()->uri == secondUri);
  CHECK(f.emptyFrom(0));
}

TEST_CASE("Advancing to the final context track clears upcoming tracks") {
  QueueFixture f;
  cspot_proto::ContextPage page;
  page.tracks = {{.uri = firstUri}, {.uri = secondUri}, {.uri = thirdUri}};
  REQUIRE(f.queue->loadContext("spotify:album:2DpwYA58Qs00nRl59i4eiL",
                               std::nullopt, std::nullopt, std::nullopt, {page}));
  f.update();
  REQUIRE(f.queue->nextTracks()[0].uri == secondUri);
  REQUIRE(f.queue->nextTracks()[1].uri == thirdUri);
  REQUIRE(f.queue->skipToNextTrack());
  f.update();
  CHECK(f.queue->nextTracks()[0].uri == thirdUri);
  CHECK(f.emptyFrom(1));
  REQUIRE(f.queue->skipToNextTrack());
  f.update();
  CHECK(f.emptyFrom(0));
  REQUIRE(f.queue->currentTrack());
  CHECK(f.queue->currentTrack()->uri == thirdUri);
  REQUIRE(f.queue->skipToPreviousTrack());
  f.update();
  CHECK(f.queue->nextTracks()[0].uri == thirdUri);
  CHECK(f.emptyFrom(1));
}
