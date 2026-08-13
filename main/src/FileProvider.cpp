#include "FileProvider.h"

#include <chrono>
#include <mutex>
#include <optional>

#include "bell/Logger.h"
#include "bell/utils/Task.h"
#include "events/EventLoop.h"
#include "events/EventModels.h"

using namespace cspot;

namespace {
// Bounds the audio key wait in taskLoop() - without it, a request the AP
// silently drops leaves the track buffering forever.
constexpr int kAudioKeyTimeoutMs = 5000;

bool countryListContains(const std::string& countryList,
                         const std::string& country) {
  for (size_t i = 0; i + 1 < countryList.size(); i += 2) {
    if (countryList[i] == country[0] && countryList[i + 1] == country[1]) {
      return true;
    }
  }
  return false;
}

// Country restriction is a oneof on the wire, so presence (which field was
// set), not emptiness, is what selects the branch. countries_allowed
// present but empty means "allowed nowhere" - true even though the string
// itself is empty. True if the given country can NOT play a track/
// alternative with these restrictions.
bool doRestrictionsApply(const std::vector<cspot_proto::Restriction>& restrictions,
                         const std::string& country) {
  if (country.empty()) {
    // AP hasn't sent its CountryCode packet yet - can't evaluate, assume
    // playable rather than block everything.
    return false;
  }
  for (auto& restriction : restrictions) {
    if (restriction.countriesAllowed.hasValue) {
      if (restriction.countriesAllowed.value.empty()) {
        return true;
      }
      return !countryListContains(restriction.countriesAllowed.value, country);
    }
    if (restriction.countriesForbidden.hasValue) {
      return countryListContains(restriction.countriesForbidden.value, country);
    }
  }
  return false;
}
}  // namespace

class DefaultFileProvider : public FileProvider, bell::Task {
 public:
  DefaultFileProvider(std::shared_ptr<EventLoop> eventLoop,
                      std::shared_ptr<SpClient> spClient,
                      std::shared_ptr<ApClient> apClient,
                      std::vector<AudioFormat> qualityPreference);

  ~DefaultFileProvider() override;

  void provideTrack(const SpotifyId& trackId) override;

  // Cancels providing a track by its ID
  void cancel(const SpotifyId& trackId) override;

 private:
  const char* LOG_TAG = "FileProvider";

  std::shared_ptr<EventLoop> eventLoop;
  std::shared_ptr<SpClient> spClient;
  std::shared_ptr<ApClient> apClient;
  // Tried in order against each track's offered AudioFile list - first
  // match wins. See Session::AudioConfig for the default.
  std::vector<AudioFormat> qualityPreference;

  std::mutex providedFilesMutex;
  bell::Semaphore providedFileSemaphore;
  std::vector<ProvidedFile> currentlyProvidedFiles;

  // Handshake between handleAudioKeyResponse() and taskLoop()'s wait for
  // it - one slot, since taskLoop() only ever awaits one track at a time.
  std::mutex audioKeyMutex;
  bell::Semaphore audioKeySemaphore;
  std::optional<AudioKeyResponse> pendingAudioKeyResponse;

  void taskLoop() override;

  void handleAudioKeyResponse(const AudioKeyResponse& response);
};

DefaultFileProvider::DefaultFileProvider(
    std::shared_ptr<EventLoop> eventLoop, std::shared_ptr<SpClient> spClient,
    std::shared_ptr<ApClient> apClient,
    std::vector<AudioFormat> qualityPreference)
    : bell::Task("cspot_file_provider", 32 * 1024, false),
      eventLoop(std::move(eventLoop)),
      spClient(std::move(spClient)),
      apClient(std::move(apClient)),
      qualityPreference(std::move(qualityPreference)) {

  startTask();

  this->eventLoop->registerHandler(
      EventLoop::EventType::AUDIO_KEY, [this](EventLoop::Event&& event) {
        auto ev = std::move(event);
        auto res = std::get<AudioKeyResponse>(ev.payload);

        BELL_LOG(info, LOG_TAG,
                 "Handling audio key response event for track ID: {}",
                 res.trackId.hexGid());
        handleAudioKeyResponse(res);
      });
}

DefaultFileProvider::~DefaultFileProvider() {
  stopTask();
}

void DefaultFileProvider::provideTrack(const SpotifyId& trackId) {
  std::scoped_lock lock(providedFilesMutex);

  ProvidedFile file = {.itemId = trackId};
  currentlyProvidedFiles.push_back(file);

  providedFileSemaphore.give();
}

void DefaultFileProvider::cancel(const SpotifyId& trackId) {
  std::scoped_lock lock(providedFilesMutex);

  auto it = std::remove_if(
      currentlyProvidedFiles.begin(), currentlyProvidedFiles.end(),
      [&trackId](const ProvidedFile& file) { return file.itemId == trackId; });

  if (it != currentlyProvidedFiles.end()) {
    currentlyProvidedFiles.erase(it, currentlyProvidedFiles.end());
  }
}

void DefaultFileProvider::taskLoop() {
  if (providedFileSemaphore.take(100)) {
    std::optional<ProvidedFile> file = std::nullopt;

    {
      std::scoped_lock lock(providedFilesMutex);
      if (currentlyProvidedFiles.empty()) {
        return;
      }

      file = currentlyProvidedFiles.front();

      currentlyProvidedFiles.erase(currentlyProvidedFiles.begin());
    }

    const std::string& countryCode = apClient->getCountryCode();
    SpotifyId effectiveTrackId = file->itemId;
    bool hasPlayableEntity = true;

    std::optional<cspot_proto::Track> trackMeta;
    std::optional<cspot_proto::Episode> episodeMeta;

    if (file->itemId.type == SpotifyIdType::Episode) {
      auto metadataRes = spClient->episodeMetadata(file->itemId);
      if (!metadataRes) {
        file->isError = true;
        BELL_LOG(info, LOG_TAG, "Could not fetch episode metadata, err={}",
                 metadataRes.error());

        eventLoop->post(EventLoop::EventType::FILE_PROVIDED, *file);
        return;
      }

      // Episodes carry no alternativeTracks list - restricted means
      // unplayable, there is nothing to fall back to.
      if (doRestrictionsApply(metadataRes->restrictions, countryCode)) {
        hasPlayableEntity = false;
      }

      episodeMeta = std::move(*metadataRes);
    } else {
      auto metadataRes = spClient->trackMetadata(file->itemId);
      if (!metadataRes) {
        file->isError = true;
        BELL_LOG(info, LOG_TAG, "Could not fetch track metadata, err={}",
                 metadataRes.error());

        eventLoop->post(EventLoop::EventType::FILE_PROVIDED, *file);
        return;
      }

      if (doRestrictionsApply(metadataRes->restrictions, countryCode)) {
        hasPlayableEntity = false;
        for (auto& alt : metadataRes->alternativeTracks) {
          if (!doRestrictionsApply(alt.restrictions, countryCode)) {
            effectiveTrackId = SpotifyId(SpotifyIdType::Track, alt.gid);
            hasPlayableEntity = true;
            break;
          }
        }
      }

      trackMeta = std::move(*metadataRes);
    }

    if (!hasPlayableEntity) {
      file->isError = true;
      BELL_LOG(info, LOG_TAG,
               "{} {} is restricted in {} with no playable alternative",
               file->itemId.type == SpotifyIdType::Episode ? "Episode" : "Track",
               file->itemId.uri, countryCode);
      eventLoop->post(EventLoop::EventType::FILE_PROVIDED, *file);
      return;
    }

    // Episodes carry their playable audio files directly in the EPISODE_V4
    // response fetched above - a separate AUDIO_FILES extended-metadata
    // request 410s for episode entities under this client's auth scope.
    std::vector<cspot_proto::AudioFile> trackAudioFiles;
    if (!episodeMeta) {
      auto filesRes = spClient->resolveAudioFiles(effectiveTrackId.uri);
      if (!filesRes) {
        file->isError = true;
        BELL_LOG(info, LOG_TAG, "Could not resolve audio files, err={}",
                 filesRes.error());
        eventLoop->post(EventLoop::EventType::FILE_PROVIDED, *file);
        return;
      }
      trackAudioFiles = std::move(*filesRes);
    }

    auto& files = episodeMeta ? episodeMeta->audioFiles : trackAudioFiles;
    // First format in qualityPreference that this track actually offers
    // wins - the LAST matching entry when a format has more than one,
    // since a duplicate's later entry is more likely to be the live one.
    auto selectedAudioFile = files.rend();
    for (AudioFormat preferred : qualityPreference) {
      selectedAudioFile =
          std::find_if(files.rbegin(), files.rend(),
                       [preferred](const cspot_proto::AudioFile& f) {
                         return f.format == preferred;
                       });
      if (selectedAudioFile != files.rend()) {
        break;
      }
    }

    if (selectedAudioFile == files.rend()) {
      file->isError = true;
      std::string formatsSeen;
      for (const auto& f : files) {
        formatsSeen += std::to_string(static_cast<int>(f.format)) + " ";
      }
      BELL_LOG(info, LOG_TAG,
               "Could not find suitable audio file, {} files available, "
               "formats: {}",
               files.size(), formatsSeen);

      eventLoop->post(EventLoop::EventType::FILE_PROVIDED, *file);
      return;
    }

    // Fired before resolveStorageInteractive() below so both round trips
    // run concurrently. reset() clears any stale signal from an abandoned
    // previous wait.
    audioKeySemaphore.reset();
    auto requestRes =
        apClient->requestAudioKey(effectiveTrackId, selectedAudioFile->fileId);
    if (!requestRes) {
      file->isError = true;
      BELL_LOG(info, LOG_TAG, "Could not request audio key, err={}",
               requestRes.error());
      eventLoop->post(EventLoop::EventType::FILE_PROVIDED, *file);
      return;
    }

    auto cdnUrlRes =
        spClient->resolveStorageInteractive(selectedAudioFile->fileId);
    if (!cdnUrlRes) {
      file->isError = true;
      BELL_LOG(info, LOG_TAG, "Could not resolve cdn url, err={}",
               cdnUrlRes.error());
      eventLoop->post(EventLoop::EventType::FILE_PROVIDED, *file);
      return;
    }

    BELL_LOG(info, LOG_TAG, "Resolved CDN url for track {}", file->itemId.uri);

    // Loop (not a single take()): a stale response for an abandoned wait
    // must not be mistaken for this track's.
    std::optional<AudioKeyResponse> audioKeyResponse;
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(kAudioKeyTimeoutMs);
    while (true) {
      auto remainingMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                             deadline - std::chrono::steady_clock::now())
                             .count();
      // take(0) means "wait forever" to bell::Semaphore - never pass 0.
      if (remainingMs <= 0 ||
          !audioKeySemaphore.take(static_cast<int>(remainingMs))) {
        break;  // timed out
      }
      std::scoped_lock lock(audioKeyMutex);
      if (pendingAudioKeyResponse &&
          pendingAudioKeyResponse->trackId == effectiveTrackId) {
        audioKeyResponse = std::move(pendingAudioKeyResponse);
        pendingAudioKeyResponse.reset();
        break;
      }
    }

    if (!audioKeyResponse) {
      file->isError = true;
      BELL_LOG(info, LOG_TAG, "Timed out waiting for audio key for track {}",
               file->itemId.uri);
      eventLoop->post(EventLoop::EventType::FILE_PROVIDED, *file);
      return;
    }

    if (!audioKeyResponse->success) {
      // An AudioKeyResponseError carries a short error code in place of
      // the key (2 bytes, not 16) - treating it as a real key regardless
      // of response.success (as this code used to) fed garbage into
      // mbedtls_aes_setkey_enc downstream, reproduced on real hardware as
      // "Failed to set AES key" retried forever for the affected track.
      file->isError = true;
      BELL_LOG(info, LOG_TAG, "Audio key request denied for track {}",
               file->itemId.uri);
      eventLoop->post(EventLoop::EventType::FILE_PROVIDED, *file);
      return;
    }

    file->cdnUrl = *cdnUrlRes;
    file->fileId = selectedAudioFile->fileId;
    file->format = selectedAudioFile->format;
    if (episodeMeta) {
      file->episodeMetadata = std::move(*episodeMeta);
    } else {
      file->trackMetadata = std::move(*trackMeta);
    }
    file->decryptionKey = audioKeyResponse->audioKey;

    BELL_LOG(info, LOG_TAG, "File ready for track {} (keyLen={})",
             file->itemId.uri, file->decryptionKey.size());

    eventLoop->post(EventLoop::EventType::FILE_PROVIDED, *file);
  }
}

void DefaultFileProvider::handleAudioKeyResponse(
    const AudioKeyResponse& response) {
  std::scoped_lock lock(audioKeyMutex);
  pendingAudioKeyResponse = response;
  audioKeySemaphore.give();
}

std::unique_ptr<FileProvider> cspot::createDefaultFileProvider(
    std::shared_ptr<EventLoop> eventLoop, std::shared_ptr<SpClient> spClient,
    std::shared_ptr<ApClient> apClient,
    std::vector<AudioFormat> qualityPreference) {
  return std::make_unique<DefaultFileProvider>(
      std::move(eventLoop), std::move(spClient), std::move(apClient),
      std::move(qualityPreference));
}
