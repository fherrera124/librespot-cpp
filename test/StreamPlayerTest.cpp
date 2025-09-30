#include <doctest/doctest.h>
#include <tao/json.hpp>
#include <trompeloeil.hpp>
#include "events/EventLoop.h"
#include "events/EventModels.h"
#include "mocks/MockAudioDecoder.h"
#include "mocks/MockFileProvider.h"
#include "mocks/MockSpClient.h"
#include "proto/SpotifyId.h"
#include "tracks/StreamPlayer.h"
#include "tracks/TrackQueueHandler.h"

TEST_CASE("StreamPlayer tests") {
  using trompeloeil::_;  // wild card for matching any value

  auto eventLoop = std::make_shared<cspot::EventLoop>();

  auto mockFileProvider = std::make_unique<MockFileProvider>();
  auto* rawMockFileProvider = mockFileProvider.get();
  auto mockAudioDecoder = std::make_unique<MockAudioDecoder>();
  // auto* rawMockAudioDecoder = mockAudioDecoder.get();

  auto streamPlayer = std::make_unique<cspot::StreamPlayer>(
      eventLoop, std::move(mockFileProvider), std::move(mockAudioDecoder));

  SUBCASE("Requests queued files from file provider") {
    // Ensure that the StreamPlayer requests the correct files from the file provider
    REQUIRE_CALL(*rawMockFileProvider, provideTrack(_)).TIMES(3);

    cspot::TrackQueueUpdate queueUpdate = {
        .previousTrackId = std::nullopt,
        .nextTracks =
            {
                cspot::SpotifyId("spotify:track:0987654321"),
                cspot::SpotifyId("spotify:track:1122334455"),
                cspot::SpotifyId("spotify:track:666"),
            },
        .currentTrackId = cspot::SpotifyId("spotify:track:1234567890"),
    };

    eventLoop->post(cspot::EventLoop::EventType::QUEUE_UPDATED, queueUpdate);
  }

  // SUBCASE("Resolves playlist context and sends correct events") {
  //   // Require one call to contextResolve with the specific playlist URI
  //   REQUIRE_CALL(*rawMockSpClient, contextResolve(_))
  //       .WITH(_1 == "spotify:playlist:37i9dQZF1FbDk7WZs0loln")
  //       .RETURN(createMockResponseTestFile(200, "context-resolve/daylist.json",
  //                                          "application/json"));
  //   auto res = queueHandler->loadContext(
  //       "spotify:playlist:37i9dQZF1FbDk7WZs0loln", std::nullopt,
  //       "62623566653963383163363961396434");
  //   CHECK(res);  // Ensure the loadContext call was successful

  //   queueHandler->updateTrackWindows();

  //   auto currentTrackContext = queueHandler->currentContextIndex();
  //   CHECK(currentTrackContext.has_value());
  //   CHECK(currentTrackContext->track == 9);  // 10th track, zero-based index
  //   CHECK(currentTrackContext->page ==
  //         0);  // No pagination in a playlist context

  //   auto currentTrack = queueHandler->currentTrack();
  //   CHECK(currentTrack.has_value());
  //   CHECK(currentTrack->uri == "spotify:track:5jUTUtASaCSvod7opmnTlH");

  //   // Ensure next tracks were populated
  //   CHECK(queueHandler->nextTracks()[0].uri ==
  //         "spotify:track:4OLhh8d7T5RUXrQZMTX7eh");
  //   CHECK(queueHandler->nextTracks().size() == 6);

  //   // Check if next moves to the next track correctly
  //   res = queueHandler->skipToNextTrack();
  //   CHECK(res);  // Ensure the skipToNextTrack call was successful

  //   CHECK(queueHandler->currentTrack()->uri ==
  //         "spotify:track:4OLhh8d7T5RUXrQZMTX7eh");
  // }

  // SUBCASE("Manual queue works properly") {
  //   REQUIRE_CALL(*rawMockSpClient, contextResolve(_))
  //       .WITH(_1 == "spotify:playlist:37i9dQZF1FbDk7WZs0loln")
  //       .RETURN(createMockResponseTestFile(200, "context-resolve/daylist.json",
  //                                          "application/json"));
  //   auto res = queueHandler->loadContext(
  //       "spotify:playlist:37i9dQZF1FbDk7WZs0loln", std::nullopt,
  //       "62623566653963383163363961396434");
  //   CHECK(res);  // Ensure the loadContext call was successful

  //   std::vector<cspot_proto::ContextTrack> queue = {
  //       {.uri = "spotify:track:2zWayiqyrdrqZTAAqnUM8y"},
  //       {.uri = "spotify:track:5sc8Hhp7hBwDbtsg4aGVcf"},
  //       {.uri = "spotify:track:6JV1qSUJh03F3XnNSKX2CY"},
  //   };

  //   // Insert a queue manually
  //   queueHandler->setQueue(queue);

  //   queueHandler->updateTrackWindows();

  //   // Just setting the queue should not change the current track
  //   CHECK(queueHandler->currentTrack()->uri ==
  //         "spotify:track:5jUTUtASaCSvod7opmnTlH");

  //   queueHandler->setPlayingQueue(true);
  //   queueHandler->updateTrackWindows();

  //   CHECK(
  //       queueHandler->currentTrack()->uri ==
  //       "spotify:track:2zWayiqyrdrqZTAAqnUM8y");  // First track in the manual queue

  //   // No context index when playing a manual queue
  //   CHECK(!queueHandler->currentContextIndex());

  //   CHECK(queueHandler->nextTracks()[0].uri ==
  //         "spotify:track:5sc8Hhp7hBwDbtsg4aGVcf");
  //   CHECK(queueHandler->nextTracks().size() == 6);

  //   res = queueHandler->skipToNextTrack();
  //   CHECK(res);  // Ensure the skipToNextTrack call was successful

  //   // Skipped to the second track in the manual queue
  //   CHECK(queueHandler->currentTrack()->uri ==
  //         "spotify:track:5sc8Hhp7hBwDbtsg4aGVcf");

  //   // Skipping back when playing a manual queue should go to the last track in the context
  //   CHECK(queueHandler->skipToPreviousTrack());
  //   CHECK(queueHandler->currentTrack()->uri ==
  //         "spotify:track:5jUTUtASaCSvod7opmnTlH");

  //   queueHandler->updateTrackWindows();

  //   // Next track should still be the queued track
  //   CHECK(queueHandler->nextTracks()[0].uri ==
  //         "spotify:track:5sc8Hhp7hBwDbtsg4aGVcf");
  // }

  // SUBCASE("Handles multi-page contexts") {
  //   // Require one call to contextResolve with the specific artist URI
  //   REQUIRE_CALL(*rawMockSpClient, contextResolve(_))
  //       .WITH(_1 == "spotify:artist:4CvTDPKA6W06DRfBnZKrau")
  //       .RETURN(createMockResponseTestFile(
  //           200, "context-resolve/artist/root.json", "application/json"));
  //   auto res = queueHandler->loadContext(
  //       "spotify:artist:4CvTDPKA6W06DRfBnZKrau", std::nullopt,
  //       "toptrack3bXH9y487cnLfA6VWXefaB");
  //   CHECK(res);  // Ensure the loadContext call was successful

  //   // 2nd track of the first page
  //   CHECK(queueHandler->currentContextIndex()->track == 1);
  //   CHECK(queueHandler->currentContextIndex()->page == 0);  // Page 0
  //   // The first page of artist is top 10 tracks, skip all the way to the first album
  //   for (int x = 0; x < 9; x++) {
  //     res = queueHandler->skipToNextTrack();
  //     CHECK(res);  // Ensure the skipToNextTrack call was successful
  //   }

  //   CHECK(
  //       queueHandler->currentTrack()->uri ==
  //       "spotify:track:1dJx3jat0jXUxBari5KSiw");  // First track of the first album
  //   CHECK(queueHandler->currentContextIndex()->track == 0);
  //   CHECK(queueHandler->currentContextIndex()->page ==
  //         1);  // Page 1 (second page)

  //   const int songsInFirstAlbum = 12;
  //   const int refetchThreshold = 8;

  //   // Require a call to fetch the next page when we are close to the end of the current page
  //   REQUIRE_CALL(*rawMockSpClient, rawRequest(_))
  //       .WITH(_1 ==
  //             "artistplaycontext/v1/page/spotify/album/2DpwYA58Qs00nRl59i4eiL/"
  //             "km_artist")
  //       .RETURN(createMockResponseTestFile(
  //           200, "context-resolve/artist/page.json", "application/json"));

  //   for (int x = 0; x <= songsInFirstAlbum - refetchThreshold; x++) {
  //     res = queueHandler->skipToNextTrack();
  //     CHECK(res);  // Ensure the skipToNextTrack call was successful
  //   }
  // }
}
