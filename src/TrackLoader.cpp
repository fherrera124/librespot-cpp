#include "TrackLoader.h"
#include <pb_decode.h>

#include "AccessKeyFetcher.h"
#include "CSpotContext.h"
#include "WrappedSemaphore.h"

using namespace cspot;

TrackLoader::TrackLoader(std::shared_ptr<cspot::Context> ctx,
                         std::shared_ptr<cspot::AccessKeyFetcher> accessKeyFetcher,
                         std::shared_ptr<bell::WrappedSemaphore> processSemaphore,
                         SnapshotFn snapshotPreloaded, TopUpFn tryTopUpLookahead)
    : bell::Task("CSpotTrackLoader", 1024 * 32, 2, 1), ctx(ctx),
      accessKeyFetcher(accessKeyFetcher), processSemaphore(processSemaphore),
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

    // Make sure we have the newest access key
    accessKey = accessKeyFetcher->getAccessKey();

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
      track->stepLoadCDNUrl(accessKey);

      if (track->getState() == QueuedTrack::State::READY) {
        tryTopUpLookahead();  // TrackQueue locks + resolves + queues internally
      }
      break;
    default:
      // Do not perform any action
      break;
  }
}
