#include "TrackLoader.h"
#include <pb_decode.h>

#include "ApResolve.h"
#include "CSpotContext.h"
#include "Login5Client.h"
#include "Logger.h"
#include "WrappedSemaphore.h"

using namespace cspot;

TrackLoader::TrackLoader(std::shared_ptr<cspot::Context> ctx,
                         std::shared_ptr<cspot::Login5Client> login5,
                         std::shared_ptr<bell::WrappedSemaphore> processSemaphore,
                         SnapshotFn snapshotPreloaded, TopUpFn tryTopUpLookahead)
    : bell::Task("CSpotTrackLoader", 1024 * 32, 2, 1), ctx(ctx),
      login5(login5), processSemaphore(processSemaphore),
      snapshotPreloaded(snapshotPreloaded),
      tryTopUpLookahead(tryTopUpLookahead) {
  pbTrack = Track_init_zero;
  pbEpisode = Episode_init_zero;

  startTask();
}

TrackLoader::~TrackLoader() {
  stopTask();

  pb_release(Track_fields, &pbTrack);
  pb_release(Episode_fields, &pbEpisode);
}

void TrackLoader::runTask() {
  while (!shouldStop()) {
    processSemaphore->twait(100);

    // Make sure we have the newest access + client token - the same pair
    // PlayerEngine's own sendPutStateRequest() refreshes together
    // (PlayerEngine.cpp), since Login5Client treats them as a matched set
    // (a fresh login can rotate both).
    accessKey = login5->getToken();
    clientToken = login5->getClientToken();

    // Resolved once, then cached for this task's entire lifetime - see
    // this class's own spclientHost comment (TrackLoader.h). Failure just
    // leaves it empty; stepLoadCDNUrl() already no-ops until it's set, so
    // the next tick retries the resolve for free rather than needing its
    // own separate backoff.
    if (spclientHost.empty()) {
      try {
        spclientHost = ApResolve("").fetchFirstSpclientAddress();
      } catch (const std::exception& e) {
        CSPOT_LOG(error, "Failed to resolve spclient host for CDN access: %s",
                 e.what());
      }
    }

    auto snapshot = snapshotPreloaded();  // TrackQueue locks internally

    for (auto& track : snapshot) {
      if (track) {
        processTrack(track);
      }
    }
  }
}

void TrackLoader::stopTask() {
  stopAndWait();
}

void TrackLoader::onStopRequested() {
  processSemaphore->give();
}

void TrackLoader::processTrack(std::shared_ptr<QueuedTrack> track) {
  switch (track->getState()) {
    case QueuedTrack::State::QUEUED:
      track->stepLoadMetadata(&pbTrack, &pbEpisode, processSemaphore);
      break;
    case QueuedTrack::State::KEY_REQUIRED:
      track->stepLoadAudioFile(processSemaphore);
      break;
    case QueuedTrack::State::CDN_REQUIRED:
      track->stepLoadCDNUrl(accessKey, clientToken, spclientHost);

      if (track->getState() == QueuedTrack::State::READY) {
        tryTopUpLookahead();  // TrackQueue locks + resolves + queues internally
      }
      break;
    default:
      // Do not perform any action
      break;
  }
}
